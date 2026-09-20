#include "session.hpp"

#include <algorithm>
#include <stdexcept>

namespace mo::session {

Session::Session(Config config, protocol::SessionId id, std::uint64_t generation, Time start)
    : config_(config), id_(id), generation_(generation), start_(start), last_now_(start),
      state_deadline_(start) {
    const auto valid_duration = [](Clock::duration d) {
        return d > Clock::duration::zero() && d <= std::chrono::hours(24);
    };
    if (!id.valid() || generation == 0 || !valid_duration(config.handshake_timeout) ||
        !valid_duration(config.idle_timeout) || !valid_duration(config.send_timeout) ||
        !valid_duration(config.close_timeout)) {
        throw std::invalid_argument("Session needs live id/generation and timeouts within (0,24h]");
    }
    state_deadline_ = start + config_.handshake_timeout;
    reschedule();
}

void Session::throw_packet_size_error() {
    throw std::logic_error("Control packet no longer fits the bounded session output");
}

bool Session::can_receive() const {
    return output_size_ == 0 && (state_ == State::await_handshake || state_ == State::active);
}

std::optional<Output> Session::output() const {
    if (output_size_ == 0) {
        return std::nullopt;
    }
    return Output{{generation_, output_operation_}, std::span(output_bytes_).first(output_size_)};
}

void Session::reschedule() {
    if (state_ == State::closed) {
        timer_.reset();
        return;
    }
    auto due = state_deadline_;
    if (output_size_ != 0) {
        due = std::min(due, send_deadline_);
    }
    // 기한이 우연히 같더라도 revision을 바꿔 이전 timer 이벤트와 구분한다.
    timer_ = TimerToken{generation_, ++timer_revision_, due};
}

void Session::finish(CloseReason reason, CloseMode mode) {
    if (state_ == State::closed) {
        return;
    }
    state_ = State::closed;
    reason_ = reason;
    close_mode_ = mode;
    output_size_ = 0;
    timer_.reset();
}

bool Session::advance(Time now) {
    if (state_ == State::closed) {
        return false;
    }
    if (now < last_now_) {
        throw std::invalid_argument("Session time must be monotonic");
    }
    last_now_ = now;
    // driver 처리가 늦어 여러 기한이 이미 만료됐으면 먼저 만료된 원인을 기록한다.
    // 두 기한이 같을 때만 상태 기한을 우선한다.
    if (output_size_ != 0 && send_deadline_ < state_deadline_ && now >= send_deadline_) {
        finish(CloseReason::send_timeout, CloseMode::abort);
        return false;
    }
    if (now >= state_deadline_) {
        const auto reason = state_ == State::closing ? CloseReason::close_timeout :
            state_ == State::active ? CloseReason::idle_timeout : CloseReason::handshake_timeout;
        finish(reason, CloseMode::abort);
        return false;
    }
    return true;
}

void Session::reject(protocol::HandshakeStatus status, CloseReason reason, Time now) {
    state_ = State::closing;
    reason_ = reason;
    state_deadline_ = now + config_.close_timeout;
    queue(protocol::HandshakeAck{status, protocol::wire_version, protocol::SessionId{}}, now);
    reschedule();
}

InputResult Session::on_frame(std::uint64_t generation, std::span<const std::byte> frame, Time now) {
    if (generation != generation_ || !advance(now)) {
        return InputResult::ignored;
    }
    if (state_ == State::closing) {
        return InputResult::ignored;
    }
    if (!can_receive()) {
        return InputResult::backpressured;
    }
    const auto packet = protocol::decode_packet(frame);
    if (!packet) {
        finish(CloseReason::invalid_packet, CloseMode::abort);
        return InputResult::processed;
    }
    if (state_ == State::await_handshake) {
        const auto* hello = std::get_if<protocol::Handshake>(&*packet);
        if (!hello) {
            finish(CloseReason::unexpected_message, CloseMode::abort);
        } else if (hello->version != protocol::wire_version) {
            reject(protocol::HandshakeStatus::version_mismatch, CloseReason::version_mismatch, now);
        } else if (hello->build_id != config_.build_id) {
            reject(protocol::HandshakeStatus::build_mismatch, CloseReason::build_mismatch, now);
        } else {
            state_ = State::ack_pending;
            queue(protocol::HandshakeAck{protocol::HandshakeStatus::accepted, protocol::wire_version, id_}, now);
            reschedule();
        }
    } else {
        const auto* ping = std::get_if<protocol::Ping>(&*packet);
        if (!ping) {
            finish(CloseReason::unexpected_message, CloseMode::abort);
        } else {
            state_deadline_ = now + config_.idle_timeout;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count();
            queue(protocol::Pong{ping->sent_at_us, static_cast<std::uint64_t>(elapsed)}, now);
            reschedule();
        }
    }
    return InputResult::processed;
}

void Session::on_send_result(OutputToken token, SendResult result, Time now) {
    if (token.generation != generation_ || token.operation != output_operation_ || output_size_ == 0 ||
        !advance(now)) {
        return;
    }
    if (result == SendResult::would_block) {
        return; // 재시도할 응답 바이트, 토큰, 원래 기한을 그대로 유지한다.
    }
    if (result == SendResult::failed) {
        finish(CloseReason::transport_error, CloseMode::abort);
        return;
    }
    output_size_ = 0;
    if (state_ == State::ack_pending) {
        state_ = State::active;
        state_deadline_ = now + config_.idle_timeout;
    } else if (state_ == State::closing) {
        close_mode_ = CloseMode::drain;
    }
    reschedule();
}

void Session::on_timeout(TimerToken token, Time now) {
    if (!timer_ || token != *timer_) {
        return;
    }
    (void)advance(now);
}

void Session::on_transport_closed(std::uint64_t generation, bool error, Time now) {
    if (generation != generation_ || !advance(now)) {
        return;
    }
    finish(error ? CloseReason::transport_error : CloseReason::peer_eof, CloseMode::none);
}

void Session::on_transport_drained(std::uint64_t generation, Time now) {
    if (generation != generation_ || state_ != State::closing || close_mode_ != CloseMode::drain ||
        !advance(now)) {
        return;
    }
    finish(reason_, CloseMode::abort);
}

void Session::close(Time now) {
    if (advance(now)) {
        finish(CloseReason::local_close, CloseMode::abort);
    }
}

} // mo::session 네임스페이스 끝

// 약어 설명
// ACK(Acknowledgment): handshake 처리 결과 응답. 송신 큐의 수락은 상대의 수신 확인과 다르다.
// EOF(End Of File): 상대의 송신 종료로 입력 스트림이 끝난 상태.
// ID(Identifier): 세션이나 build를 구분하는 식별자.
