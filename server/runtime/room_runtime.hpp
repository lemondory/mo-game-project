#pragma once

#include "latency_histogram.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace mo {

using RuntimeClock = std::chrono::steady_clock;

struct RoomHandle {
    std::size_t slot{};
    std::uint64_t generation{};
};

// 합성 고정 크기 명령이다. 아직 전송 형식도 게임 입력 규약도 아니다.
struct Command {
    std::uint64_t id{};
    std::uint64_t value{};
};

struct Tick {
    std::uint64_t number{};
    RuntimeClock::duration dt{};
    RuntimeClock::time_point scheduled_at{};
};

class RoomLogic {
public:
    virtual ~RoomLogic() = default;
    virtual void on_command(const Command& command) = 0;
    virtual void on_tick(const Tick& tick) = 0;
};

// shared_queue가 이 프로젝트의 실행 모델이다. 어떤 worker든 유휴 Room을 실행한다.
// fixed_worker는 Room을 hash(slot) % workers에 고정하며 비교 실험에만 쓴다.
// head-of-line blocking을 일부러 재현하는 모드이고 운영용 선택지가 아니다.
enum class Placement { shared_queue, fixed_worker };

struct RuntimeConfig {
    std::size_t workers{2};
    std::size_t max_rooms{64};
    std::size_t command_queue_capacity{256};
    std::size_t max_commands_per_slice{32};
    RuntimeClock::duration slice_budget{std::chrono::milliseconds(2)};
    std::size_t max_catch_up_ticks{2};
    Placement placement{Placement::shared_queue};
};

struct RoomConfig {
    RuntimeClock::duration tick_period;
    RuntimeClock::duration first_tick_delay;
};

enum class SendResult { accepted, full, closed, stale_handle };
enum class RoomPhase { idle, queued, running, closed };

struct RoomSnapshot {
    RoomPhase phase{RoomPhase::idle};
    bool closing{};
    bool failed{};
    bool degraded{};
    std::size_t pending{};
    std::size_t high_water{};
    std::uint64_t accepted{};
    std::uint64_t processed{};
    std::uint64_t discarded{};
    std::uint64_t failed_commands{};
    std::uint64_t rejected_full{};
    std::uint64_t ticks{};
    std::uint64_t slices{};
    std::uint64_t catch_up_limits{};
    RuntimeClock::duration max_ready_wait{};
    RuntimeClock::duration max_command_wait{};
    RuntimeClock::duration max_tick_lateness{};
    RuntimeClock::duration max_tick_duration{};
    RuntimeClock::duration max_slice_duration{};
};

// snapshot이 최댓값으로 세는 것과 같은 구간을 Room별 분포로 기록한다.
// 크기가 고정이며(Room당 약 15KB) slot을 재사용할 때 초기화한다.
// 이 값을 복사하는 것은 진단·보고용 호출이며 tick마다 부르는 API가 아니다.
struct RoomLatencies {
    Histogram ready_wait;      // ready 큐에 들어간 뒤 실행이 시작될 때까지
    Histogram command_wait;    // 명령 큐에 수락된 뒤 실행이 시작될 때까지
    Histogram tick_lateness;   // 예정된 tick 기한부터 tick이 시작될 때까지
    Histogram tick_duration;   // on_tick 안에서 보낸 벽시계 시간
    Histogram slice_duration;  // 한 번의 실행 전체가 걸린 벽시계 시간
};

// 제어 API는 스레드 안전하다. logic은 블로킹하거나 shutdown()을 부르면 안 된다.
// 외부 API 호출자가 모두 멈춘 뒤에만 이 객체를 소멸시킨다.
class RoomRuntime {
public:
    explicit RoomRuntime(RuntimeConfig config = {});
    ~RoomRuntime();
    RoomRuntime(const RoomRuntime&) = delete;
    RoomRuntime& operator=(const RoomRuntime&) = delete;

    // 용량이 없거나 종료 중이면 nullopt를 반환한다. 잘못된 설정은 예외를 던진다.
    std::optional<RoomHandle> create(RoomConfig config, std::unique_ptr<RoomLogic> logic);
    SendResult try_send(RoomHandle room, Command command);
    bool close(RoomHandle room);
    std::optional<RoomSnapshot> snapshot(RoomHandle room) const;
    std::optional<RoomLatencies> latencies(RoomHandle room) const;
    // 대기 중인 명령을 취소하고, 진행 중인 작업을 끝낸 뒤 모든 스레드를 join한다.
    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // mo 네임스페이스 끝
