#include "socket_session.hpp"
#include "framing.hpp"
#include <asio.hpp>
#include <array>
#include <iostream>
#include <memory>
#include <vector>

namespace p = mo::protocol;
namespace n = mo::net;
namespace t = mo::transport;
using asio::ip::tcp;

template<class T> std::vector<std::byte> wire(const T& message) {
    std::array<std::byte, p::max_control_packet_bytes> storage{};
    p::Writer writer(storage);
    std::vector<std::byte> framed;
    if (!p::encode_packet(writer, message) || !t::write_frame(framed, writer.view(), p::max_control_packet_bytes))
        throw std::runtime_error("encode failed");
    return framed;
}

int main() {
    try {
        asio::io_context io;
        tcp::acceptor listener(io, {asio::ip::address_v4::loopback(), 0});
        const auto endpoint = listener.local_endpoint();
        tcp::socket client_socket(io);
        auto serial = asio::make_strand(asio::any_io_executor(io.get_executor()));
        std::unique_ptr<n::SocketSession> server;
        std::unique_ptr<cpp_network::Connection> client;
        t::FrameAssembler assembler(p::max_control_packet_bytes, 2048);
        asio::steady_timer deadline(io, std::chrono::seconds(5));
        bool success = false, failed = false;
        int closed = 0;
        auto finish = [&] { if (++closed == 2) deadline.cancel(); };
        deadline.async_wait([&](std::error_code error) {
            if (error) return;
            failed = true; listener.close(); client_socket.close();
            if (server) server->close();
            if (client) client->close();
        });
        listener.async_accept(serial, [&](std::error_code error, n::SessionSocket socket) {
            if (error) { failed = true; return; }
            listener.close();
            n::SocketConfig config; config.session.build_id = 7;
            server = std::make_unique<n::SocketSession>(std::move(socket), config, p::SessionId{42}, 1,
                [&](auto result) {
                    std::cout << "Server closed; frames=" << result.stats.frames
                              << ", received_bytes=" << result.stats.received_bytes << '\n';
                    finish();
                });
            server->start();
        });
        client_socket.async_connect(endpoint, [&](std::error_code error) {
            if (error) { failed = true; return; }
            client = std::make_unique<cpp_network::Connection>(std::move(client_socket), cpp_network::Limits{4096, 16, 64},
                cpp_network::Callbacks{[&](auto bytes) {
                    if (!assembler.push(bytes)) throw std::runtime_error("client receive limit");
                    std::span<const std::byte> frame;
                    while (assembler.next_frame(frame) == t::FrameResult::frame) {
                        const auto packet = p::decode_packet(frame);
                        if (!packet) throw std::runtime_error("invalid server packet");
                        if (const auto* ack = std::get_if<p::HandshakeAck>(&*packet)) {
                            if (ack->status != p::HandshakeStatus::accepted || ack->session != p::SessionId{42})
                                throw std::runtime_error("handshake rejected");
                            std::cout << "HandshakeAck accepted -> Ping\n";
                            if (client->send(wire(p::Ping{123456})) != cpp_network::SendResult::accepted)
                                throw std::runtime_error("ping blocked");
                        } else if (const auto* pong = std::get_if<p::Pong>(&*packet)) {
                            success = pong->echoed_sent_at_us == 123456;
                            std::cout << "Pong echo=" << pong->echoed_sent_at_us << '\n';
                            client->close();
                            if (server) server->close();
                        } else throw std::runtime_error("unexpected response");
                    }
                    return true;
                }, [&](auto) { finish(); }, {}});
            client->start();
            std::cout << "Real localhost sockets: Handshake(version=1, build=7)\n";
            if (client->send(wire(p::Handshake{p::wire_version, 0, 7})) != cpp_network::SendResult::accepted)
                throw std::runtime_error("handshake blocked");
        });
        io.run();
        return success && !failed && closed == 2 ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

// ACK(Acknowledgment): 접속 협상 처리 결과. ID(Identifier): 세션 등의 식별자.
// p(protocol), n(net), t(transport): 메시지 규약·소켓 연결·프레이밍의 네임스페이스 별칭.
