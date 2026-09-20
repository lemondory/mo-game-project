#include "message.hpp"

namespace mo::protocol {

bool valid(MessageId id) {
    switch (id) {
        case MessageId::handshake:
        case MessageId::handshake_ack:
        case MessageId::ping:
        case MessageId::pong:
            return true;
    }
    return false;
}

namespace {

bool read_status(Reader& reader, HandshakeStatus& out) {
    std::uint8_t raw{};
    if (!reader.u8(raw)) {
        return false;
    }
    switch (static_cast<HandshakeStatus>(raw)) {
        case HandshakeStatus::accepted:
        case HandshakeStatus::version_mismatch:
        case HandshakeStatus::build_mismatch:
        case HandshakeStatus::rejected:
            out = static_cast<HandshakeStatus>(raw);
            return true;
    }
    return false; // 모르는 상태 값이다. 추측하지 말고 거절한다.
}

} // 익명 네임스페이스 끝

bool encode(Writer& writer, const Header& value) {
    return writer.u16(static_cast<std::uint16_t>(value.id)) && writer.u16(value.version);
}

bool encode(Writer& writer, const Handshake& value) {
    return writer.u16(value.version) && writer.u64(value.client_nonce) && writer.u32(value.build_id);
}

bool encode(Writer& writer, const HandshakeAck& value) {
    return writer.u8(static_cast<std::uint8_t>(value.status)) && writer.u16(value.server_version) &&
           writer.u64(value.session_id);
}

bool encode(Writer& writer, const Ping& value) { return writer.u64(value.sent_at_us); }

bool encode(Writer& writer, const Pong& value) {
    return writer.u64(value.echoed_sent_at_us) && writer.u64(value.replied_at_us);
}

bool decode(Reader& reader, Header& out) {
    std::uint16_t id{};
    if (!reader.u16(id) || !reader.u16(out.version)) {
        return false;
    }
    const auto message = static_cast<MessageId>(id);
    if (!valid(message)) {
        return false; // 모르는 id다. 상대의 설명을 믿지 않고 프레임을 버린다.
    }
    out.id = message;
    return true;
}

bool decode(Reader& reader, Handshake& out) {
    return reader.u16(out.version) && reader.u64(out.client_nonce) && reader.u32(out.build_id);
}

bool decode(Reader& reader, HandshakeAck& out) {
    return read_status(reader, out.status) && reader.u16(out.server_version) &&
           reader.u64(out.session_id);
}

bool decode(Reader& reader, Ping& out) { return reader.u64(out.sent_at_us); }

bool decode(Reader& reader, Pong& out) {
    return reader.u64(out.echoed_sent_at_us) && reader.u64(out.replied_at_us);
}

} // mo::protocol 네임스페이스 끝
