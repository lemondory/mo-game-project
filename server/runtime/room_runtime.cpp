#include "room_runtime.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mo {

class RoomRuntime::Impl {
    struct Envelope {
        Command command;
        RuntimeClock::time_point received_at;
    };

    // shared_queue 배치에서는 큐가 하나고, fixed_worker에서는 worker마다 하나다.
    // 두 경우 모두 하나의 스케줄링 mutex가 보호한다.
    struct ReadyQueue {
        std::vector<std::size_t> ring;
        std::size_t head{};
        std::size_t count{};
    };

    struct Room {
        std::uint64_t generation{};
        RoomConfig config{};
        std::unique_ptr<RoomLogic> logic;
        RoomSnapshot stats;
        RoomLatencies latencies;
        std::vector<Envelope> command_queue;
        std::size_t head{};
        std::size_t catch_up_streak{};
        RuntimeClock::time_point next_tick;
        RuntimeClock::time_point tick_not_before;
        RuntimeClock::time_point queued_at;
    };

public:
    explicit Impl(RuntimeConfig config) : config_(config) {
        if (config.workers == 0 || config.max_rooms == 0 || config.command_queue_capacity == 0 ||
            config.max_commands_per_slice == 0 || config.slice_budget <= RuntimeClock::duration::zero() ||
            config.max_catch_up_ticks == 0) {
            throw std::invalid_argument("Runtime limits must be positive");
        }
        rooms_.resize(config.max_rooms);
        const auto queues = config.placement == Placement::fixed_worker ? config.workers : 1;
        ready_.reserve(queues);
        for (std::size_t i = 0; i < queues; ++i) {
            ready_.push_back(std::make_unique<ReadyQueue>());
            ready_.back()->ring.resize(config.max_rooms);
        }
        workers_.reserve(config.workers);
        try {
            for (std::size_t i = 0; i < config.workers; ++i) {
                workers_.emplace_back([this, i] { worker_loop(i); });
            }
            timer_ = std::thread([this] { timer_loop(); });
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Impl() { shutdown(); }

    std::optional<RoomHandle> create(RoomConfig config, std::unique_ptr<RoomLogic> logic) {
        // 아래의 모든 시각 덧셈이 실제로 표현 가능하도록 간격에 상한을 둔다.
        const auto limit = std::chrono::hours(24);
        if (!logic || config.tick_period <= RuntimeClock::duration::zero() ||
            config.first_tick_delay < RuntimeClock::duration::zero() ||
            config.tick_period > limit || config.first_tick_delay > limit) {
            throw std::invalid_argument("Room needs logic and a period/delay within 24 hours");
        }
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < rooms_.size(); ++i) {
            auto& room = rooms_[i];
            if (room.generation != 0 && room.stats.phase != RoomPhase::closed) {
                continue;
            }
            if (room.generation == std::numeric_limits<std::uint64_t>::max()) {
                continue;
            }
            room.command_queue.resize(config_.command_queue_capacity);
            ++room.generation;
            room.config = config;
            room.logic = std::move(logic);
            room.stats = {};
            room.latencies = {};
            room.head = 0;
            room.catch_up_streak = 0;
            room.next_tick = RuntimeClock::now() + config.first_tick_delay;
            room.tick_not_before = room.next_tick;
            timer_cv_.notify_one();
            return RoomHandle{i, room.generation};
        }
        return std::nullopt;
    }

    SendResult try_send(RoomHandle handle, Command command) {
        std::lock_guard lock(mutex_);
        auto* room = find(handle);
        if (!room) {
            return SendResult::stale_handle;
        }
        if (stopping_ || room->stats.closing || room->stats.phase == RoomPhase::closed) {
            return SendResult::closed;
        }
        if (room->stats.pending == config_.command_queue_capacity) {
            ++room->stats.rejected_full;
            return SendResult::full;
        }
        const auto tail = (room->head + room->stats.pending) % config_.command_queue_capacity;
        room->command_queue[tail] = {command, RuntimeClock::now()};
        ++room->stats.pending;
        ++room->stats.accepted;
        room->stats.high_water = std::max(room->stats.high_water, room->stats.pending);
        if (room->stats.phase == RoomPhase::idle) {
            enqueue(handle.slot);
        }
        return SendResult::accepted;
    }

    bool close(RoomHandle handle) {
        std::lock_guard lock(mutex_);
        auto* room = find(handle);
        if (!room) {
            return false;
        }
        request_close(handle.slot);
        return true;
    }

    std::optional<RoomSnapshot> snapshot(RoomHandle handle) const {
        std::lock_guard lock(mutex_);
        const auto* room = find(handle);
        return room ? std::optional{room->stats} : std::nullopt;
    }

    std::optional<RoomLatencies> latencies(RoomHandle handle) const {
        std::lock_guard lock(mutex_);
        const auto* room = find(handle);
        return room ? std::optional{room->latencies} : std::nullopt;
    }

    void shutdown() {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            for (std::size_t i = 0; i < rooms_.size(); ++i) {
                if (rooms_[i].generation != 0) {
                    request_close(i);
                }
            }
        }
        timer_cv_.notify_all();
        ready_cv_.notify_all();
        if (timer_.joinable()) {
            timer_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    Room* find(RoomHandle handle) {
        return const_cast<Room*>(std::as_const(*this).find(handle));
    }

    const Room* find(RoomHandle handle) const {
        if (handle.slot >= rooms_.size() || handle.generation == 0 ||
            rooms_[handle.slot].generation != handle.generation) {
            return nullptr;
        }
        return &rooms_[handle.slot];
    }

    std::size_t queue_index(std::size_t slot) const {
        return config_.placement == Placement::fixed_worker ? slot % config_.workers : 0;
    }

    // 실행 종료 시 재확인을 포함해 모든 예약 상태 전이는 mutex_를 잡고 수행한다.
    void enqueue(std::size_t slot) {
        auto& room = rooms_[slot];
        room.stats.phase = RoomPhase::queued;
        room.queued_at = RuntimeClock::now();
        auto& queue = *ready_[queue_index(slot)];
        queue.ring[(queue.head + queue.count) % queue.ring.size()] = slot;
        ++queue.count;
        if (config_.placement == Placement::fixed_worker) {
            // 이 Room은 소유 worker만 가져갈 수 있어 정확히 깨우려면 worker마다
            // 조건 변수가 필요하다. 비교 실험용 모드라 처리량을 최적화하지 않는다.
            ready_cv_.notify_all();
        } else {
            ready_cv_.notify_one();
        }
    }

    void request_close(std::size_t slot) {
        auto& room = rooms_[slot];
        room.stats.closing = true;
        if (room.stats.phase == RoomPhase::idle) {
            enqueue(slot);
        }
    }

    static RuntimeClock::time_point tick_deadline(const Room& room) {
        return std::max(room.next_tick, room.tick_not_before);
    }

    void timer_loop() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            auto wake_at = RuntimeClock::time_point::max();
            const auto now = RuntimeClock::now();
            for (std::size_t i = 0; i < rooms_.size(); ++i) {
                const auto& room = rooms_[i];
                if (room.generation == 0 || room.stats.phase != RoomPhase::idle || room.stats.closing) {
                    continue;
                }
                const auto due = tick_deadline(room);
                if (due <= now) {
                    enqueue(i);
                } else {
                    wake_at = std::min(wake_at, due);
                }
            }
            if (wake_at == RuntimeClock::time_point::max()) {
                timer_cv_.wait(lock);
            } else {
                timer_cv_.wait_until(lock, wake_at);
            }
        }
    }

