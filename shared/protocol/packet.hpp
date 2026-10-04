#pragma once

#include "message.hpp"

#include <optional>
#include <variant>

namespace mo::protocol {

using Packet = std::variant<Handshake, HandshakeAck, Ping, Pong>;
// 현재 지원하는 Header와 body를 합친 최대 크기다. 게임 플레이 패킷은 아직 없다.
inline constexpr std::size_t max_control_packet_bytes = 18;

template <typename T> struct packet_traits;
template <> struct packet_traits<Handshake> { static constexpr auto id = MessageId::handshake; };
template <> struct packet_traits<HandshakeAck> { static constexpr auto id = MessageId::handshake_ack; };
template <> struct packet_traits<Ping> { static constexpr auto id = MessageId::ping; };
template <> struct packet_traits<Pong> { static constexpr auto id = MessageId::pong; };

template <typename T>
[[nodiscard]] bool encode_packet(Writer& writer, const T& body) {
    return encode(writer, Header{packet_traits<T>::id}) && encode(writer, body);
}

inline std::optional<Packet> decode_packet(std::span<const std::byte> bytes) {
    if (bytes.size() > max_control_packet_bytes) {
        return std::nullopt;
    }
    Reader reader(bytes);
    Header header;
    if (!decode(reader, header)) {
        return std::nullopt;
    }
    const auto read_body = [&reader]<typename T>() -> std::optional<Packet> {
        T body;
        if (!decode(reader, body) || !reader.done()) {
            return std::nullopt;
        }
        return Packet{body};
    };
    switch (header.id) {
    case MessageId::handshake: return read_body.template operator()<Handshake>();
    case MessageId::handshake_ack: return read_body.template operator()<HandshakeAck>();
    case MessageId::ping: return read_body.template operator()<Ping>();
    case MessageId::pong: return read_body.template operator()<Pong>();
    }
    return std::nullopt;
}

} // mo::protocol 네임스페이스 끝

// 약어 설명
// ACK(Acknowledgment): 요청의 처리 결과를 알리는 응답. HandshakeAck는 연결 협상 결과를 담는다.
// ID(Identifier): 패킷의 메시지 종류를 구분하는 식별자.
