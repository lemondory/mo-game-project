#include "session.hpp"
#include "loopback.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
namespace p = mo::protocol;
namespace s = mo::session;
namespace t = mo::transport;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
const s::Time start{};
s::Config config() { return {7, 10ms, 20ms, 5ms, 4ms}; }

template <typename T>
std::vector<std::byte> bytes(const T& message) {
    std::array<std::byte, p::max_control_packet_bytes> storage{};
    p::Writer writer(storage);
    check(p::encode_packet(writer, message), "packet encode failed");
    return {writer.view().begin(), writer.view().end()};
}
auto hello() { return bytes(p::Handshake{p::wire_version, 123, 7}); }
void deliver(s::Session& session, const std::vector<std::byte>& data, s::Time now = start + 1ms) {
    check(session.on_frame(session.generation(), data, now) == s::InputResult::processed, "frame not consumed");
}
void accept(s::Session& session, s::Time now = start + 1ms) {
    check(session.output().has_value(), "missing output");
    session.on_send_result(session.output()->token, s::SendResult::accepted, now);
}
void activate(s::Session& session) {
    deliver(session, hello());
    accept(session);
    check(session.state() == s::State::active, "not active");
}

void packets() {
    const std::vector<std::vector<std::byte>> valid{
        hello(), bytes(p::HandshakeAck{p::HandshakeStatus::accepted, 1, p::SessionId{5}}),
        bytes(p::Ping{9}), bytes(p::Pong{9, 10})};
    for (const auto& packet : valid) {
        check(p::decode_packet(packet).has_value(), "complete packet rejected");
        for (std::size_t size = 0; size < packet.size(); ++size) {
            check(!p::decode_packet(std::span(packet).first(size)), "truncated packet accepted");
        }
        auto trailing = packet;
        trailing.push_back(std::byte{0});
        check(!p::decode_packet(trailing), "trailing bytes accepted");
    }
    auto unknown = hello();
    unknown[0] = std::byte{99};
    check(!p::decode_packet(unknown), "unknown id accepted");
    auto bad_enum = valid[1];
    bad_enum[2] = std::byte{200};
    check(!p::decode_packet(bad_enum), "unknown status accepted");
    s::Session session(config(), p::SessionId{5}, 1, start);
    deliver(session, unknown);
    check(session.reason() == s::CloseReason::invalid_packet, "malformed frame did not close");
}

void handshake() {
    s::Session session(config(), p::SessionId{42}, 1, start);
    deliver(session, hello());
    check(session.state() == s::State::ack_pending && !session.can_receive(), "ACK was bypassed");
    auto ack = p::decode_packet(session.output()->bytes);
    check(ack && std::holds_alternative<p::HandshakeAck>(*ack), "ACK header/body incorrect");
    check(std::get<p::HandshakeAck>(*ack).session == p::SessionId{42}, "assigned session lost");
    accept(session, start + 2ms);
    deliver(session, bytes(p::Ping{123456}), start + 3ms);
    const auto pong = p::decode_packet(session.output()->bytes);
    check(pong && std::get<p::Pong>(*pong).echoed_sent_at_us == 123456, "ping correlation lost");
    check(std::get<p::Pong>(*pong).replied_at_us == 3000, "response time is not session-relative");
    accept(session, start + 4ms);
    check(session.can_receive() && !session.output(), "output did not release");
}

void order() {
    for (const auto& data : {bytes(p::Ping{1}), bytes(p::Pong{1, 2}),
                            bytes(p::HandshakeAck{p::HandshakeStatus::accepted, 1, p::SessionId{1}})}) {
        s::Session session(config(), p::SessionId{1}, 1, start);
        deliver(session, data);
        check(session.reason() == s::CloseReason::unexpected_message, "pre-handshake message accepted");
    }
    s::Session session(config(), p::SessionId{1}, 1, start);
    activate(session);
    deliver(session, hello(), start + 2ms);
    check(session.reason() == s::CloseReason::unexpected_message, "duplicate hello accepted");
}