    void worker_loop(std::size_t worker_index) {
        // 공유 배치에서는 모든 worker가 하나의 큐를 비운다. 고정 배치에서는
        // 이 worker에 해시된 Room만 보게 된다.
        auto& queue = *ready_[config_.placement == Placement::fixed_worker ? worker_index : 0];
        for (;;) {
            std::size_t slot;
            {
                std::unique_lock lock(mutex_);
                ready_cv_.wait(lock, [&] { return stopping_ || queue.count != 0; });
                if (queue.count == 0) {
                    return;
                }
                slot = queue.ring[queue.head];
                queue.head = (queue.head + 1) % queue.ring.size();
                --queue.count;
                auto& room = rooms_[slot];
                room.stats.phase = RoomPhase::running;
                ++room.stats.slices;
                const auto wait = RuntimeClock::now() - room.queued_at;
                room.stats.max_ready_wait = std::max(room.stats.max_ready_wait, wait);
                room.latencies.ready_wait.record(wait);
            }
            run_slice(slot);
        }
    }

    void run_slice(std::size_t slot) {
        auto& room = rooms_[slot]; // 크기가 고정된 vector이며 logic은 이 worker만 소유한다.
        const auto start = RuntimeClock::now();
        for (std::size_t n = 0; n < config_.max_commands_per_slice; ++n) {
            if (n != 0 && RuntimeClock::now() - start >= config_.slice_budget) {
                break;
            }
            Command command;
            {
                std::lock_guard lock(mutex_);
                if (room.stats.closing || room.stats.pending == 0) {
                    break;
                }
                const auto envelope = room.command_queue[room.head];
                command = envelope.command;
                room.head = (room.head + 1) % config_.command_queue_capacity;
                --room.stats.pending;
                const auto wait = RuntimeClock::now() - envelope.received_at;
                room.stats.max_command_wait = std::max(room.stats.max_command_wait, wait);
                room.latencies.command_wait.record(wait);
            }
            bool failed = false;
            try {
                room.logic->on_command(command);
            } catch (...) {
                failed = true;
            }
            {
                std::lock_guard lock(mutex_);
                if (failed) {
                    ++room.stats.failed_commands;
                    room.stats.failed = room.stats.closing = true;
                } else {
                    ++room.stats.processed;
                }
            }
        }

        std::optional<Tick> tick;
        auto tick_start = RuntimeClock::now();
        {
            std::lock_guard lock(mutex_);
            tick_start = RuntimeClock::now();
            if (!room.stats.closing && tick_deadline(room) <= tick_start) {
                tick = Tick{room.stats.ticks + 1, room.config.tick_period, room.next_tick};
                const auto lateness = tick_start - room.next_tick;
                room.stats.max_tick_lateness = std::max(room.stats.max_tick_lateness, lateness);
                room.latencies.tick_lateness.record(lateness);
            }
        }
        if (tick) {
            bool failed = false;
            try {
                room.logic->on_tick(*tick);
            } catch (...) {
                failed = true;
            }
            const auto end = RuntimeClock::now();
            std::lock_guard lock(mutex_);
            room.stats.max_tick_duration = std::max(room.stats.max_tick_duration, end - tick_start);
            room.latencies.tick_duration.record(end - tick_start);
            if (failed) {
                room.stats.failed = room.stats.closing = true;
            } else {
                ++room.stats.ticks;
                room.next_tick += room.config.tick_period;
                if (room.next_tick <= end) {
                    if (++room.catch_up_streak >= config_.max_catch_up_ticks) {
                        room.stats.degraded = true;
                        ++room.stats.catch_up_limits;
                        room.tick_not_before = end + room.config.tick_period;
                        room.catch_up_streak = 0;
                    }
                } else {
                    room.catch_up_streak = 0;
                }
            }
        }

        std::unique_ptr<RoomLogic> retired;
        {
            std::lock_guard lock(mutex_);
            const auto slice = RuntimeClock::now() - start;
            room.stats.max_slice_duration = std::max(room.stats.max_slice_duration, slice);
            room.latencies.slice_duration.record(slice);
            if (room.stats.closing) {
                room.stats.discarded += room.stats.pending;
                room.stats.pending = 0;
                retired = std::move(room.logic);
            } else {
                // 명령 적재와 실행권을 놓을지 다시 큐에 넣을지의 판단이 같은 잠금 안에 있다.
                if (room.stats.pending != 0 || tick_deadline(room) <= RuntimeClock::now()) {
                    enqueue(slot);
                } else {
                    room.stats.phase = RoomPhase::idle;
                    timer_cv_.notify_one();
                }
                return;
            }
        }
        // 사용자 소유 객체의 소멸자를 전역 스케줄링 mutex 아래에서 실행하지 않는다.
        retired.reset();
        {
            std::lock_guard lock(mutex_);
            room.stats.phase = RoomPhase::closed;
        }
    }

    const RuntimeConfig config_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable ready_cv_;
    std::condition_variable timer_cv_;
    bool stopping_{};
    std::vector<Room> rooms_;
    std::vector<std::unique_ptr<ReadyQueue>> ready_;
    std::vector<std::thread> workers_;
    std::thread timer_;
};

RoomRuntime::RoomRuntime(RuntimeConfig config) : impl_(std::make_unique<Impl>(config)) {}
RoomRuntime::~RoomRuntime() = default;
std::optional<RoomHandle> RoomRuntime::create(RoomConfig config, std::unique_ptr<RoomLogic> logic) {
    return impl_->create(config, std::move(logic));
}
SendResult RoomRuntime::try_send(RoomHandle room, Command command) { return impl_->try_send(room, command); }
bool RoomRuntime::close(RoomHandle room) { return impl_->close(room); }
std::optional<RoomSnapshot> RoomRuntime::snapshot(RoomHandle room) const { return impl_->snapshot(room); }
std::optional<RoomLatencies> RoomRuntime::latencies(RoomHandle room) const { return impl_->latencies(room); }
void RoomRuntime::shutdown() { impl_->shutdown(); }

} // mo 네임스페이스 끝
