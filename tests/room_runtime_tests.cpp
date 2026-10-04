#include "room_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace mo;

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void eventually(Predicate predicate) {
    const auto deadline = RuntimeClock::now() + 5s;
    while (!predicate()) {
        check(RuntimeClock::now() < deadline, "condition timed out");
        std::this_thread::sleep_for(100us);
    }
}

// 테스트 콜백만 블로킹한다. 대기에 상한을 둬 실패한 테스트가 멈춰 있지 않게 한다.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released{};
    std::atomic<int> entered{};

    void wait() {
        ++entered;
        std::unique_lock lock(mutex);
        check(cv.wait_for(lock, 5s, [this] { return released; }), "gate timed out");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct ReleaseOnExit {
    Gate& gate;
    ~ReleaseOnExit() { gate.release(); }
};

struct Probe {
    std::atomic<int> active{};
    std::atomic<int> overlaps{};
    std::atomic<int> destroyed{};
    std::mutex mutex;
    std::vector<std::uint64_t> ids;
    std::vector<Tick> ticks;
    std::vector<RuntimeClock::time_point> tick_starts;
    std::function<void(const Command&)> command_hook;
    std::function<void(const Tick&)> tick_hook;
};

class Logic final : public RoomLogic {
    struct Active {
        Probe& probe;
        explicit Active(Probe& p) : probe(p) {
            if (probe.active.fetch_add(1) != 0) {
                ++probe.overlaps;
            }
        }
        ~Active() { --probe.active; }
    };
public:
    explicit Logic(Probe& probe) : probe_(probe) {}
    ~Logic() override {
        if (probe_.active != 0) {
            ++probe_.overlaps;
        }
        ++probe_.destroyed;
    }
    void on_command(const Command& command) override {
        Active guard(probe_);
        if (probe_.command_hook) {
            probe_.command_hook(command);
        }
        std::lock_guard lock(probe_.mutex);
        probe_.ids.push_back(command.id);
    }
    void on_tick(const Tick& tick) override {
        Active guard(probe_);
        {
            std::lock_guard lock(probe_.mutex);
            probe_.ticks.push_back(tick);
            probe_.tick_starts.push_back(RuntimeClock::now());
        }
        if (probe_.tick_hook) {
            probe_.tick_hook(tick);
        }
    }
private:
    Probe& probe_;
};

RoomHandle add(RoomRuntime& runtime, Probe& probe, RoomConfig config = {1h, 1h}) {
    const auto handle = runtime.create(config, std::make_unique<Logic>(probe));
    check(handle.has_value(), "room creation failed");
    return *handle;
}

void accounting(const RoomSnapshot& snapshot) {
    check(snapshot.phase == RoomPhase::closed, "room not closed");
    check(snapshot.pending == 0, "pending commands after close");
    check(snapshot.accepted == snapshot.processed + snapshot.discarded + snapshot.failed_commands,
          "accepted command accounting mismatch");
}

void producers() {
    Probe probe;
    RuntimeConfig config;
    config.workers = 4;
    config.command_queue_capacity = 31;
    config.max_commands_per_slice = 7;
    RoomRuntime runtime(config);
    const auto room = add(runtime, probe, {1ms, 0ms});
    constexpr std::size_t count = 12000;
    std::atomic<bool> unexpected{};
    std::vector<std::jthread> senders;
    for (std::size_t p = 0; p < 6; ++p) {
        senders.emplace_back([&, p] {
            const auto deadline = RuntimeClock::now() + 5s;
            for (std::size_t i = p; i < count; i += 6) {
                for (;;) {
                    const auto result = runtime.try_send(room, {i, 1});
                    if (result == SendResult::accepted) {
                        break;
                    }
                    if (result != SendResult::full || RuntimeClock::now() >= deadline) {
                        unexpected = true;
                        return;
                    }
                    std::this_thread::yield(); // 테스트에서만 쓰는, 상한이 있는 재시도.
                }
            }
        });
    }
    senders.clear();
    check(!unexpected, "producer unexpectedly rejected/timed out");
    eventually([&] { return runtime.snapshot(room)->processed == count; });
    eventually([&] { return runtime.snapshot(room)->ticks >= 2; });
    runtime.shutdown();
    const auto stats = *runtime.snapshot(room);
    accounting(stats);
    check(stats.high_water <= config.command_queue_capacity, "command queue bound exceeded");
    check(stats.accepted == count && stats.discarded == 0, "lost accepted command");
    check(probe.overlaps == 0 && probe.destroyed == 1, "logic overlap/lifetime failure");
    std::sort(probe.ids.begin(), probe.ids.end());
    check(probe.ids.size() == count, "wrong command count");
    for (std::size_t i = 0; i < count; ++i) {
        check(probe.ids[i] == i, "duplicate or missing command");
    }
}

void wakeup() {
    Probe probe;
    RuntimeConfig config;
    config.max_commands_per_slice = 1;
    config.workers = 4;
    std::atomic<std::uint64_t> entered{};
    RoomRuntime runtime(config);
    probe.command_hook = [&](const Command& command) { entered.store(command.id + 1); };
    const auto room = add(runtime, probe);
    // 짧은 sleep을 3,000번 반복하면 플랫폼의 타이머 해상도 때문에 대기가 누적된다.
    // 이 경합 검사만 실행권을 양보하며 재확인한다. 진행 유실은 전체 10초 기한으로 잡는다.
    const auto deadline = RuntimeClock::now() + 10s;
    const auto await_progress = [&](auto predicate) {
        while (!predicate()) {
            check(RuntimeClock::now() < deadline, "wakeup progress timed out");
            std::this_thread::yield();
        }
    };
    for (std::uint64_t i = 0; i < 3000; ++i) {
        check(runtime.try_send(room, {i, 0}) == SendResult::accepted, "wakeup enqueue failed");
        // 이전 콜백이 아직 끝나는 중일 수 있는 시점에 다음 명령을 넣는다.
        await_progress([&] { return entered.load() == i + 1; });
        if (i % 3 == 0) {
            await_progress([&] { return runtime.snapshot(room)->phase == RoomPhase::idle; });
        }
    }
    await_progress([&] { return runtime.snapshot(room)->processed == 3000; });
    runtime.shutdown();
    accounting(*runtime.snapshot(room));
    check(probe.overlaps == 0, "same room ran concurrently");
}

void parallel_rooms() {
    Probe a, b;
    Gate gate;
    a.command_hook = b.command_hook = [&](const Command&) { gate.wait(); };
    RoomRuntime runtime;
    ReleaseOnExit release{gate};
    const auto first = add(runtime, a);
    const auto second = add(runtime, b);
    check(runtime.try_send(first, {1, 0}) == SendResult::accepted, "first send");
    check(runtime.try_send(second, {2, 0}) == SendResult::accepted, "second send");
    eventually([&] { return gate.entered == 2; });
    check(a.active == 1 && b.active == 1, "rooms did not overlap");
    gate.release();
    eventually([&] { return runtime.snapshot(first)->processed + runtime.snapshot(second)->processed == 2; });
    runtime.shutdown();
    check(a.overlaps == 0 && b.overlaps == 0, "room serialization failed");
}

void fairness() {
    Probe hot, cold;
    Gate gate;
    std::atomic<std::size_t> hot_seen{};
    std::atomic<std::size_t> hot_at_cold{999};
    hot.command_hook = [&](const Command& command) {
        if (command.id == 0) {
            gate.wait();
        }
        ++hot_seen;
    };
    cold.command_hook = [&](const Command&) { hot_at_cold = hot_seen.load(); };
    RuntimeConfig config;
    config.workers = 1;
    config.max_commands_per_slice = 3;
    RoomRuntime runtime(config);
    ReleaseOnExit release{gate};
    const auto h = add(runtime, hot);
    const auto c = add(runtime, cold);
    runtime.try_send(h, {0, 0});
    eventually([&] { return gate.entered == 1; });
    for (std::uint64_t i = 1; i < 40; ++i) {
        check(runtime.try_send(h, {i, 0}) == SendResult::accepted, "hot send");
    }
    runtime.try_send(c, {0, 0});
    gate.release();
    eventually([&] { return runtime.snapshot(c)->processed == 1; });
    check(hot_at_cold >= 1 && hot_at_cold <= 3, "hot room monopolized worker");
    eventually([&] { return runtime.snapshot(h)->processed == 40; });
    runtime.shutdown();
}

void lifecycle() {
    Probe old_probe, new_probe;
    Gate gate;
    old_probe.command_hook = [&](const Command&) { gate.wait(); };
    RuntimeConfig config;
    config.max_rooms = 1;
    config.command_queue_capacity = 4;
    RoomRuntime runtime(config);
    ReleaseOnExit release{gate};
    const auto old = add(runtime, old_probe, {1ms, 1h});
    runtime.try_send(old, {0, 0});
    eventually([&] { return gate.entered == 1; });
    check(!runtime.create({1h, 1h}, std::make_unique<Logic>(new_probe)), "room capacity exceeded");
    for (std::uint64_t i = 1; i <= 4; ++i) {
        check(runtime.try_send(old, {i, 0}) == SendResult::accepted, "bounded enqueue failed");
    }
    check(runtime.try_send(old, {5, 0}) == SendResult::full, "overflow not rejected");
    check(runtime.close(old) && runtime.close(old), "close not idempotent");
    check(runtime.try_send(old, {6, 0}) == SendResult::closed, "send after close accepted");
    check(old_probe.destroyed == 0, "logic destroyed while callback running");
    gate.release();
    eventually([&] { return runtime.snapshot(old)->phase == RoomPhase::closed; });
    const auto stats = *runtime.snapshot(old);
    accounting(stats);
    check(stats.processed == 1 && stats.discarded == 4 && stats.rejected_full == 1, "close accounting");
    check(old_probe.destroyed == 1 && old_probe.overlaps == 0, "close lifetime");
    const auto replacement = add(runtime, new_probe, {1ms, 0ms});
    check(replacement.slot == old.slot && replacement.generation != old.generation, "generation not advanced");
    check(runtime.try_send(old, {7, 0}) == SendResult::stale_handle, "stale callback accepted");
    check(!runtime.close(old) && !runtime.snapshot(old), "stale handle affected new room");
    eventually([&] { return runtime.snapshot(replacement)->ticks >= 2; });
    runtime.shutdown();
    accounting(*runtime.snapshot(replacement));
}

void shutdown_race() {
    Probe probe;
    Gate gate;
    probe.command_hook = [&](const Command&) { gate.wait(); };
    RoomRuntime runtime;
    ReleaseOnExit release{gate};
    const auto room = add(runtime, probe);
    runtime.try_send(room, {0, 0});
    eventually([&] { return gate.entered == 1; });
    std::atomic<std::uint64_t> accepted{1};
    std::atomic<bool> bad_result{};
    std::barrier start{6};
    std::vector<std::jthread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (int n = 0; n < 3000; ++n) {
                const auto result = runtime.try_send(room, {1, 0});
                if (result == SendResult::accepted) {
                    ++accepted;
                } else if (result == SendResult::closed) {
                    return;
                } else if (result != SendResult::full) {
                    bad_result = true;
                }
            }
        });
    }
    std::jthread stop_a([&] { start.arrive_and_wait(); runtime.shutdown(); });
    std::jthread stop_b([&] { start.arrive_and_wait(); runtime.shutdown(); });
    eventually([&] { return runtime.snapshot(room)->closing; });
    check(runtime.try_send(room, {2, 0}) == SendResult::closed, "shutdown accepted new command");
    gate.release();
    producers.clear();
    stop_a.join();
    stop_b.join();
    const auto stats = *runtime.snapshot(room);
    accounting(stats);
    check(!bad_result && stats.accepted == accepted, "shutdown producer accounting");
    check(stats.processed == 1 && probe.destroyed == 1, "shutdown ran queued work");
    check(!runtime.create({1h, 1h}, std::make_unique<Logic>(probe)), "created after shutdown");
    runtime.shutdown();
}

