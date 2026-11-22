#pragma once

#include "session.hpp"
#include <cpp_network/connection.hpp>
#include <asio/strand.hpp>
#include <functional>
#include <memory>

namespace mo::net {

// 연결의 모든 처리와 세션 타이머가 같은 실행 문맥을 공유하도록 타입으로 제한한다.
using SerialExecutor = asio::strand<asio::any_io_executor>;
using SessionSocket = asio::basic_stream_socket<asio::ip::tcp, SerialExecutor>;

struct SocketConfig {
    session::Config session;
    cpp_network::Limits transport{256 * 1024, 256, 1024};
    std::size_t receive_buffer_bytes = 4096;
    std::size_t frames_per_turn = 32;
    std::chrono::microseconds processing_budget{500};
};
struct SocketStats {
    std::uint64_t received_bytes{};
    // 로컬 송신 대기열에 수락된 바이트다. 원격 수신·처리 완료량이 아니다.
    std::uint64_t accepted_send_bytes{};
    // 전체 로컬 쓰기가 성공한 송신의 바이트 수다. 원격 처리 완료량은 아니다.
    std::uint64_t completed_send_bytes{};
    std::uint64_t frames{};
    std::uint64_t send_blocks{};
    std::uint64_t budget_yields{};
    // 연결별 논리적 대기량이다. 할당 여유분·커널 버퍼·프로세스 전체 메모리는 제외한다.
    std::size_t peak_receive_bytes{};
    std::size_t peak_send_bytes{};
};
struct SocketResult {
    session::CloseReason reason;
    SocketStats stats;
};

// 연결된 소켓을 넘긴 뒤 start한다. 소켓/타이머/세션은 내부에서만 조작한다.
// start·close는 여러 스레드에서 호출 가능하며 중복 예약하지 않는다.
// 이동·파괴와 다른 공개 함수 호출은 겹치면 안 된다. 종료 통지까지 실행 문맥을 유지한다.
class SocketSession {
public:
    using OnClosed = std::function<void(SocketResult)>;
    SocketSession(SessionSocket socket, SocketConfig config, protocol::SessionId id,
                  std::uint64_t generation, OnClosed on_closed);
    ~SocketSession();
    SocketSession(const SocketSession&) = delete;
    SocketSession& operator=(const SocketSession&) = delete;
    void start();
    void close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // mo::net 네임스페이스 끝

// ID(Identifier): 연결을 구분하는 식별자. drain: 수락한 송신이 완료될 때까지 기다리는 처리.
// strand: 지정된 처리들을 한 번에 하나씩 실행하는 Asio 실행 수단.
