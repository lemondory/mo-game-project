#include "socket_session.hpp"
#include "framing.hpp"
#include <asio/bind_executor.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <vector>

namespace mo::net {

struct SocketSession::Impl : std::enable_shared_from_this<Impl> {
    SerialExecutor serial;
    const SocketConfig config;
    session::Session core;
    transport::FrameAssembler frames;
    asio::steady_timer timer;
    std::unique_ptr<cpp_network::Connection> connection;
    OnClosed on_closed;
    SocketStats stats;
    std::vector<std::byte> output;
    std::atomic<bool> start_requested{false};
    std::atomic<bool> close_requested{false};
    bool finished = false;
    bool drive_posted = false;
    bool timer_waiting = false;
    bool timer_cancel_requested = false;

    Impl(SerialExecutor executor, SocketConfig settings, protocol::SessionId id,
         std::uint64_t generation, OnClosed closed)
        : serial(std::move(executor)), config(settings),
          core(config.session, id, generation, session::Clock::now()),
          frames(protocol::max_control_packet_bytes, config.receive_buffer_bytes),
          timer(serial), on_closed(std::move(closed)) {
        constexpr auto one_frame = protocol::max_control_packet_bytes + transport::length_prefix_bytes;
        // 불완전 프레임을 보관한 상태에서도 수신 청크 한 개를 전부 받을 수 있어야 한다.
        if (!config.transport.receive_chunk || config.receive_buffer_bytes < one_frame ||
            config.transport.receive_chunk > config.receive_buffer_bytes - one_frame ||
            config.transport.send_bytes < one_frame || !config.frames_per_turn ||
            config.processing_budget <= std::chrono::microseconds::zero() ||
            config.drain_probe <= std::chrono::milliseconds::zero() || !on_closed)
            throw std::invalid_argument("invalid socket session bounds or callback");
        output.reserve(one_frame);
    }

    void attach(SessionSocket socket) {
        const auto weak = weak_from_this();
        connection = std::make_unique<cpp_network::Connection>(
            asio::ip::tcp::socket(std::move(socket)), config.transport, cpp_network::Callbacks{
            [weak](auto bytes) {
                if (auto self = weak.lock()) {
                    if (self->finished) return false;
                    self->stats.received_bytes += bytes.size();
                    if (!self->frames.push(bytes)) self->invalid_frame();
                    self->stats.peak_receive_bytes = std::max(self->stats.peak_receive_bytes, self->frames.buffered());
                    self->drive();
                }
                // 다음 읽기는 drive가 제한과 상태를 확인한 뒤 resume으로 허용한다.
                return false;
            },
            [weak](auto info) {
                if (auto self = weak.lock()) self->transport_closed(info);
            },
            [weak] { if (auto self = weak.lock()) self->drive(); }});
    }

    void invalid_frame() {
        // 빈 패킷은 기존 세션의 invalid_packet 경로로 종료된다.
        (void)core.on_frame(core.generation(), {}, session::Clock::now());
    }

    bool send_output() {
        const auto pending = core.output();
        if (!pending) return true;
        output.clear();
        if (!transport::write_frame(output, pending->bytes, protocol::max_control_packet_bytes))
            throw std::logic_error("session output exceeded frame contract");
        const auto result = connection->send(output);
        auto mapped = session::SendResult::failed;
        if (result == cpp_network::SendResult::accepted) {
            mapped = session::SendResult::accepted;
            stats.accepted_send_bytes += output.size();
            stats.peak_send_bytes = std::max(stats.peak_send_bytes, connection->pending().bytes);
        } else if (result == cpp_network::SendResult::would_block) {
            mapped = session::SendResult::would_block;
            ++stats.send_blocks;
        }
        core.on_send_result(pending->token, mapped, session::Clock::now());
        return mapped != session::SendResult::would_block;
    }

    void schedule_drive() {
        if (drive_posted || finished) return;
        drive_posted = true;
        asio::post(serial, [self = shared_from_this()] {
            self->drive_posted = false;
            self->drive();
        });
    }

