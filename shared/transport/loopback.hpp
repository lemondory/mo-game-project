#pragma once

// 소켓도 스레드도 없이 메모리 안에서 이어 붙인 Endpoint 한 쌍이다.
//
// 세션과 프로토콜 동작을 결정적으로 시험하기 위해 있다. 바이트를 언제 몇 개씩 옮길지
// 테스트가 정확히 정한다. 실제 소켓은 읽기가 나뉘는 방식이 매번 달라서, 재조립 버그가
// 매 실행이 아니라 백 번에 한 번 드러난다.

#include "endpoint.hpp"
#include "framing.hpp"

#include <memory>
#include <vector>

namespace mo::transport {

struct LoopbackConfig {
    std::uint32_t max_payload{64 * 1024};
    // send()가 would_block을 반환하기 전까지 상대에게 가는 도중일 수 있는 바이트 수.
    std::size_t send_buffer_bytes{256 * 1024};
    // 받는 쪽이 더 받지 않기 전까지 들고 있을 수 있는 바이트 수. 가득 차면 pump()가
    // 아무것도 옮기지 않고, 송신 버퍼가 차면서 send()가 would_block을 반환한다.
    // 0이면 조립기의 기본값을 쓴다.
    std::size_t receive_buffer_bytes{0};
    // pump() 한 번에 방향마다 옮기는 최대 덩어리 크기. 몇 바이트로 낮추면 모든 프레임이
    // 쪼개져 도착하므로 재조립 경로를 확실히 지나간다.
    std::size_t chunk_bytes{0}; // 0이면 수신 여유 안에서 가능한 바이트를 모두 옮긴다.
};

// 양쪽 끝을 모두 소유한다. pump()를 불러야만 바이트가 움직이므로 테스트가 타이밍에
// 의존하지 않는다.
class LoopbackLink {
public:
    explicit LoopbackLink(LoopbackConfig config = {});
    ~LoopbackLink();
    LoopbackLink(const LoopbackLink&) = delete;
    LoopbackLink& operator=(const LoopbackLink&) = delete;

    Endpoint& first();
    Endpoint& second();

    // 양방향으로 모아둔 바이트를 옮긴다. 전달한 총 바이트 수를 반환하므로, 호출자는
    // 0이 될 때까지 반복하면 된다.
    std::size_t pump();

    // first에서 second로 가는 다음 drop_bytes 바이트를 버린다. 손상되거나 잘린 스트림이
    // 조용히 잘못 읽히지 않고 거절되는지 확인하는 데 쓴다.
    void drop_from_first(std::size_t drop_bytes);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // mo::transport 네임스페이스 끝