void ticks() {
    Probe probe;
    Gate gate;
    probe.tick_hook = [&](const Tick& tick) {
        if (tick.number == 1) {
            gate.wait();
        }
    };
    RuntimeConfig config;
    config.command_queue_capacity = 4;
    config.max_catch_up_ticks = 1;
    RoomRuntime runtime(config);
    ReleaseOnExit release{gate};
    const auto room = add(runtime, probe, {2ms, 0ms});
    eventually([&] { return gate.entered == 1; });
    for (std::uint64_t i = 0; i < 4; ++i) {
        check(runtime.try_send(room, {i, 0}) == SendResult::accepted, "tick-path enqueue");
    }
    check(runtime.try_send(room, {4, 0}) == SendResult::full, "tick-path queue overflow");
    std::this_thread::sleep_for(15ms); // 일부러 tick 부채를 만든다. 지연을 단언하는 것이 아니다.
    gate.release();
    eventually([&] { return runtime.snapshot(room)->ticks >= 5; });
    eventually([&] { return runtime.snapshot(room)->processed == 4; });
    runtime.shutdown();
    const auto stats = *runtime.snapshot(room);
    accounting(stats);
    check(stats.degraded && stats.catch_up_limits >= 1, "tick debt not bounded");
    check(probe.overlaps == 0, "command/tick overlap");
    for (std::size_t i = 0; i < probe.ticks.size(); ++i) {
        check(probe.ticks[i].number == i + 1 && probe.ticks[i].dt == 2ms, "tick skipped or dt changed");
        if (i != 0) {
            check(probe.ticks[i].scheduled_at - probe.ticks[i - 1].scheduled_at == 2ms, "deadline drift");
        }
    }
    check(probe.tick_starts[1] - probe.tick_starts[0] >= 17ms, "catch-up cooldown ignored");
}

