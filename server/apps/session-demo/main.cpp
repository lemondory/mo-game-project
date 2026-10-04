#include "session.hpp"
#include "loopback.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace p = mo::protocol;
namespace s = mo::session;
namespace t = mo::transport;

template <typename T>
void send(t::Endpoint& endpoint, const T& message) {
    std::array<std::byte, p::max_control_packet_bytes> storage{};
    p::Writer writer(storage);
    if (!p::encode_packet(writer, message) || endpoint.send(writer.view()) != t::SendStatus::sent)
        throw std::runtime_error("demo client send failed");
}

int main() {
    try {
        std::cout << "Session demo: in-memory loopback, simulated time, no socket/gameplay\n";
        t::LoopbackConfig cfg;
        cfg.chunk_bytes = 3;
        t::LoopbackLink link(cfg);
        s::Config settings;
        settings.build_id = 7;
        const s::Time start{};
        s::Session server(settings, p::SessionId{42}, 1, start);
        send(link.first(), p::Handshake{p::wire_version, 123, 7});
        std::cout << "Client -> Handshake(version=1, build=7)\n";
        bool ack_seen = false;
        for (int step = 0; step < 100; ++step) {
            const auto now = start + std::chrono::milliseconds(step);
            link.pump();
            if (server.can_receive()) {
                std::vector<std::vector<std::byte>> frames;
                link.second().receive(frames, 1);
                if (!frames.empty() && server.on_frame(1, frames[0], now) != s::InputResult::processed)
                    throw std::runtime_error("server did not consume frame");
            }
            if (auto out = server.output()) {
                const auto result = link.second().send(out->bytes);
                server.on_send_result(out->token, result == t::SendStatus::sent ? s::SendResult::accepted :
                                      result == t::SendStatus::would_block ? s::SendResult::would_block : s::SendResult::failed, now);
            }
            if (auto timer = server.timer(); timer && now >= timer->deadline) server.on_timeout(*timer, now);
            std::vector<std::vector<std::byte>> received;
            link.first().receive(received, 1);
            if (!received.empty()) {
                const auto packet = p::decode_packet(received[0]);
                if (!packet) throw std::runtime_error("invalid response");
                if (const auto* ack = std::get_if<p::HandshakeAck>(&*packet)) {
                    if (ack_seen || ack->status != p::HandshakeStatus::accepted || ack->session != p::SessionId{42})
                        throw std::runtime_error("unexpected handshake ACK");
                    ack_seen = true;
                    std::cout << "Server -> HandshakeAck(accepted, session=42); server Active\n";
                    send(link.first(), p::Ping{123456});
                    std::cout << "Client -> Ping(123456)\n";
                } else if (const auto* pong = std::get_if<p::Pong>(&*packet)) {
                    if (!ack_seen || pong->echoed_sent_at_us != 123456) throw std::runtime_error("unmatched pong");
                    std::cout << "Server -> Pong(echo=123456)\n";
                    server.close(now);
                    link.second().close();
                    std::cout << "Server Closed(local_close)\n";
                    return 0;
                } else throw std::runtime_error("unexpected response type");
            }
        }
        throw std::runtime_error("exchange did not finish");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

// 약어와 코드 축약 표기
// ACK(Acknowledgment): handshake 요청의 처리 결과를 알리는 응답.
// ID(Identifier): 세션이나 build를 구분하는 식별자.
// cfg(Configuration): 메모리 통신의 전달 크기와 버퍼 등을 설정하는 변수.
// p(protocol), s(session), t(transport): 메시지 규약, 연결 상태, 전송 계층의 네임스페이스 별칭.
