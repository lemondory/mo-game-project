// 합성 런타임 측정 하네스다.
//
// CPU 비용 모델과 열린 루프 입력 일정으로 프로세스 내부의 Room runtime을 돌린 뒤,
// Room 그룹별 지연 분포를 보고한다. 게임 플레이 벤치마크도, 네트워크 측정도, 서비스
// 수용량 결과도 아니다. transport도 직렬화도 DB도 실제 전투 부하도 없다.
// 목적은 같은 계약 위에서 실행 모델(공유 큐와 고정 배치)과 부하 모양(균일과 편향)을
// 비교하는 것이다.

#include "room_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 합성 작업의 결과를 관측 가능하게 남겨 최적화가 반복문을 지우지 못하게 한다.
std::atomic<std::uint64_t> g_sink{0};

struct Options {
    std::size_t rooms{16};
    std::size_t workers{4};
    std::size_t generators{2};
    std::size_t duration_ms{3000};
    std::size_t tick_hz{60};
    std::size_t input_hz{30};
    std::size_t tick_cost_us{200};
    std::size_t command_cost_us{20};
    std::size_t hot_rooms{0};
    std::size_t hot_multiplier{10};
    bool hot_collide{false};
    std::size_t command_queue{256};
    std::size_t slice_commands{32};
    std::size_t slice_budget_us{2000};
    mo::Placement placement{mo::Placement::shared_queue};
    std::string json_path;
};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::size_t to_size(const std::string& flag, const std::string& text) {
    try {
        const auto value = std::stoll(text);
        if (value < 0) {
            fail(flag + " must not be negative");
        }
        return static_cast<std::size_t>(value);
    } catch (const std::invalid_argument&) {
        fail(flag + " needs a number");
    } catch (const std::out_of_range&) {
        fail(flag + " is out of range");
    }
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") {
            std::cout <<
                "room-runtime-bench: synthetic Room runtime measurement (no network, no gameplay)\n"
                "  --rooms N --workers N --generators N --duration-ms N\n"
                "  --tick-hz N --input-hz N --tick-cost-us N --command-cost-us N\n"
                "  --hot-rooms N --hot-multiplier N --hot-layout spread|collide\n"
                "  --command-queue N --slice-commands N --slice-budget-us N\n"
                "  --placement shared|fixed --json PATH\n";
            std::exit(0);
        }
        if (i + 1 >= argc) {
            fail(flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (flag == "--rooms") { options.rooms = to_size(flag, value); }
        else if (flag == "--workers") { options.workers = to_size(flag, value); }
        else if (flag == "--generators") { options.generators = to_size(flag, value); }
        else if (flag == "--duration-ms") { options.duration_ms = to_size(flag, value); }
        else if (flag == "--tick-hz") { options.tick_hz = to_size(flag, value); }
        else if (flag == "--input-hz") { options.input_hz = to_size(flag, value); }
        else if (flag == "--tick-cost-us") { options.tick_cost_us = to_size(flag, value); }
        else if (flag == "--command-cost-us") { options.command_cost_us = to_size(flag, value); }
        else if (flag == "--hot-rooms") { options.hot_rooms = to_size(flag, value); }
        else if (flag == "--hot-multiplier") { options.hot_multiplier = to_size(flag, value); }
        else if (flag == "--command-queue") { options.command_queue = to_size(flag, value); }
        else if (flag == "--slice-commands") { options.slice_commands = to_size(flag, value); }
        else if (flag == "--slice-budget-us") { options.slice_budget_us = to_size(flag, value); }
        else if (flag == "--json") { options.json_path = value; }
        else if (flag == "--hot-layout") {
            if (value == "spread") {
                options.hot_collide = false;
            } else if (value == "collide") {
                options.hot_collide = true;
            } else {
                fail("--hot-layout must be spread or collide");
            }
        }
        else if (flag == "--placement") {
            if (value == "shared") {
                options.placement = mo::Placement::shared_queue;
            } else if (value == "fixed") {
                options.placement = mo::Placement::fixed_worker;
            } else {
                fail("--placement must be shared or fixed");
            }
        } else {
            fail("unknown flag " + flag);
        }
    }
    if (options.rooms == 0 || options.workers == 0 || options.generators == 0 ||
        options.tick_hz == 0 || options.input_hz == 0 || options.duration_ms == 0) {
        fail("rooms, workers, generators, tick-hz, input-hz and duration must be positive");
    }
    if (options.hot_rooms > options.rooms) {
        fail("--hot-rooms cannot exceed --rooms");
    }
    if (options.hot_collide && options.hot_rooms * options.workers > options.rooms) {
        // 그렇지 않으면 보고서가 실제로 만든 것보다 많은 과부하 Room을 주장하게 된다.
        fail("--hot-layout collide needs --rooms >= --hot-rooms * --workers");
    }
    return options;
}