    void drive() {
        if (finished) return;
        if (close_requested) core.close(session::Clock::now());
        const auto begin = session::Clock::now();
        std::size_t processed = 0;
        while (core.state() != session::State::closed) {
            if (!send_output() || !core.can_receive()) break;
            if (processed >= config.frames_per_turn ||
                (processed && session::Clock::now() - begin >= config.processing_budget)) {
                if (frames.buffered()) { ++stats.budget_yields; schedule_drive(); }
                break;
            }
            std::span<const std::byte> frame;
            const auto result = frames.next_frame(frame);
            if (result == transport::FrameResult::incomplete) break;
            if (result == transport::FrameResult::too_large) { invalid_frame(); break; }
            ++stats.frames;
            ++processed;
            (void)core.on_frame(core.generation(), frame, session::Clock::now());
        }
        if (core.close_mode() == session::CloseMode::drain && connection->pending().messages == 0)
            core.on_transport_drained(core.generation(), session::Clock::now());
        if (core.state() == session::State::closed) connection->close();
        else if (!drive_posted && core.can_receive() && frames.capacity() >= config.transport.receive_chunk)
            connection->resume();
        arm_timer();
    }

    void arm_timer() {
        const auto token = core.timer();
        if (finished) {
            if (timer_waiting && !timer_cancel_requested) {
                timer_cancel_requested = true;
                timer.cancel();
            }
            return;
        }
        // 종료 취소 완료까지 이 타이머의 핸들러가 상태 수명을 유지한다.
        // 실행 문맥이 파괴되어도 핸들러 해제로 정리되며 자기 소유 순환 참조는 없다.
        auto due = token ? token->deadline : session::Time::max();
        if (core.close_mode() == session::CloseMode::drain)
            due = std::min(due, session::Clock::now() + config.drain_probe);
        if (timer_waiting) {
            // 기한 연장은 이전 타이머가 깨었을 때 반영한다. 취소 완료도 한 개만 대기한다.
            if (due < timer.expiry() && !timer_cancel_requested) {
                timer_cancel_requested = true;
                timer.cancel();
            }
            return;
        }
        timer.expires_at(due);
        timer_waiting = true;
        timer.async_wait([self = shared_from_this()](std::error_code error) {
            self->timer_waiting = false;
            self->timer_cancel_requested = false;
            if (self->finished) return;
            const auto current = self->core.timer();
            const auto now = session::Clock::now();
            if (!error && current && now >= current->deadline) self->core.on_timeout(*current, now);
            self->drive();
        });
    }

    void transport_closed(cpp_network::CloseInfo info) {
        if (finished) return;
        const bool error = info.reason == cpp_network::CloseReason::io_error ||
                           info.reason == cpp_network::CloseReason::callback_error;
        if (info.reason == cpp_network::CloseReason::peer && core.can_receive() && frames.buffered())
            invalid_frame();
        core.on_transport_closed(core.generation(), error, session::Clock::now());
        finished = true;
        timer.cancel();
        const SocketResult result{core.reason(), stats};
        auto callback = std::move(on_closed);
        callback(result);
    }
};

SocketSession::SocketSession(SessionSocket socket, SocketConfig config, protocol::SessionId id,
                             std::uint64_t generation, OnClosed closed)
    : impl_(std::make_shared<Impl>(socket.get_executor(), config, id, generation, std::move(closed))) {
    impl_->attach(std::move(socket));
}
SocketSession::~SocketSession() { close(); }
void SocketSession::start() {
    if (impl_->start_requested.exchange(true)) return;
    asio::post(impl_->serial, [self = impl_] {
        if (self->finished) return;
        self->connection->start();
        self->drive();
    });
}
void SocketSession::close() {
    if (impl_->close_requested.exchange(true)) return;
    asio::post(impl_->serial, [self = impl_] { self->drive(); });
}

} // mo::net 네임스페이스 끝

// drain: 송신 대기열과 진행 중 쓰기가 비워지기를 기다리는 처리. 원격 수신 확인은 아니다.
// resume: 중단한 읽기를 다시 허용하는 처리. weak: 소유권을 늘리지 않는 약한 참조.
