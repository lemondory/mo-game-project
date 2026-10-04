cmake_minimum_required(VERSION 3.25)
project(MOGameProject VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build, preferably a CMake preset.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 전역이 아니라 우리 타깃에만 적용한다. 외부 의존성에 번지지 않게 하기 위해서다.
add_library(mo_project_options INTERFACE)
add_library(mo::project_options ALIAS mo_project_options)
target_compile_features(mo_project_options INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(mo_project_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mo_project_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(cmake/CheckToolchain.cmake)

set(MO_SANITIZER "none" CACHE STRING "Runtime sanitizer: none, address, thread")
set_property(CACHE MO_SANITIZER PROPERTY STRINGS none address thread)
if(NOT MO_SANITIZER STREQUAL "none")
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "This sanitizer setup requires Clang or GCC.")
    endif()
    if(MO_SANITIZER STREQUAL "address")
        set(mo_sanitizer_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
    elseif(MO_SANITIZER STREQUAL "thread")
        set(mo_sanitizer_flags -fsanitize=thread -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR "Unknown MO_SANITIZER=${MO_SANITIZER}")
    endif()
    target_compile_options(mo_project_options INTERFACE ${mo_sanitizer_flags})
    target_link_options(mo_project_options INTERFACE ${mo_sanitizer_flags})
endif()

add_library(mo_runtime server/runtime/room_runtime.cpp server/runtime/latency_histogram.hpp)
add_library(mo::runtime ALIAS mo_runtime)
target_include_directories(mo_runtime PUBLIC server/runtime)
target_link_libraries(mo_runtime PUBLIC mo::project_options Threads::Threads)

add_executable(room-runtime-demo server/apps/room-runtime-demo/main.cpp)
target_link_libraries(room-runtime-demo PRIVATE mo::runtime)

# 네트워크 계층은 runtime과도, 서로와도 독립이다. 프로토콜은 소켓을 모르고
# transport는 메시지의 의미를 모른다.
add_library(mo_protocol shared/protocol/message.cpp)
add_library(mo::protocol ALIAS mo_protocol)
target_include_directories(mo_protocol PUBLIC shared/protocol)
target_link_libraries(mo_protocol PUBLIC mo::project_options)

add_library(mo_transport shared/transport/framing.cpp shared/transport/loopback.cpp)
add_library(mo::transport ALIAS mo_transport)
target_include_directories(mo_transport PUBLIC shared/transport)
target_link_libraries(mo_transport PUBLIC mo::project_options)

# 세션 정책은 프로토콜만 안다. transport와 엮는 일은 구동 코드가 한다.
add_library(mo_session server/session/session.cpp)
add_library(mo::session ALIAS mo_session)
target_include_directories(mo_session PUBLIC server/session)
target_link_libraries(mo_session PUBLIC mo::protocol)

add_executable(session-demo server/apps/session-demo/main.cpp)
target_link_libraries(session-demo PRIVATE mo::session mo::transport)

add_executable(room-runtime-bench server/apps/room-runtime-bench/main.cpp)
target_link_libraries(room-runtime-bench PRIVATE mo::runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(room-runtime-tests tests/room_runtime_tests.cpp)
    target_link_libraries(room-runtime-tests PRIVATE mo::runtime)
    foreach(case producers wakeup parallel fairness lifecycle shutdown ticks tick_close exceptions
                 histogram latencies placement)
        add_test(NAME runtime.${case} COMMAND room-runtime-tests ${case})
        set_tests_properties(runtime.${case} PROPERTIES TIMEOUT 20)
    endforeach()
    add_executable(protocol-tests tests/protocol_tests.cpp)
    target_link_libraries(protocol-tests PRIVATE mo::protocol mo::transport)
    foreach(case wire_layout writer_bounds truncation trailing unknown_values round_trip
                 strong_ids quantization framing_split framing_oversized framing_buffer
                 loopback_delivery loopback_backpressure loopback_integrity_limit receive_bound)
        add_test(NAME protocol.${case} COMMAND protocol-tests ${case})
        set_tests_properties(protocol.${case} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(session-tests tests/session_tests.cpp)
    target_link_libraries(session-tests PRIVATE mo::session mo::transport)
    foreach(case packets handshake order rejection timeouts backpressure lifetime loopback)
        add_test(NAME session.${case} COMMAND session-tests ${case})
        set_tests_properties(session.${case} PROPERTIES TIMEOUT 10)
    endforeach()
    add_test(NAME session.demo COMMAND session-demo)
    set_tests_properties(session.demo PROPERTIES TIMEOUT 10)

    add_test(NAME runtime.demo COMMAND room-runtime-demo)
    set_tests_properties(runtime.demo PROPERTIES TIMEOUT 10)
    # 짧은 확인용 실행이다. 하네스 동작과 집계 불변식만 보고 지연은 판정하지 않는다.
    add_test(NAME runtime.bench_shared COMMAND room-runtime-bench
             --rooms 6 --workers 2 --duration-ms 400 --tick-hz 50 --input-hz 20
             --tick-cost-us 200 --command-cost-us 50 --placement shared)
    add_test(NAME runtime.bench_fixed COMMAND room-runtime-bench
             --rooms 6 --workers 2 --duration-ms 400 --tick-hz 50 --input-hz 20
             --tick-cost-us 200 --command-cost-us 50 --hot-rooms 2 --placement fixed)
    set_tests_properties(runtime.bench_shared runtime.bench_fixed PROPERTIES TIMEOUT 60)
endif()