// 지정한 벽시계 시간 동안 회전한다. CPU 시간이 아니라 벽시계 시간이다. 스케줄에서
// 밀려난 worker의 시간도 포함되며, tick 기한이 겪는 것도 그 시간이다.
void burn(std::chrono::microseconds cost) {
    if (cost <= 0us) {
        return;
    }
    const auto start = mo::RuntimeClock::now();
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    do {
        for (int i = 0; i < 64; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
        }
    } while (mo::RuntimeClock::now() - start < cost);
    g_sink.fetch_add(state, std::memory_order_relaxed);
}

class SyntheticRoom final : public mo::RoomLogic {
public:
    SyntheticRoom(std::chrono::microseconds tick_cost, std::chrono::microseconds command_cost)
        : tick_cost_(tick_cost), command_cost_(command_cost) {}
    void on_command(const mo::Command&) override { burn(command_cost_); }
    void on_tick(const mo::Tick&) override { burn(tick_cost_); }

private:
    std::chrono::microseconds tick_cost_;
    std::chrono::microseconds command_cost_;
};

struct Counters {
    std::uint64_t offered{};
    std::uint64_t accepted{};
    std::uint64_t rejected_full{};
    std::uint64_t rejected_other{};
};

struct Group {
    std::string name;
    std::size_t rooms{};
    std::uint64_t ticks{};
    std::uint64_t processed{};
    std::uint64_t discarded{};
    std::uint64_t accepted{};
    std::uint64_t rejected_full{};
    std::uint64_t failed_commands{};
    std::uint64_t catch_up_limits{};
    std::size_t degraded_rooms{};
    std::size_t high_water{};
    mo::RoomLatencies latencies;
    std::uint64_t worst_tick_lateness_p99_us{};
    std::size_t worst_room{};
};

Group make_group(std::string name) {
    Group group;
    group.name = std::move(name);
    return group;
}

void absorb(Group& group, std::size_t index, const mo::RoomSnapshot& stats,
            const mo::RoomLatencies& room) {
    ++group.rooms;
    group.ticks += stats.ticks;
    group.processed += stats.processed;
    group.discarded += stats.discarded;
    group.accepted += stats.accepted;
    group.rejected_full += stats.rejected_full;
    group.failed_commands += stats.failed_commands;
    group.catch_up_limits += stats.catch_up_limits;
    group.degraded_rooms += stats.degraded ? 1 : 0;
    group.high_water = std::max(group.high_water, stats.high_water);
    group.latencies.ready_wait.merge(room.ready_wait);
    group.latencies.command_wait.merge(room.command_wait);
    group.latencies.tick_lateness.merge(room.tick_lateness);
    group.latencies.tick_duration.merge(room.tick_duration);
    group.latencies.slice_duration.merge(room.slice_duration);
    const auto p99 = room.tick_lateness.percentile_us(99.0);
    if (p99 >= group.worst_tick_lateness_p99_us) {
        group.worst_tick_lateness_p99_us = p99;
        group.worst_room = index;
    }
}

std::string ms(std::uint64_t microseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << static_cast<double>(microseconds) / 1000.0;
    return out.str();
}

