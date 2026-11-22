#include "socket_session.hpp"
#include "framing.hpp"
#include <asio.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace p = mo::protocol;
namespace s = mo::session;
namespace n = mo::net;
namespace t = mo::transport;
using namespace std::chrono_literals;
using asio::ip::tcp;

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> std::vector<std::byte> wire(const T& message) {
    std::array<std::byte, p::max_control_packet_bytes> storage{};
    p::Writer writer(storage);
    check(p::encode_packet(writer, message), "encode failed");
    std::vector<std::byte> result;
    check(t::write_frame(result, writer.view(), p::max_control_packet_bytes), "frame failed");
    return result;
}

struct Fixture {
    asio::io_context io;
    tcp::acceptor listener{io, {asio::ip::address_v4::loopback(), 0}};
    tcp::socket client{io};
    std::unique_ptr<n::SocketSession> server;
    t::FrameAssembler responses{p::max_control_packet_bytes, 65536};
    std::vector<p::Packet> packets;
    std::optional<n::SocketResult> result;
    unsigned closes = 0;
    bool eof = false;

    explicit Fixture(n::SocketConfig config = {}) {
        n::SessionSocket accepted(asio::make_strand(asio::any_io_executor(io.get_executor())));
        client.connect(listener.local_endpoint());
        listener.accept(accepted);
        listener.close();
        server = std::make_unique<n::SocketSession>(std::move(accepted), config, p::SessionId{42}, 1,
            [this](auto value) { result = value; ++closes; });
    }
    void send(std::span<const std::byte> bytes) { asio::write(client, asio::buffer(bytes.data(), bytes.size())); }
    void receive(bool allow_reset = false) {
        client.non_blocking(true);
        std::array<std::byte, 4096> buffer{};
        for (;;) {
            std::error_code error;
            const auto size = client.read_some(asio::buffer(buffer), error);
            if (size) {
                check(responses.push(std::span(buffer).first(size)), "client receive bound");
                std::span<const std::byte> frame;
                while (responses.next_frame(frame) == t::FrameResult::frame) {
                    const auto packet = p::decode_packet(frame);
                    check(packet.has_value(), "invalid response");
                    packets.push_back(*packet);
                }
            }
            if (error == asio::error::would_block || error == asio::error::try_again) break;
            if (error == asio::error::eof) { eof = true; break; }
            // 즉시 종료는 EOF 대신 reset으로 관측될 수 있다. 응답 수신이나
            // 거절 ACK의 drain 검사에서는 허용하지 않고, EOF와도 구분한다.
            if (allow_reset && error == asio::error::connection_reset) break;
            if (error) throw std::system_error(error);
        }
        client.non_blocking(false);
    }
    template<class Predicate> void until(Predicate done, bool allow_reset = false) {
        const auto deadline = s::Clock::now() + 5s;
        while (!done() && s::Clock::now() < deadline) {
            if (io.stopped()) io.restart();
            io.run_for(1ms);
            receive(allow_reset);
        }
        check(done(), "socket session deadline exceeded");
    }
    void until_closed() {
        until([&] { return result.has_value(); }, true);
        check(closes == 1, "duplicate close notification");
    }
    void finish() {
        server->close();
        until_closed();
        io.restart(); io.poll();
        check(closes == 1, "duplicate close notification");
    }
};

void exchange(bool fragmented) {
    n::SocketConfig config;
    config.session.build_id = 7;
    if (fragmented) config.transport.receive_chunk = 1;
    Fixture fixture(config);
    fixture.server->start();
    fixture.send(wire(p::Handshake{p::wire_version, 123, 7}));
    fixture.until([&] { return fixture.packets.size() == 1; });
    const auto ack = std::get<p::HandshakeAck>(fixture.packets[0]);
    check(ack.status == p::HandshakeStatus::accepted && ack.session == p::SessionId{42}, "bad ACK");
    fixture.send(wire(p::Ping{123456}));
    fixture.until([&] { return fixture.packets.size() == 2; });
    check(std::get<p::Pong>(fixture.packets[1]).echoed_sent_at_us == 123456, "bad pong");
    fixture.finish();
    check(fixture.result->reason == s::CloseReason::local_close && fixture.result->stats.frames == 2, "close/stats");
}