void tick_close() {
    Probe probe;
    Gate gate;
    probe.tick_hook = [&](const Tick&) { gate.wait(); };
    RuntimeConfig config;
    config.command_queue_capacity = 2;
    RoomRuntime runtime(config);
    ReleaseOnExit release{gate};
    const auto room = add(runtime, probe, {1ms, 0ms});
    eventually([&] { return gate.entered == 1; });
    runtime.try_send(room, {0, 0});
    runtime.try_send(room, {1, 0});
    check(runtime.try_send(room, {2, 0}) == SendResult::full, "tick-close overflow");
    check(runtime.close(room), "tick-close rejected");
    std::this_thread::sleep_for(3ms); // 종료하는 동안 tick 기한이 지나게 한다.
    check(probe.destroyed == 0, "destroyed during tick");
    gate.release();
    runtime.shutdown();
    const auto stats = *runtime.snapshot(room);
    accounting(stats);
    check(stats.ticks == 1 && stats.processed == 0 && stats.discarded == 2, "work started after tick close");
    check(probe.overlaps == 0 && probe.destroyed == 1, "tick-close lifetime failure");
}

void exceptions() {
    Probe broken, healthy, tick_broken;
    Gate gate;
    broken.command_hook = [&](const Command&) {
        gate.wait();
        throw std::runtime_error("injected command failure");
    };
    tick_broken.tick_hook = [](const Tick&) { throw std::runtime_error("injected tick failure"); };
    RoomRuntime runtime;
    ReleaseOnExit release{gate};
    const auto bad = add(runtime, broken);
    const auto good = add(runtime, healthy);
    const auto bad_tick = add(runtime, tick_broken, {1ms, 0ms});
    runtime.try_send(bad, {0, 0});
    eventually([&] { return gate.entered == 1; });
    runtime.try_send(bad, {1, 0});
    runtime.try_send(good, {0, 0});
    gate.release();
    eventually([&] { return runtime.snapshot(bad)->phase == RoomPhase::closed; });
    eventually([&] { return runtime.snapshot(bad_tick)->phase == RoomPhase::closed; });
    eventually([&] { return runtime.snapshot(good)->processed == 1; });
    const auto stats = *runtime.snapshot(bad);
    accounting(stats);
    check(stats.failed && stats.failed_commands == 1 && stats.discarded == 1, "exception accounting");
    check(runtime.snapshot(bad_tick)->failed, "tick exception not isolated");
    check(!runtime.snapshot(good)->failed, "failure leaked to another room");
    runtime.shutdown();
}

