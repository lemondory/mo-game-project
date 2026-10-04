#pragma once

// 바이트 스트림에서 메시지 경계를 복원한다.
//
// TCP는 메시지가 아니라 바이트를 전달한다. 한 번 읽기에 프레임 절반이 올 수도 있고
// 세 개와 조각이 올 수도 있다. 이 조립기는 받은 만큼 모아두고 완성된 프레임만 넘긴다.
//
// 하지 않는 일: payload 안의 손상이나 유실을 탐지하는 것. 길이 접두사는 프레임이 어디서
// 끝나는지만 알려주므로, 바이트가 사라지면 다음 프레임의 접두사가 payload로 읽히고
// 보낸 적 없는 값이 정상적으로 디코딩된다. 무결성은 보안 계층의 몫이다
// (TLS와 GameNetworkingSockets 모두 레코드마다 인증한다). 프레이밍을 그것으로 착각하면 안 된다.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mo::transport {

// 4바이트 리틀엔디안 payload 길이, 그다음 payload.
inline constexpr std::size_t length_prefix_bytes = 4;

enum class FrameResult {
    // 완성된 프레임이 하나 있다.
    frame,
    // 아직 없다. 바이트가 더 필요하다.
    incomplete,
    // 선언된 길이가 설정한 상한을 넘었다. 다음 경계를 알 수 없어 스트림을 다시 맞출 수
    // 없으므로, 호출자는 건너뛰지 말고 연결을 끊어야 한다.
    too_large,
};

class FrameAssembler {
public:
    // max_payload는 프레임 하나의 상한이다. max_buffered는 호출자가 아직 가져가지 않은
    // 완성 프레임까지 포함해 한 번에 들고 있는 전체의 상한이다. 두 번째 상한이 없으면
    // 아무도 읽지 않는데 계속 보내는 상대가 이 버퍼를 프로세스가 죽을 때까지 키우고,
    // 같은 노드의 다른 연결까지 함께 끌고 내려간다. max_buffered에 0을 넘기면 프레임
    // 하나의 작은 배수를 쓴다.
    explicit FrameAssembler(std::uint32_t max_payload, std::size_t max_buffered = 0);

    // 지금 push할 수 있는 바이트 수다. 소켓 읽기 루프는 최대 이만큼만 읽는다. 0이 되면
    // 읽기를 멈추고 수신 윈도가 닫히며 송신자가 막힌다. 그 정체가 곧 역압이다.
    std::size_t capacity() const;

    // 받은 바이트를 전부 넣거나 아무것도 넣지 않는다. capacity()를 넘거나 이미 과대
    // 프레임으로 거절된 뒤라면 아무것도 소비하지 않고 false를 반환한다. 호출자는 이
    // 반환값을 버리면 안 된다. 바이트를 흘리면 스트림이 조용히 깨진다.
    [[nodiscard]] bool push(std::span<const std::byte> bytes);

    // 다음 완성 프레임을 가져간다. 반환한 span은 이 객체의 버퍼를 가리키며 다음
    // push()나 next_frame() 호출로 무효가 된다.
    FrameResult next_frame(std::span<const std::byte>& out);

    bool poisoned() const { return poisoned_; }
    std::size_t buffered() const { return buffer_.size() - consumed_; }
    std::size_t max_buffered() const { return max_buffered_; }

private:
    void compact();

    std::uint32_t max_payload_;
    std::size_t max_buffered_;
    std::vector<std::byte> buffer_;
    std::size_t consumed_{};
    bool poisoned_{};
};

// 길이 접두사와 payload를 out에 쓴다. payload가 max_payload를 넘으면 false를 반환한다.
// 받는 쪽 조립기가 반드시 거절해야 하는 프레임을 내보내면 안 되기 때문이다.
bool write_frame(std::vector<std::byte>& out, std::span<const std::byte> payload,
                 std::uint32_t max_payload);

} // mo::transport 네임스페이스 끝
