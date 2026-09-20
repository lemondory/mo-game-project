#pragma once

// 게임 규칙에 의존하지 않는 제어 메시지다.
//
// 이동·전투·snapshot 메시지는 일부러 넣지 않았다. 모양이 파티 인원과 전투 범위에
// 달려 있는데 아직 정해지지 않았다. 미리 추가하면 추측 위에 전송 형식을 고정하게 된다.

#include "wire.hpp"

#include <cstdint>
#include <span>

namespace mo::protocol {

// 기존 메시지의 의미나 배치가 바뀔 때마다 올린다. 다른 버전을 알리는 상대는 이후
// 프레임을 잘못 읽게 두지 않고 handshake에서 거절한다.
inline constexpr std::uint16_t wire_version = 1;

// 이보다 큰 프레임은 메모리를 할당하기 전에 거절한다. 이 상한은 길이 접두사를
// 제외한 payload에 적용된다.
inline constexpr std::uint32_t max_payload_bytes = 64 * 1024;

enum class MessageId : std::uint16_t {
    // 0은 배정하지 않는다. 0으로 채운 버퍼가 유효한 메시지가 되지 않게 하기 위해서다.
    handshake = 1,
    handshake_ack = 2,
    ping = 3,
    pong = 4,
};

bool valid(MessageId id);

// 모든 프레임이 이것으로 시작한다. id가 나머지를 어떻게 읽을지 결정한다.
struct Header {
    MessageId id{};
    std::uint16_t version{};
};

struct Handshake {
    std::uint16_t version{wire_version};
    std::uint64_t client_nonce{};
    // 빌드 식별자다. 다른 빌드의 클라이언트가 컴파일되지 않은 규칙으로 동작하게 두지
    // 않고 거절한다.
    std::uint32_t build_id{};
};

enum class HandshakeStatus : std::uint8_t {
    accepted = 0,
    version_mismatch = 1,
    build_mismatch = 2,
    rejected = 3,
};

struct HandshakeAck {
    HandshakeStatus status{};
    std::uint16_t server_version{wire_version};
    std::uint64_t session_id{};
};

// 보낸 쪽의 시각을 실어, 상대가 자기 쪽에 대기 중인 ping 목록을 들고 있지 않아도
// 왕복 시간을 잴 수 있게 한다.
struct Ping {
    std::uint64_t sent_at_us{};
};

struct Pong {
    std::uint64_t echoed_sent_at_us{};
    std::uint64_t replied_at_us{};
};

bool encode(Writer& writer, const Header& value);
bool encode(Writer& writer, const Handshake& value);
bool encode(Writer& writer, const HandshakeAck& value);
bool encode(Writer& writer, const Ping& value);
bool encode(Writer& writer, const Pong& value);

bool decode(Reader& reader, Header& out);
bool decode(Reader& reader, Handshake& out);
bool decode(Reader& reader, HandshakeAck& out);
bool decode(Reader& reader, Ping& out);
bool decode(Reader& reader, Pong& out);

} // mo::protocol 네임스페이스 끝