void rejection() {
    for (bool wrong_version : {true, false}) {
        s::Session session(config(), p::SessionId{1}, 1, start);
        deliver(session, bytes(p::Handshake{static_cast<std::uint16_t>(wrong_version ? 99 : 1),
                                            0, wrong_version ? 7u : 99u}));
        check(session.state() == s::State::closing, "rejection not pending");
        const auto ack = std::get<p::HandshakeAck>(*p::decode_packet(session.output()->bytes));
        check(!ack.session.valid(), "rejected connection got live id");
        check(ack.status == (wrong_version ? p::HandshakeStatus::version_mismatch : p::HandshakeStatus::build_mismatch),
              "wrong rejection status");
        session.on_send_result(session.output()->token, s::SendResult::would_block, start + 2ms);
        check(session.close_mode() == s::CloseMode::none && session.output(), "ACK discarded before send");
        accept(session, start + 3ms);
        check(session.state() == s::State::closing && session.close_mode() == s::CloseMode::drain,
              "acceptance mistaken for delivery");
        session.on_transport_drained(1, start + 4ms);
        check(session.state() == s::State::closed && session.close_mode() == s::CloseMode::abort, "drain did not close");
    }
    s::Session blocked(config(), p::SessionId{1}, 1, start);
    deliver(blocked, bytes(p::Handshake{99, 0, 7}));
    blocked.on_timeout(*blocked.timer(), start + 5ms);
    check(blocked.reason() == s::CloseReason::close_timeout && !blocked.output(), "close wait unbounded");
    s::Session undrained(config(), p::SessionId{1}, 2, start);
    deliver(undrained, bytes(p::Handshake{99, 0, 7}));
    accept(undrained);
    undrained.on_timeout(*undrained.timer(), start + 5ms);
    check(undrained.reason() == s::CloseReason::close_timeout, "transport drain wait unbounded");
}

void timeouts() {
    s::Session empty(config(), p::SessionId{1}, 1, start);
    check(empty.on_frame(1, hello(), start + 10ms) == s::InputResult::ignored, "deadline frame won over timeout");
    check(empty.reason() == s::CloseReason::handshake_timeout, "missing handshake timeout");
    s::Session late_ack(config(), p::SessionId{1}, 1, start);
    deliver(late_ack, hello(), start + 9ms);
    late_ack.on_send_result(late_ack.output()->token, s::SendResult::accepted, start + 10ms);
    check(late_ack.reason() == s::CloseReason::handshake_timeout, "ACK extended handshake deadline");
    s::Session blocked(config(), p::SessionId{1}, 1, start);
    activate(blocked);
    deliver(blocked, bytes(p::Ping{1}), start + 2ms);
    // 이벤트가 늦게 도착했다. 송신 기한(7ms)이 idle 기한(22ms)보다 먼저 만료됐다.
    blocked.on_timeout(*blocked.timer(), start + 30ms);
    check(blocked.reason() == s::CloseReason::send_timeout, "missing send stall timeout");
    s::Session idle(config(), p::SessionId{1}, 1, start);
    activate(idle);
    auto old_idle = *idle.timer();
    deliver(idle, bytes(p::Ping{1}), start + 20ms);
    accept(idle, start + 20ms);
    idle.on_timeout(old_idle, start + 22ms);
    check(idle.state() == s::State::active, "stale idle timer closed refreshed connection");
    idle.on_timeout(*idle.timer(), start + 40ms);
    check(idle.reason() == s::CloseReason::idle_timeout, "missing idle timeout");
}

void backpressure() {
    s::Session session(config(), p::SessionId{1}, 1, start);
    deliver(session, hello());
    const auto first = *session.output();
    const std::vector<std::byte> copy(first.bytes.begin(), first.bytes.end());
    const auto timer = *session.timer();
    for (int i = 0; i < 1000; ++i) {
        session.on_send_result(first.token, s::SendResult::would_block, start + 2ms);
        check(session.on_frame(1, bytes(p::Ping{1}), start + 2ms) == s::InputResult::backpressured,
              "pending ACK let input through");
        check(session.output()->token == first.token && *session.timer() == timer, "retry changed token/deadline");
        check(std::equal(copy.begin(), copy.end(), session.output()->bytes.begin()), "retry mutated output");
    }
    accept(session, start + 3ms);
    deliver(session, bytes(p::Ping{1}), start + 4ms);
    check(session.output()->bytes.size() <= p::max_control_packet_bytes, "unbounded session output");
}