void pipelined(bool budget) {
    n::SocketConfig config;
    config.transport.send_messages = budget ? 256 : 1;
    config.frames_per_turn = budget ? 1 : 32;
    config.receive_buffer_bytes = 1500;
    Fixture fixture(config);
    auto batch = wire(p::Handshake{p::wire_version, 123, 1});
    for (std::uint64_t index = 0; index < 100; ++index) {
        const auto input = wire(p::Ping{index});
        batch.insert(batch.end(), input.begin(), input.end());
    }
    // 수신 시작 전에 한 번에 보내 분할·합쳐짐과 보류 응답 다음 입력을 함께 검사한다.
    fixture.send(batch);
    fixture.server->start();
    fixture.until([&] { return fixture.packets.size() == 101; });
    check(std::get<p::HandshakeAck>(fixture.packets.front()).status == p::HandshakeStatus::accepted, "ACK missing");
    for (std::size_t i = 1; i < fixture.packets.size(); ++i)
        check(std::get<p::Pong>(fixture.packets[i]).echoed_sent_at_us == i - 1, "lost/duplicate/reordered response");
    fixture.finish();
    const auto stats = fixture.result->stats;
    check(stats.frames == 101 && stats.peak_receive_bytes <= config.receive_buffer_bytes, "receive bound/accounting");
    check(stats.peak_send_bytes <= config.transport.send_bytes, "send bound");
    check(budget ? stats.budget_yields > 0 : stats.send_blocks > 0, "budget/backpressure path not exercised");
}

void rejection() {
    for (bool version : {false, true}) {
        n::SocketConfig config;
        // 종료 기한보다 먼저 쓰기 완료 이벤트만으로 ACK와 종료가 전달되어야 한다.
        config.session.close_timeout = 30s;
        Fixture fixture(config);
        fixture.server->start();
        fixture.send(wire(p::Handshake{static_cast<std::uint16_t>(version ? 99 : p::wire_version), 0, version ? 1u : 99u}));
        fixture.until([&] { return fixture.result && fixture.eof; });
        check(fixture.packets.size() == 1, "rejection ACK dropped before close");
        const auto ack = std::get<p::HandshakeAck>(fixture.packets.front());
        check(ack.status == (version ? p::HandshakeStatus::version_mismatch : p::HandshakeStatus::build_mismatch), "rejection reason");
        check(fixture.result->reason == (version ? s::CloseReason::version_mismatch : s::CloseReason::build_mismatch), "session reason lost");
        check(!ack.session.valid() && fixture.closes == 1, "rejection lifetime");
        check(fixture.result->stats.completed_send_bytes == fixture.result->stats.accepted_send_bytes &&
              fixture.result->stats.completed_send_bytes > 0, "rejection write completion missing");
    }
}

void invalid() {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture fixture;
        fixture.server->start();
        if (mode == 0) {
            const std::array<std::byte, 4> oversized{std::byte{255}, std::byte{}, std::byte{}, std::byte{}};
            fixture.send(oversized);
        } else if (mode == 1) fixture.send(wire(p::Ping{1}));
        else {
            const std::array<std::byte, 1> fragment{std::byte{18}};
            fixture.send(fragment);
            fixture.client.shutdown(tcp::socket::shutdown_send);
        }
        fixture.until_closed();
        check(fixture.result->reason == (mode == 1 ? s::CloseReason::unexpected_message : s::CloseReason::invalid_packet), "invalid input not rejected");
        check(fixture.packets.empty(), "invalid input got response");
    }
}

