find_package(Threads REQUIRED)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles([=[
    #include <array>
    #include <bit>
    #include <chrono>
    #include <span>
    #include <stop_token>
    #include <thread>

    int main() {
        static_assert(std::chrono::steady_clock::is_steady);
        static_assert(std::popcount(3u) == 2);
        const std::array<int, 2> values{1, 2};
        const std::span<const int> view{values};
        std::jthread worker([](std::stop_token stop) {
            (void)stop.stop_requested();
        });
        worker.request_stop();
        worker.join();
        return view.size() == 2 ? 0 : 1;
    }
]=] MO_HAS_REQUIRED_CXX20_LIBRARY)
cmake_pop_check_state()

if(NOT MO_HAS_REQUIRED_CXX20_LIBRARY)
    message(FATAL_ERROR
        "C++20 span/bit/jthread/stop_token support is required. "
        "Check your compiler and standard library; see docs/07-development-environment.md.")
endif()