void lifetime() {
    s::Session previous(config(), p::SessionId{1}, 1, start);
    const auto old_timer = *previous.timer();
    deliver(previous, hello());
    const auto old_output = previous.output()->token;
    s::Session current(config(), p::SessionId{1}, 2, start);
    deliver(current, hello());
    current.on_timeout(old_timer, start + 100ms);
    current.on_send_result(old_output, s::SendResult::failed, start + 100ms);
    current.on_transport_closed(1, true, start + 100ms);
    check(current.on_frame(1, hello(), start + 100ms) == s::InputResult::ignored, "old generation frame accepted");
    check(current.state() == s::State::ack_pending, "old connection affected replacement");
    const auto ack_token = current.output()->token;
    accept(current, start + 2ms);
    deliver(current, bytes(p::Ping{1}), start + 3ms);
    current.on_send_result(ack_token, s::SendResult::accepted, start + 4ms);
    check(current.output().has_value(), "duplicate completion consumed next output");
    current.close(start + 4ms);
    current.on_send_result(current.output().has_value() ? current.output()->token : ack_token,
                           s::SendResult::accepted, start + 100ms);
    current.on_timeout(old_timer, start + 100ms);
    current.on_transport_closed(2, true, start + 100ms);
    current.close(start + 100ms);
    check(current.reason() == s::CloseReason::local_close && !current.output() && !current.timer(), "close not terminal");
    for (bool error : {false, true}) {
        s::Session peer(config(), p::SessionId{1}, 1, start);
        peer.on_transport_closed(1, error, start + 1ms);
        check(peer.reason() == (error ? s::CloseReason::transport_error : s::CloseReason::peer_eof), "EOF/error conflated");
    }
    s::Session failed(config(), p::SessionId{1}, 1, start);
    deliver(failed, hello());
    failed.on_send_result(failed.output()->token, s::SendResult::failed, start + 2ms);
    check(failed.reason() == s::CloseReason::transport_error, "send failure ignored");
}

void loopback() {
    t::LoopbackConfig cfg;
    cfg.max_payload = 18; cfg.send_buffer_bytes = 22; cfg.receive_buffer_bytes = 22; cfg.chunk_bytes = 1;
    t::LoopbackLink link(cfg);
    s::Session server(config(), p::SessionId{42}, 1, start);
    check(link.first().send(hello()) == t::SendStatus::sent, "hello not queued");
    bool ack_seen = false, pong_seen = false;
    for (int step = 0; step < 200 && !pong_seen; ++step) {
        const auto now = start + std::chrono::microseconds(step);
        link.pump();
        if (server.can_receive()) {
            std::vector<std::vector<std::byte>> frames;
            link.second().receive(frames, 1);
            if (!frames.empty()) deliver(server, frames[0], now);
        }
        if (const auto out = server.output()) {
            const auto sent = link.second().send(out->bytes);
            server.on_send_result(out->token, sent == t::SendStatus::sent ? s::SendResult::accepted :
                                  sent == t::SendStatus::would_block ? s::SendResult::would_block : s::SendResult::failed, now);
        }
        std::vector<std::vector<std::byte>> frames;
        link.first().receive(frames, 1);
        if (!frames.empty()) {
            auto packet = p::decode_packet(frames[0]);
            check(packet.has_value(), "wire packet decode failed");
            if (const auto* ack = std::get_if<p::HandshakeAck>(&*packet)) {
                check(!ack_seen && ack->status == p::HandshakeStatus::accepted, "duplicate/rejected ACK");
                ack_seen = true;
                check(link.first().send(bytes(p::Ping{9876})) == t::SendStatus::sent, "ping not queued");
            } else {
                check(ack_seen && std::get<p::Pong>(*packet).echoed_sent_at_us == 9876, "wrong pong");
                pong_seen = true;
            }
        }
    }
    check(ack_seen && pong_seen && server.state() == s::State::active, "loopback exchange incomplete");
    server.close(start + 1ms);
    link.second().close();
    check(link.first().send(bytes(p::Ping{2})) == t::SendStatus::closed, "closed peer still accepts sends");
}
} // 익명 네임스페이스 끝

int main(int argc, char** argv) {
    try {
        check(argc == 2, "provide case name");
        const std::pair<const char*, void(*)()> cases[]{
            {"packets", packets}, {"handshake", handshake}, {"order", order}, {"rejection", rejection},
            {"timeouts", timeouts}, {"backpressure", backpressure}, {"lifetime", lifetime}, {"loopback", loopback}};
        for (const auto& [name, run] : cases) {
            if (std::string(argv[1]) == name) { run(); std::cout << "PASS " << name << '\n'; return 0; }
        }
        throw std::runtime_error("unknown case");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

// 약어와 코드 축약 표기
// ACK(Acknowledgment): handshake 처리 결과를 알리는 응답.
// EOF(End Of File): 상대의 송신 종료로 더 읽을 데이터가 없는 상태.
// ID(Identifier): 세션이나 메시지 종류를 구분하는 식별자.
// ms(Milliseconds): 밀리초. 1초의 1,000분의 1.
// us(Microseconds): 마이크로초. 코드에서 쓰는 표기로 1초의 1,000,000분의 1.
// cfg(Configuration): 테스트용 송수신 설정 변수.
// p(protocol), s(session), t(transport): 각 네임스페이스를 짧게 쓴 별칭.