// 보고용 히스토그램의 버킷 배치와 분위수 계약을 확인한다.
void histogram() {
    Histogram empty;
    check(empty.count() == 0 && empty.percentile_us(99.0) == 0 && empty.max_us() == 0,
          "empty histogram must report zeros");

    Histogram linear;
    for (std::uint64_t us = 0; us < 32; ++us) {
        linear.record_us(us);
    }
    check(linear.count() == 32, "linear sample count");
    check(linear.percentile_us(50.0) == 15 && linear.percentile_us(100.0) == 31,
          "1 us resolution below the linear limit");

    // 선형 한계 위에서 분위수는 버킷의 상한이다. 표본보다 작지 않고 오차는 6.25%
    // 이내다.
    Histogram wide;
    const std::uint64_t samples[] = {32, 100, 1'000, 16'384, 250'000, 5'000'000};
    for (auto value : samples) {
        wide.record_us(value);
        const auto bound = histogram_layout::bucket_upper_us(histogram_layout::bucket_index(value));
        check(bound >= value, "bucket upper bound below its sample");
        check(bound - value <= value / 16 + 1, "bucket wider than 6.25%");
    }
    check(wide.count() == 6 && wide.max_us() == 5'000'000, "wide sample bookkeeping");
    check(wide.percentile_us(100.0) == 5'000'000, "top percentile is clamped to the real max");
    check(wide.min_us() == 32, "minimum tracked");

    Histogram over;
    over.record_us(Histogram::max_trackable_us * 2);
    check(over.overflows() == 1 && over.count() == 1, "above-range samples must be counted");

    Histogram merged;
    merged.merge(linear);
    merged.merge(wide);
    check(merged.count() == 38 && merged.max_us() == 5'000'000 && merged.min_us() == 0,
          "merge keeps counts and extremes");
    check(merged.percentile_us(99.9) == 5'000'000, "merged tail percentile");

    // 모든 버킷 번호는 자기 구간 이상의 상한으로 되돌아와야 한다.
    for (std::size_t i = 0; i < Histogram::bucket_count; ++i) {
        const auto upper = mo::histogram_layout::bucket_upper_us(i);
        check(mo::histogram_layout::bucket_index(upper) == i, "bucket bound outside its bucket");
    }
}

// runtime은 집계하는 구간마다 지연 표본을 정확히 하나씩 기록해야 한다.
void latencies() {
    Probe probe;
    RuntimeConfig config;
    config.workers = 2;
    config.max_commands_per_slice = 4;
    RoomRuntime runtime(config);
    const auto room = add(runtime, probe, {2ms, 0ms});
    constexpr std::uint64_t count = 200;
    for (std::uint64_t i = 0; i < count; ++i) {
        eventually([&] { return runtime.try_send(room, {i, 1}) == SendResult::accepted; });
    }
    eventually([&] { return runtime.snapshot(room)->processed == count; });
    eventually([&] { return runtime.snapshot(room)->ticks >= 3; });
    runtime.shutdown();

    const auto stats = *runtime.snapshot(room);
    const auto measured = *runtime.latencies(room);
    accounting(stats);
    check(measured.command_wait.count() == stats.processed + stats.failed_commands,
          "one command_wait sample per executed command");
    check(measured.tick_lateness.count() == stats.ticks, "one lateness sample per tick");
    check(measured.tick_duration.count() == stats.ticks, "one duration sample per tick");
    check(measured.ready_wait.count() == stats.slices, "one ready_wait sample per slice");
    check(measured.slice_duration.count() == stats.slices, "one slice sample per slice");
    check(measured.tick_lateness.percentile_us(100.0) <=
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                  stats.max_tick_lateness).count()) + 1,
          "percentile above the recorded maximum");
    check(runtime.latencies({room.slot, room.generation + 1}) == std::nullopt,
          "stale handle must not return measurements");
}

