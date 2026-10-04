#pragma once

#include "packet.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace mo::session {

using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

enum class State { await_handshake, ack_pending, active, closing, closed };
enum class CloseReason {
    none, version_mismatch, build_mismatch, invalid_packet, unexpected_message,
    handshake_timeout, idle_timeout, send_timeout, close_timeout,
    local_close, peer_eof, transport_error
};
enum class CloseMode { none, drain, abort };
enum class InputResult { processed, backpressured, ignored };
enum class SendResult { accepted, would_block, failed };

struct Config {
    std::uint32_t build_id{1};
    // 실험용 기본 기한이며 서비스 보장 수치는 아니다. 버전과 build는 정확히 일치해야 한다.
    Clock::duration handshake_timeout{std::chrono::seconds(5)};
    Clock::duration idle_timeout{std::chrono::seconds(30)};
    Clock::duration send_timeout{std::chrono::seconds(5)};
    Clock::duration close_timeout{std::chrono::seconds(2)};
};

struct OutputToken {
    std::uint64_t generation{};
    std::uint64_t operation{};
    friend bool operator==(OutputToken, OutputToken) = default;
};
struct TimerToken {
    std::uint64_t generation{};
    std::uint64_t revision{};
    Time deadline{};
    friend bool operator==(TimerToken, TimerToken) = default;
};
struct Output {
    OutputToken token;
    // 다음 상태 변경 호출 전까지 빌려 쓰는 메모리다. adapter는 송신 수락 시 복사하거나 소유해야 한다.
    std::span<const std::byte> bytes;
};

// 서버의 연결 절차를 관리한다. 계정 로그인이나 게임 행동 권한을 인증하지는 않는다.
// 내부 스레드와 잠금이 없으므로 driver가 모든 호출을 직렬화하고 객체 수명을 보장한다.
// generation은 0이 아니어야 하며, 연결 slot을 재사용해도 이전 연결과 겹치면 안 된다.
class Session {
public:
    Session(Config config, protocol::SessionId id, std::uint64_t generation, Time start);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] InputResult on_frame(std::uint64_t generation, std::span<const std::byte> frame, Time now);
    void on_send_result(OutputToken token, SendResult result, Time now);
    void on_timeout(TimerToken token, Time now);
    void on_transport_closed(std::uint64_t generation, bool error, Time now);
    void on_transport_drained(std::uint64_t generation, Time now);
    void close(Time now);

    State state() const { return state_; }
    CloseReason reason() const { return reason_; }
    CloseMode close_mode() const { return close_mode_; }
    std::uint64_t generation() const { return generation_; }
    bool can_receive() const;
    std::optional<Output> output() const;
    std::optional<TimerToken> timer() const { return timer_; }

private:
    template <typename T> void queue(const T& message, Time now) {
        protocol::Writer writer(output_bytes_);
        if (!protocol::encode_packet(writer, message)) {
            throw_packet_size_error();
        }
        output_size_ = writer.written();
        ++output_operation_;
        send_deadline_ = now + config_.send_timeout;
    }
    [[noreturn]] static void throw_packet_size_error();
    bool advance(Time now);
    void reschedule();
    void finish(CloseReason reason, CloseMode mode);
    void reject(protocol::HandshakeStatus status, CloseReason reason, Time now);

    const Config config_;
    const protocol::SessionId id_;
    const std::uint64_t generation_;
    const Time start_;
    Time last_now_;
    Time state_deadline_;
    Time send_deadline_{};
    State state_{State::await_handshake};
    CloseReason reason_{CloseReason::none};
    CloseMode close_mode_{CloseMode::none};
    std::array<std::byte, protocol::max_control_packet_bytes> output_bytes_{};
    std::size_t output_size_{};
    std::uint64_t output_operation_{};
    std::uint64_t timer_revision_{};
    std::optional<TimerToken> timer_;
};

} // mo::session 네임스페이스 끝

// 약어 설명
// ACK(Acknowledgment): handshake 요청의 처리 결과를 알리는 응답. 계정 인증 성공을 뜻하지 않는다.
// EOF(End Of File): 입력 스트림의 끝. peer_eof는 상대의 송신 종료로 더 읽을 데이터가 없음을 뜻한다.
// ID(Identifier): 세션이나 build를 구분하는 식별자.
