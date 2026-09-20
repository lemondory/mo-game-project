#pragma once

// 게임 규칙에 의존하지 않는 제어 메시지다.
//
// 이동·전투·snapshot 메시지는 일부러 넣지 않았다. 모양이 파티 인원과 전투 공간에
// 달려 있는데 아직 정해지지 않았다. 추측 위에 전송 형식을 고정하면 기다리는 것보다
// 비싸진다.
//
// 구버전 클라이언트가 새 서버를 잘못 읽지 않게 하는 규칙이 둘 있다.
//   - 메시지 id는 재사용하지 않는다. 삭제한 메시지의 번호는 영구 결번이다.
//   - 필드는 뒤에만 추가한다. 중간 삽입도 순서 변경도 하지 않는다.

#include "codec.hpp"
#include "ids.hpp"

#include <array>
#include <cstdint>

namespace mo::protocol {

// 연결마다 handshake에서 한 번 협상하고 패킷마다 싣지 않는다. 한 연결 안에서
// 버전이 바뀌지 않으므로, 반복해 보내면 모든 입력과 snapshot에서 대역폭만 쓴다.
inline constexpr std::uint16_t wire_version = 1;

// 이보다 큰 프레임은 메모리를 할당하기 전에 거절한다.
inline constexpr std::uint32_t max_payload_bytes = 64 * 1024;

// 0은 배정하지 않는다. 0으로 채운 버퍼가 유효한 메시지가 되지 않게 하기 위해서다.
enum class MessageId : std::uint16_t {
    handshake = 1,
    handshake_ack = 2,
    ping = 3,
    pong = 4,
};

template <>
struct enum_traits<MessageId> {
    static constexpr std::array values{MessageId::handshake, MessageId::handshake_ack,
                                       MessageId::ping, MessageId::pong};
};

enum class HandshakeStatus : std::uint8_t {
    accepted = 0,
    version_mismatch = 1,
    build_mismatch = 2,
    rejected = 3,
};

template <>
struct enum_traits<HandshakeStatus> {
    static constexpr std::array values{HandshakeStatus::accepted,
                                       HandshakeStatus::version_mismatch,
                                       HandshakeStatus::build_mismatch,
                                       HandshakeStatus::rejected};
};

bool valid(MessageId id);

// 모든 프레임이 이것으로 시작한다. id가 나머지를 어떻게 읽을지 결정한다.
struct Header {
    MessageId id{};

    template <typename Self, typename V>
    static void fields(Self& self, V& visit) {
        visit(self.id);
    }
};

struct Handshake {
    std::uint16_t version{wire_version};
    std::uint64_t client_nonce{};
    // 빌드 식별자다. 다른 규칙으로 컴파일된 클라이언트가 서버와 공유하지 않는 규칙으로
    // 플레이하게 두지 않고 접속 시점에 거절한다.
    std::uint32_t build_id{};

    template <typename Self, typename V>
    static void fields(Self& self, V& visit) {
        visit(self.version);
        visit(self.client_nonce);
        visit(self.build_id);
    }
};

struct HandshakeAck {
    HandshakeStatus status{};
    std::uint16_t server_version{wire_version};
    SessionId session{};

    template <typename Self, typename V>
    static void fields(Self& self, V& visit) {
        visit(self.status);
        visit(self.server_version);
        visit(self.session);
    }
};

// 보낸 쪽의 시각을 실어, 상대가 대기 중인 ping 목록을 들고 있지 않아도 왕복 시간을
// 잴 수 있게 한다.
struct Ping {
    std::uint64_t sent_at_us{};

    template <typename Self, typename V>
    static void fields(Self& self, V& visit) {
        visit(self.sent_at_us);
    }
};

struct Pong {
    std::uint64_t echoed_sent_at_us{};
    std::uint64_t replied_at_us{};

    template <typename Self, typename V>
    static void fields(Self& self, V& visit) {
        visit(self.echoed_sent_at_us);
        visit(self.replied_at_us);
    }
};

} // mo::protocol 네임스페이스 끝