void timeouts() {
    n::SocketConfig config;
    config.session.handshake_timeout = 40ms;
    Fixture handshake(config);
    handshake.server->start();
    handshake.until_closed();
    check(handshake.result->reason == s::CloseReason::handshake_timeout, "no handshake deadline");
    config.session.handshake_timeout = 2s;
    config.session.idle_timeout = 100ms;
    Fixture idle(config);
    idle.server->start();
    idle.send(wire(p::Handshake{p::wire_version, 0, 1}));
    idle.until([&] { return idle.packets.size() == 1; });
    idle.send(wire(p::Ping{3}));
    idle.until([&] { return idle.packets.size() == 2; });
    idle.until_closed();
    check(idle.result->reason == s::CloseReason::idle_timeout, "idle timer lost after refresh");
}

void shutdown() {
    asio::io_context io;
    tcp::acceptor listener(io, {asio::ip::address_v4::loopback(), 0});
    tcp::socket client(io);
    n::SessionSocket accepted(asio::make_strand(asio::any_io_executor(io.get_executor())));
    client.connect(listener.local_endpoint()); listener.accept(accepted); listener.close();
    std::atomic<unsigned> closed{0};
    std::atomic<bool> reason_ok{false};
    n::SocketSession server(std::move(accepted), {}, p::SessionId{1}, 1, [&](auto result) {
        reason_ok = result.reason == s::CloseReason::local_close; ++closed;
    });
    auto work = asio::make_work_guard(io);
    server.start();
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] { io.run(); });
    std::vector<std::jthread> closers;
    for (int i = 0; i < 6; ++i) closers.emplace_back([&] { server.close(); });
    closers.clear();
    const auto deadline = s::Clock::now() + 5s;
    while (!closed && s::Clock::now() < deadline) std::this_thread::yield();
    work.reset(); io.stop(); workers.clear();
    check(closed == 1 && reason_ok, "concurrent shutdown");
    // 공개 객체가 사라져도 예약된 종료가 안전하게 완료되어야 한다.
    Fixture lifetime;
    lifetime.server->start();
    lifetime.server.reset();
    lifetime.until_closed();
    check(lifetime.closes == 1, "destruction lost close");
}

void stalled_client() {
    asio::io_context io;
    tcp::acceptor listener(io, {asio::ip::address_v4::loopback(), 0});
    tcp::socket client(io);
    client.open(tcp::v4());
    client.set_option(asio::socket_base::receive_buffer_size(1024));
    client.connect(listener.local_endpoint());
    n::SessionSocket accepted(asio::make_strand(asio::any_io_executor(io.get_executor())));
    listener.accept(accepted); listener.close();
    accepted.set_option(asio::socket_base::send_buffer_size(1024));
    n::SocketConfig config;
    config.transport.send_messages = 1;
    config.transport.send_bytes = 22;
    config.session.handshake_timeout = 10s;
    config.session.idle_timeout = 10s;
    config.session.send_timeout = 200ms;
    std::optional<n::SocketResult> result;
    unsigned closes = 0;
    n::SocketSession server(std::move(accepted), config, p::SessionId{1}, 1,
        [&](auto value) { result = value; ++closes; });
    auto input = wire(p::Handshake{p::wire_version, 0, 1});
    const auto ping = wire(p::Ping{1});
    while (input.size() < 1024 * 1024) input.insert(input.end(), ping.begin(), ping.end());
    bool write_finished = false;
    asio::async_write(client, asio::buffer(input), [&](auto, auto) { write_finished = true; });
    server.start();
    // 읽지 않는 상대에게도 커널이 한동안 쓰기를 받아줄 수 있다. 버퍼 크기를
    // 작게 요청했어도 특정 시간 안에 송신 기한 초과가 발생한다고 가정하지 않는다.
    const auto deadline = s::Clock::now() + 1s;
    while (!result && s::Clock::now() < deadline) io.run_for(1ms);
    server.close();
    const auto close_deadline = s::Clock::now() + 5s;
    while (!result && s::Clock::now() < close_deadline) io.run_for(1ms);
    client.close();
    if (io.stopped()) io.restart();
    io.run_for(1s);
    check(write_finished && result.has_value() && closes == 1, "stalled client shutdown incomplete");
    check(result->reason == s::CloseReason::local_close || result->reason == s::CloseReason::send_timeout,
          "unexpected stalled client close reason");
    // 트래픽이 없는 상태에서 상한 검사만 통과하는 것을 막는다. 송신 제한 시간의
    // 정확한 정책은 session.timeouts에서 가상 시각으로 별도 검사한다.
    check(result->stats.frames > 1 && result->stats.accepted_send_bytes > 0 && result->stats.send_blocks > 0,
          "stalled client did not exercise traffic/backpressure");
    check(result->stats.peak_receive_bytes <= config.receive_buffer_bytes && result->stats.peak_send_bytes <= 22,
          "stalled client exceeded bounds");
}

