#pragma once

// 세션 로직과 실제 네트워크 사이의 경계다.
//
// 이 인터페이스 위쪽은 프레임 단위로만 동작하고 소켓을 직접 만지지 않는다. 덕분에 같은
// 세션 코드를 실제 소켓에서도, 아래의 메모리 loopback에서도, 프레임을 일부러 버리고
// 지연시키는 링크에서도 돌릴 수 있다. 나중에 문제가 생겼을 때 같은 경우를 loopback에서
// 돌려보면 프로토콜 버그인지 소켓 버그인지 가를 수 있다.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mo::transport {

enum class SendStatus {
    sent,
    // 상대가 충분히 빨리 읽지 못해 송신 버퍼가 상한에 닿았다. 상한 없이 키우면 느린
    // 클라이언트 하나가 같은 노드의 다른 모두의 메모리를 먹는다.
    would_block,
    closed,
    // 링크가 받아들이는 크기를 넘는 프레임이다. 보내지 않았다.
    too_large,
};

class Endpoint {
public:
    virtual ~Endpoint() = default;

    virtual SendStatus send(std::span<const std::byte> payload) = 0;

    // 완성된 프레임을 최대 max_frames개까지 out 뒤에 덧붙인다. 덧붙인 개수를 반환한다.
    // 각 프레임은 길이 접두사를 뗀 payload 전체다.
    virtual std::size_t receive(std::vector<std::vector<std::byte>>& out,
                                std::size_t max_frames) = 0;

    virtual void close() = 0;
    virtual bool closed() const = 0;
    // 상한을 넘는 프레임 길이 선언처럼 복구할 수 없는 프로토콜 오류를 본 뒤 참이 된다.
    // 호출자는 연결을 끊어야 한다.
    virtual bool faulted() const = 0;
};

} // mo::transport 네임스페이스 끝
