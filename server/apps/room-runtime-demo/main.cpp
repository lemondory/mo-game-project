#include "room_runtime.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {
class CounterRoom final : public mo::RoomLogic {
public:
    void on_command(const mo::Command& command) override { total_ += command.value; }
    void on_tick(const mo::Tick&) override {}
private:
    std::uint64_t total_{};
};
}

int main() {
    mo::RoomRuntime runtime;
    // 예제용 주기일 뿐이며 게임의 tick rate를 정하는 값이 아니다.
    const auto room = runtime.create({10ms, 0ms}, std::make_unique<CounterRoom>());
    if (!room) {
        return 1;
    }
    for (std::uint64_t i = 0; i < 100; ++i) {
        if (runtime.try_send(*room, {i, 1}) != mo::SendResult::accepted) {
            return 1;
        }
    }
    const auto deadline = mo::RuntimeClock::now() + 2s;
    while (mo::RuntimeClock::now() < deadline) {
        const auto stats = *runtime.snapshot(*room);
        if (stats.processed == 100 && stats.ticks >= 3) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    runtime.shutdown();
    const auto stats = *runtime.snapshot(*room);
    std::cout << "In-process runtime demo (no network/gameplay benchmark)\n"
              << "accepted=" << stats.accepted << " processed=" << stats.processed
              << " discarded=" << stats.discarded << " ticks=" << stats.ticks
              << " command_queue_high_water=" << stats.high_water << '\n';
    return stats.processed == 100 && stats.ticks >= 3 && !stats.failed ? 0 : 1;
}