void multithread() {
    asio::io_context io;
    tcp::acceptor listener(io, {asio::ip::address_v4::loopback(), 0});
    tcp::socket socket(io);
    n::SessionSocket accepted(asio::make_strand(asio::any_io_executor(io.get_executor())));
    socket.connect(listener.local_endpoint()); listener.accept(accepted); listener.close();
    std::atomic<unsigned> closed{0};
    std::atomic<unsigned> replies{0};
    std::atomic<bool> failed{false};
    n::SocketConfig config;
    config.transport.send_messages = 1;
    config.frames_per_turn = 3;
    n::SocketSession server(std::move(accepted), config, p::SessionId{1}, 1, [&](auto) { ++closed; });
    t::FrameAssembler responses(p::max_control_packet_bytes, 4096);
    cpp_network::Connection* client_ptr = nullptr;
    cpp_network::Connection client(std::move(socket), {65536, 256, 1024}, {
        [&](auto bytes) {
            if (!responses.push(bytes)) throw std::runtime_error("response bound");
            std::span<const std::byte> frame;
            while (responses.next_frame(frame) == t::FrameResult::frame) {
                const auto packet = p::decode_packet(frame);
                const auto index = replies.load();
                if (!packet || (index == 0 && !std::holds_alternative<p::HandshakeAck>(*packet)) ||
                    (index > 0 && (!std::holds_alternative<p::Pong>(*packet) || std::get<p::Pong>(*packet).echoed_sent_at_us != index - 1)))
                    failed = true;
                if (++replies == 101) { server.close(); client_ptr->close(); }
            }
            return true;
        }, [&](auto) { ++closed; }, {}});
    client_ptr = &client;
    auto input = wire(p::Handshake{p::wire_version, 0, 1});
    for (std::uint64_t i = 0; i < 100; ++i) {
        const auto ping = wire(p::Ping{i}); input.insert(input.end(), ping.begin(), ping.end());
    }
    check(client.send(input) == cpp_network::SendResult::accepted, "client send");
    server.start(); client.start();
    auto work = asio::make_work_guard(io);
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] { io.run(); });
    const auto deadline = s::Clock::now() + 5s;
    while (closed != 2 && s::Clock::now() < deadline) std::this_thread::yield();
    server.close(); client.close(); work.reset(); io.stop(); workers.clear();
    check(closed == 2 && replies == 101 && !failed, "multithreaded session exchange");
}
} // 익명 네임스페이스 끝

int main(int argc, char** argv) {
    try {
        const std::string name = argc == 2 ? argv[1] : "";
        if (name == "exchange") exchange(false);
        else if (name == "fragmented") exchange(true);
        else if (name == "pipelined") { pipelined(false); pipelined(true); }
        else if (name == "rejection") rejection();
        else if (name == "invalid") invalid();
        else if (name == "timeouts") timeouts();
        else if (name == "shutdown") shutdown();
        else if (name == "stalled_client") stalled_client();
        else if (name == "multithread") multithread();
        else throw std::runtime_error("unknown test case");
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

// ACK(Acknowledgment): 접속 협상 결과 응답. EOF(End Of File): 상대 송신의 끝.
// ms(Milliseconds): 밀리초. p(protocol), s(session), n(net), t(transport): 네임스페이스 별칭.