// fixed_worker 배치는 비교용 모델이다. Room은 자기 worker에서만 실행되므로, 막힌
// worker는 거기에 해시된 모든 Room을 함께 세운다.
void placement() {
    Probe blocking, neighbour, other;
    Gate gate;
    blocking.command_hook = [&](const Command&) { gate.wait(); };
    RuntimeConfig config;
    config.workers = 2;
    config.placement = Placement::fixed_worker;
    RoomRuntime runtime(config);
    ReleaseOnExit release{gate};
    const auto slot0 = add(runtime, blocking);   // worker 0
    const auto slot1 = add(runtime, neighbour);  // worker 1
    const auto slot2 = add(runtime, other);      // 다시 worker 0
    check(slot0.slot == 0 && slot1.slot == 1 && slot2.slot == 2, "unexpected slot assignment");

    runtime.try_send(slot0, {0, 0});
    eventually([&] { return gate.entered == 1; });
    runtime.try_send(slot1, {0, 0});
    runtime.try_send(slot2, {0, 0});
    eventually([&] { return runtime.snapshot(slot1)->processed == 1; });
    // Room 2는 막힌 Room과 worker 0을 공유하므로 다른 worker가 가져갈 수 없다.
    std::this_thread::sleep_for(100ms);
    check(runtime.snapshot(slot2)->processed == 0, "fixed placement allowed work stealing");
    gate.release();
    eventually([&] { return runtime.snapshot(slot2)->processed == 1; });
    check(runtime.latencies(slot2)->ready_wait.max_us() >= 50'000,
          "head-of-line blocking not visible in ready_wait");
    runtime.shutdown();
    check(blocking.overlaps == 0 && other.overlaps == 0, "placement broke single execution");
}

} // 익명 네임스페이스 끝

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"producers", producers}, {"wakeup", wakeup}, {"parallel", parallel_rooms},
        {"fairness", fairness}, {"lifecycle", lifecycle}, {"shutdown", shutdown_race},
        {"ticks", ticks}, {"tick_close", tick_close}, {"exceptions", exceptions},
        {"histogram", histogram}, {"latencies", latencies}, {"placement", placement}};
    try {
        check(argc == 2, "provide a test case name");
        for (const auto& [name, test] : cases) {
            if (name == argv[1]) {
                test();
                std::cout << "PASS " << name << '\n';
                return 0;
            }
        }
        throw std::runtime_error("unknown test case");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