void print_histogram(const char* label, const mo::Histogram& histogram) {
    std::cout << "    " << std::left << std::setw(16) << label << std::right
              << " n=" << std::setw(9) << histogram.count()
              << "  p50=" << std::setw(9) << ms(histogram.percentile_us(50.0))
              << "  p90=" << std::setw(9) << ms(histogram.percentile_us(90.0))
              << "  p99=" << std::setw(9) << ms(histogram.percentile_us(99.0))
              << "  p99.9=" << std::setw(9) << ms(histogram.percentile_us(99.9))
              << "  max=" << std::setw(9) << ms(histogram.max_us());
    if (histogram.overflows() != 0) {
        std::cout << "  ABOVE-RANGE=" << histogram.overflows();
    }
    std::cout << '\n';
}

void print_group(const Group& group) {
    std::cout << "  [" << group.name << "] rooms=" << group.rooms
              << " ticks=" << group.ticks
              << " accepted=" << group.accepted
              << " processed=" << group.processed
              << " discarded=" << group.discarded
              << " rejected_full=" << group.rejected_full
              << " command_queue_high_water=" << group.high_water
              << " degraded_rooms=" << group.degraded_rooms
              << " catch_up_limits=" << group.catch_up_limits << '\n';
    print_histogram("tick_lateness", group.latencies.tick_lateness);
    print_histogram("tick_duration", group.latencies.tick_duration);
    print_histogram("command_wait", group.latencies.command_wait);
    print_histogram("ready_wait", group.latencies.ready_wait);
    print_histogram("slice_duration", group.latencies.slice_duration);
    std::cout << "    worst room by tick_lateness p99: room " << group.worst_room
              << " at " << ms(group.worst_tick_lateness_p99_us) << " ms\n";
}

void write_json_histogram(std::ostream& out, const char* name, const mo::Histogram& histogram) {
    out << "      \"" << name << "\": {\"count\": " << histogram.count()
        << ", \"p50_us\": " << histogram.percentile_us(50.0)
        << ", \"p90_us\": " << histogram.percentile_us(90.0)
        << ", \"p99_us\": " << histogram.percentile_us(99.0)
        << ", \"p999_us\": " << histogram.percentile_us(99.9)
        << ", \"max_us\": " << histogram.max_us()
        << ", \"above_range\": " << histogram.overflows() << "}";
}

