#pragma once

// 바이트 스트림에서 메시지 경계를 복원한다.
//
// TCP는 메시지가 아니라 바이트를 전달한다. 한 번 읽기에 프레임 절반이 올 수도 있고
// 세 개와 조각이 올 수도 있다. 이 조립기는 받은 만큼 모아두고 완성된 프레임만 넘긴다.
// 모든 스트림 transport가 쓰므로, 과대 프레임 처리 규칙을 소켓 구현마다 두지 않고
// 여기에 모아둔다.

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
    // max_payload는 프레임 하나의 상한이다. 버퍼에 쌓인 바이트는
    // max_payload + length_prefix_bytes 아래로 유지되므로, 상대가 큰 길이를 선언한 뒤
    // 멈추는 방식으로 이 버퍼를 무한히 키울 수 없다.
    explicit FrameAssembler(std::uint32_t max_payload);

    // 받은 바이트를 덧붙인다. 과대 프레임으로 거절된 뒤에는 false를 반환하며, 그
    // 시점부터 연결은 쓸 수 없다.
    bool push(std::span<const std::byte> bytes);

    // 다음 완성 프레임을 가져간다. 반환한 span은 이 객체의 버퍼를 가리키며 다음
    // push()나 next_frame() 호출로 무효가 된다.
    FrameResult next_frame(std::span<const std::byte>& out);

    bool poisoned() const { return poisoned_; }
    std::size_t buffered() const { return buffer_.size() - consumed_; }

private:
    void compact();

    std::uint32_t max_payload_;
    std::vector<std::byte> buffer_;
    std::size_t consumed_{};
    bool poisoned_{};
};

// 길이 접두사와 payload를 out에 쓴다. payload가 max_payload를 넘으면 false를 반환한다.
// 받는 쪽 조립기가 반드시 거절해야 하는 프레임을 내보내면 안 되기 때문이다.
bool write_frame(std::vector<std::byte>& out, std::span<const std::byte> payload,
                 std::uint32_t max_payload);

} // mo::transport 네임스페이스 끝