void write_json(const std::string& path, const Options& options, const Counters& offered,
                const std::vector<Group>& groups, const mo::Histogram& generator_lateness) {
    std::ofstream out(path);
    if (!out) {
        fail("cannot write " + path);
    }
    out << "{\n  \"harness\": \"room-runtime-bench\",\n"
        << "  \"disclaimer\": \"synthetic in-process runtime measurement; no network, gameplay or service capacity claim\",\n"
        << "  \"config\": {\"rooms\": " << options.rooms
        << ", \"workers\": " << options.workers
        << ", \"generators\": " << options.generators
        << ", \"placement\": \"" << (options.placement == mo::Placement::fixed_worker ? "fixed" : "shared")
        << "\", \"duration_ms\": " << options.duration_ms
        << ", \"tick_hz\": " << options.tick_hz
        << ", \"input_hz\": " << options.input_hz
        << ", \"tick_cost_us\": " << options.tick_cost_us
        << ", \"command_cost_us\": " << options.command_cost_us
        << ", \"hot_rooms\": " << options.hot_rooms
        << ", \"hot_multiplier\": " << options.hot_multiplier
        << ", \"hot_layout\": \"" << (options.hot_collide ? "collide" : "spread") << "\""
        << ", \"command_queue_capacity\": " << options.command_queue
        << ", \"slice_commands\": " << options.slice_commands
        << ", \"slice_budget_us\": " << options.slice_budget_us << "},\n"
        << "  \"generator\": {\"offered\": " << offered.offered
        << ", \"accepted\": " << offered.accepted
        << ", \"rejected_full\": " << offered.rejected_full
        << ", \"rejected_other\": " << offered.rejected_other
        << ", \"lateness_p99_us\": " << generator_lateness.percentile_us(99.0)
        << ", \"lateness_max_us\": " << generator_lateness.max_us() << "},\n"
        << "  \"groups\": [\n";
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        out << "    {\n      \"name\": \"" << group.name << "\", \"rooms\": " << group.rooms
            << ", \"ticks\": " << group.ticks
            << ", \"accepted\": " << group.accepted
            << ", \"processed\": " << group.processed
            << ", \"discarded\": " << group.discarded
            << ", \"rejected_full\": " << group.rejected_full
            << ", \"degraded_rooms\": " << group.degraded_rooms
            << ", \"catch_up_limits\": " << group.catch_up_limits
            << ", \"command_queue_high_water\": " << group.high_water << ",\n";
        write_json_histogram(out, "tick_lateness", group.latencies.tick_lateness);
        out << ",\n";
        write_json_histogram(out, "tick_duration", group.latencies.tick_duration);
        out << ",\n";
        write_json_histogram(out, "command_wait", group.latencies.command_wait);
        out << ",\n";
        write_json_histogram(out, "ready_wait", group.latencies.ready_wait);
        out << ",\n";
        write_json_histogram(out, "slice_duration", group.latencies.slice_duration);
        out << "\n    }" << (i + 1 == groups.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

int run(const Options& options) {
    mo::RuntimeConfig runtime_config;
    runtime_config.workers = options.workers;
    runtime_config.max_rooms = options.rooms;
    runtime_config.command_queue_capacity = options.command_queue;
    runtime_config.max_commands_per_slice = options.slice_commands;
    runtime_config.slice_budget = std::chrono::microseconds(options.slice_budget_us);
    runtime_config.placement = options.placement;

    mo::RoomRuntime runtime(runtime_config);
    const auto tick_period = std::chrono::nanoseconds(1'000'000'000ull / options.tick_hz);
    const auto input_period = std::chrono::nanoseconds(1'000'000'000ull / options.input_hz);
    const std::chrono::microseconds base_tick(options.tick_cost_us);
    const std::chrono::microseconds base_command(options.command_cost_us);

    // spread: 앞의 N개 Room이 비싸므로 고정 배치에서 여러 worker로 나뉜다.
    // collide: 비싼 Room이 전부 worker 0에 해시된다. 성능 계획이 요구하는 스레드 편향
    // 시나리오가 이것이다.
    std::vector<bool> hot(options.rooms, false);
    for (std::size_t n = 0, i = 0; n < options.hot_rooms && i < options.rooms;
         ++n, i += options.hot_collide ? options.workers : 1) {
        hot[i] = true;
    }

    std::vector<mo::RoomHandle> handles;
    handles.reserve(options.rooms);
    for (std::size_t i = 0; i < options.rooms; ++i) {
        const auto factor = hot[i] ? static_cast<std::int64_t>(options.hot_multiplier) : 1;
        // 첫 tick을 한 주기에 걸쳐 흩어, 타이머가 한꺼번에 몰리지 않게 한다.
        const auto phase = tick_period * static_cast<std::int64_t>(i) / static_cast<std::int64_t>(options.rooms);
        auto logic = std::make_unique<SyntheticRoom>(base_tick * factor, base_command * factor);
        auto handle = runtime.create({tick_period, phase}, std::move(logic));
        if (!handle) {
            fail("room creation failed below max_rooms");
        }
        handles.push_back(*handle);
    }

    const auto start = mo::RuntimeClock::now();
    const auto end = start + std::chrono::milliseconds(options.duration_ms);
    std::vector<Counters> per_generator(options.generators);
    std::vector<mo::Histogram> generator_lateness(options.generators);
    std::vector<std::thread> generators;
    generators.reserve(options.generators);

    for (std::size_t g = 0; g < options.generators; ++g) {
        generators.emplace_back([&, g] {
            auto& counters = per_generator[g];
            auto& lateness = generator_lateness[g];
            std::vector<std::size_t> mine;
            for (std::size_t i = g; i < handles.size(); i += options.generators) {
                mine.push_back(i);
            }
            if (mine.empty()) {
                return;
            }
            // 열린 루프다. 일정이 서버를 기다리지 않으므로, 서버가 느려지면 부하가
            // 줄어드는 대신 거절과 큐 대기로 드러난다.
            std::uint64_t cycle = 0;
            std::size_t position = 0;
            for (;;) {
                const auto offset = input_period * static_cast<std::int64_t>(position) /
                                    static_cast<std::int64_t>(mine.size());
                const auto due = start + input_period * static_cast<std::int64_t>(cycle) + offset;
                if (due >= end) {
                    return;
                }
                std::this_thread::sleep_until(due);
                lateness.record(mo::RuntimeClock::now() - due);
                ++counters.offered;
                const mo::Command command{cycle, 1};
                switch (runtime.try_send(handles[mine[position]], command)) {
                    case mo::SendResult::accepted: ++counters.accepted; break;
                    case mo::SendResult::full: ++counters.rejected_full; break;
                    default: ++counters.rejected_other; break;
                }
                if (++position == mine.size()) {
                    position = 0;
                    ++cycle;
                }
            }
        });
    }
    for (auto& generator : generators) {
        generator.join();
    }

    // 먼저 종료한다. 그래야 마지막 snapshot에 취소된 작업까지 들어가고, 아래의 집계
    // 검사가 수락한 모든 명령을 포함한다.
    runtime.shutdown();

    Counters offered;
    mo::Histogram generator_total;
    for (std::size_t g = 0; g < options.generators; ++g) {
        offered.offered += per_generator[g].offered;
        offered.accepted += per_generator[g].accepted;
        offered.rejected_full += per_generator[g].rejected_full;
        offered.rejected_other += per_generator[g].rejected_other;
        generator_total.merge(generator_lateness[g]);
    }

    std::vector<Group> groups;
    groups.push_back(make_group("all"));
    if (options.hot_rooms != 0) {
        groups.push_back(make_group("hot"));
        groups.push_back(make_group("normal"));
    }
    bool accounting_ok = true;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const auto stats = *runtime.snapshot(handles[i]);
        const auto room = *runtime.latencies(handles[i]);
        if (stats.accepted != stats.processed + stats.discarded + stats.failed_commands ||
            stats.pending != 0 || stats.failed) {
            accounting_ok = false;
        }
        absorb(groups[0], i, stats, room);
        if (options.hot_rooms != 0) {
            absorb(groups[hot[i] ? 1 : 2], i, stats, room);
        }
    }

    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        mo::RuntimeClock::now() - start);
    std::cout << "room-runtime-bench: synthetic in-process measurement.\n"
              << "No network, serialization, DB or real combat. Not a service capacity result.\n"
              << "placement=" << (options.placement == mo::Placement::fixed_worker ? "fixed" : "shared")
              << " rooms=" << options.rooms
              << " workers=" << options.workers
              << " hot_rooms=" << options.hot_rooms << "x" << options.hot_multiplier
              << "/" << (options.hot_collide ? "collide" : "spread")
              << " tick_hz=" << options.tick_hz
              << " input_hz=" << options.input_hz
              << " tick_cost_us=" << options.tick_cost_us
              << " command_cost_us=" << options.command_cost_us
              << " wall_ms=" << wall.count() << '\n'
              << "  [generator] offered=" << offered.offered
              << " accepted=" << offered.accepted
              << " rejected_full=" << offered.rejected_full
              << " rejected_other=" << offered.rejected_other
              << " schedule_lateness_p99=" << ms(generator_total.percentile_us(99.0))
              << " ms max=" << ms(generator_total.max_us()) << " ms\n";
    if (generator_total.percentile_us(99.0) > 2000) {
        std::cout << "  WARNING: the generator itself was late; treat this run as generator-bound.\n";
    }
    for (const auto& group : groups) {
        print_group(group);
    }
    if (!options.json_path.empty()) {
        write_json(options.json_path, options, offered, groups, generator_total);
        std::cout << "  wrote " << options.json_path << '\n';
    }
    if (!accounting_ok) {
        std::cerr << "FAIL: command accounting or room state invariant broken\n";
        return 1;
    }
    return 0;
}

} // 익명 네임스페이스 끝

int main(int argc, char** argv) {
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
