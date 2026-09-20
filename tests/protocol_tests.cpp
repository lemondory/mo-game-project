#include "framing.hpp"
#include "loopback.hpp"
#include "message.hpp"
#include "wire.hpp"

#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mo;
using namespace mo::protocol;
using namespace mo::transport;

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <typename T>
std::vector<std::byte> encoded(const T& value) {
    std::array<std::byte, 256> storage{};
    Writer writer(storage);
    check(encode(writer, value), "encode failed with room to spare");
    return {writer.view().begin(), writer.view().end()};
}

// 고정 리틀엔디안 배치다. 한 기기에서 뜬 캡처가 다른 기기에서도 같게 디코딩되어야 한다.
void wire_layout() {
    std::array<std::byte, 16> storage{};
    Writer writer(storage);
    check(writer.u32(0x01020304u), "u32 write");
    const auto view = writer.view();
    check(view.size() == 4, "u32 must occupy exactly 4 bytes");
    check(std::to_integer<int>(view[0]) == 0x04 && std::to_integer<int>(view[1]) == 0x03 &&
              std::to_integer<int>(view[2]) == 0x02 && std::to_integer<int>(view[3]) == 0x01,
          "u32 must be little-endian on every platform");

    Reader reader(view);
    std::uint32_t value{};
    check(reader.u32(value) && value == 0x01020304u, "u32 round trip");
    check(reader.done(), "reader should be exhausted");
}

// 공간이 모자란 writer는 버퍼 밖에 쓰지 않고 그 사실을 알린다.
void writer_bounds() {
    std::array<std::byte, 3> storage{};
    Writer writer(storage);
    check(writer.u16(0x1122), "first write fits");
    check(!writer.u32(0), "oversized write must be refused");
    check(writer.overflowed(), "overflow must be visible to the caller");
    check(writer.written() == 2, "refused write must not advance the cursor");
}

// 모든 짧은 읽기를 잡아낸다. 메시지를 어느 길이에서 잘라도 실패해야 한다.
void reader_rejects_truncation() {
    const Handshake original{wire_version, 0xDEADBEEFCAFEF00Dull, 42};
    const auto full = encoded(original);
    check(full.size() == 14, "handshake layout changed without a version bump");

    for (std::size_t length = 0; length < full.size(); ++length) {
        Reader reader(std::span(full).first(length));
        Handshake out;
        check(!decode(reader, out), "truncated handshake must not decode");
        check(reader.failed(), "truncation must be recorded");
    }

    Reader reader(full);
    Handshake out;
    check(decode(reader, out) && reader.done(), "full handshake decodes");
    check(out.version == original.version && out.client_nonce == original.client_nonce &&
              out.build_id == original.build_id,
          "handshake round trip");
}

// 남는 바이트는 양쪽이 메시지를 다르게 알고 있다는 뜻이다. 앞부분만 받아들이면 그
// 불일치가 더 나쁜 일이 생길 때까지 가려진다.
void reader_rejects_trailing_bytes() {
    auto bytes = encoded(Ping{12345});
    bytes.push_back(std::byte{0xFF});
    Reader reader(bytes);
    Ping out;
    check(decode(reader, out), "prefix still decodes");
    check(!reader.done(), "trailing bytes must leave the reader unfinished");
}

// 이 빌드가 모르는 id나 상태 값은 추측하지 않고 거절한다.
void unknown_values_rejected() {
    std::array<std::byte, 8> storage{};
    Writer writer(storage);
    check(writer.u16(9999) && writer.u16(wire_version), "write unknown id");
    Reader reader(writer.view());
    Header header;
    check(!decode(reader, header), "unknown message id must be refused");

    std::array<std::byte, 16> ack_storage{};
    Writer ack_writer(ack_storage);
    check(ack_writer.u8(200) && ack_writer.u16(wire_version) && ack_writer.u64(1), "write bad status");
    Reader ack_reader(ack_writer.view());
    HandshakeAck ack;
    check(!decode(ack_reader, ack), "unknown handshake status must be refused");
}

void all_messages_round_trip() {
    const Header header{MessageId::pong, wire_version};
    // 버퍼를 변수에 묶는다. Reader가 빌려 쓰므로 임시 객체보다 오래 살면 안 된다.
    const auto header_bytes = encoded(header);
    Reader header_reader(header_bytes);
    Header header_out;
    check(decode(header_reader, header_out) && header_reader.done(), "header round trip");
    check(header_out.id == header.id && header_out.version == header.version, "header values");

    const HandshakeAck ack{HandshakeStatus::version_mismatch, 7, 0x1234567890ABCDEFull};
    auto ack_bytes = encoded(ack);
    Reader ack_reader(ack_bytes);
    HandshakeAck ack_out;
    check(decode(ack_reader, ack_out) && ack_reader.done(), "ack round trip");
    check(ack_out.status == ack.status && ack_out.server_version == ack.server_version &&
              ack_out.session_id == ack.session_id,
          "ack values");

    const Pong pong{111, 222};
    const auto pong_bytes = encoded(pong);
    Reader pong_reader(pong_bytes);
    Pong pong_out;
    check(decode(pong_reader, pong_out) && pong_reader.done(), "pong round trip");
    check(pong_out.echoed_sent_at_us == 111 && pong_out.replied_at_us == 222, "pong values");
}

std::vector<std::byte> payload_of(std::size_t size, std::byte fill) {
    return std::vector<std::byte>(size, fill);
}

// 스트림에는 메시지 경계가 없다. 한 바이트씩 흘려 넣어도 쓴 프레임이 순서 그대로
// 나와야 한다.
void framing_reassembles_split_streams() {
    std::vector<std::byte> stream;
    const std::vector<std::vector<std::byte>> frames{
        payload_of(0, std::byte{0}),
        payload_of(1, std::byte{0xAA}),
        payload_of(300, std::byte{0xBB}),
        payload_of(7, std::byte{0xCC}),
    };
    for (const auto& frame : frames) {
        check(write_frame(stream, frame, 1024), "write_frame");
    }

    FrameAssembler assembler(1024);
    std::vector<std::vector<std::byte>> received;
    for (const auto byte : stream) {
        const std::array<std::byte, 1> one{byte};
        check(assembler.push(one), "push one byte");
        std::span<const std::byte> frame;
        while (assembler.next_frame(frame) == FrameResult::frame) {
            received.emplace_back(frame.begin(), frame.end());
        }
    }
    check(received.size() == frames.size(), "frame count after byte-wise delivery");
    for (std::size_t i = 0; i < frames.size(); ++i) {
        check(received[i] == frames[i], "frame content after byte-wise delivery");
    }
}

// 상한을 넘는 길이 선언은 스트림을 다시 맞출 수 없게 만든다. 건너뛰지 않고 링크를
// 사용 불가 상태로 표시한다.
void framing_rejects_oversized_frames() {
    FrameAssembler assembler(16);
    std::vector<std::byte> stream;
    for (std::size_t i = 0; i < length_prefix_bytes; ++i) {
        stream.push_back(static_cast<std::byte>((1000u >> (8 * i)) & 0xFF));
    }
    check(assembler.push(stream), "push oversized header");
    std::span<const std::byte> frame;
    check(assembler.next_frame(frame) == FrameResult::too_large, "oversized frame must be rejected");
    check(assembler.poisoned(), "link must be marked unusable");
    check(!assembler.push(stream), "a poisoned assembler accepts no more bytes");

    std::vector<std::byte> out;
    check(!write_frame(out, payload_of(17, std::byte{1}), 16), "sender must refuse oversized frame");
    check(out.empty(), "refused frame must write nothing");
}

// 오래 유지되는 연결에서 버퍼에 쌓인 바이트가 무한히 늘어나면 안 된다.
void framing_reclaims_buffer() {
    FrameAssembler assembler(64);
    for (int round = 0; round < 1000; ++round) {
        std::vector<std::byte> stream;
        check(write_frame(stream, payload_of(32, std::byte{0x5A}), 64), "write");
        check(assembler.push(stream), "push");
        std::span<const std::byte> frame;
        check(assembler.next_frame(frame) == FrameResult::frame, "frame");
        check(frame.size() == 32, "frame size");
    }
    check(assembler.buffered() == 0, "consumed bytes must be reclaimed");
}

// 모든 읽기를 쪼개는 링크 위에서 같은 인코딩·디코딩 경로를 지나간다. 프레이밍과
// 메시지 디코딩의 조합을 끝에서 끝까지 확인하기 위해서다.
void loopback_delivers_messages() {
    LoopbackConfig config;
    config.chunk_bytes = 3; // 모든 프레임이 쪼개져 도착하도록 강제한다.
    LoopbackLink link(config);

    const Handshake hello{wire_version, 777, 9};
    const auto hello_bytes = encoded(hello);
    check(link.first().send(hello_bytes) == SendStatus::sent, "send handshake");

    std::vector<std::vector<std::byte>> received;
    while (link.pump() != 0) {
        link.second().receive(received, 8);
    }
    link.second().receive(received, 8);
    check(received.size() == 1, "exactly one frame delivered");

    Reader reader(received[0]);
    Handshake out;
    check(decode(reader, out) && reader.done(), "decode after split delivery");
    check(out.client_nonce == hello.client_nonce, "value survived split delivery");
}

// 느린 수신자 때문에 한 상대가 무한정 메모리를 쓰게 두면 안 된다.
void loopback_applies_backpressure() {
    LoopbackConfig config;
    config.send_buffer_bytes = 64;
    LoopbackLink link(config);

    int sent = 0;
    SendStatus status = SendStatus::sent;
    while (sent < 100) {
        status = link.first().send(payload_of(16, std::byte{1}));
        if (status != SendStatus::sent) {
            break;
        }
        ++sent;
    }
    check(status == SendStatus::would_block, "full send buffer must report would_block");
    check(sent > 0 && sent < 100, "some frames sent before the limit");

    check(link.first().send(payload_of(70 * 1024, std::byte{1})) == SendStatus::too_large,
          "oversized payload must be refused by the link");
}

// 전송 중 바이트 유실은 거절된 프레임으로 드러나야 하며, 잘못된 값으로 디코딩되는
// 메시지가 되어서는 안 된다.
void loopback_detects_corrupted_stream() {
    LoopbackConfig config;
    config.max_payload = 128;
    LoopbackLink link(config);

    check(link.first().send(encoded(Ping{1})) == SendStatus::sent, "send ping");
    // 길이 접두사에서 한 바이트를 빼 이후의 모든 경계가 밀리게 한다.
    link.drop_from_first(1);
    check(link.first().send(encoded(Ping{2})) == SendStatus::sent, "send second ping");
    while (link.pump() != 0) {
    }

    std::vector<std::vector<std::byte>> received;
    link.second().receive(received, 8);
    for (const auto& frame : received) {
        Reader reader(frame);
        Ping out;
        const bool ok = decode(reader, out) && reader.done();
        check(!ok || out.sent_at_us == 1 || out.sent_at_us == 2,
              "a decoded frame must carry a value that was actually sent");
    }
    check(received.size() < 2 || link.second().faulted(),
          "a corrupted stream must lose or fault, not fabricate frames");
}

} // 익명 네임스페이스 끝

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"wire_layout", wire_layout},
        {"writer_bounds", writer_bounds},
        {"truncation", reader_rejects_truncation},
        {"trailing", reader_rejects_trailing_bytes},
        {"unknown_values", unknown_values_rejected},
        {"round_trip", all_messages_round_trip},
        {"framing_split", framing_reassembles_split_streams},
        {"framing_oversized", framing_rejects_oversized_frames},
        {"framing_buffer", framing_reclaims_buffer},
        {"loopback_delivery", loopback_delivers_messages},
        {"loopback_backpressure", loopback_applies_backpressure},
        {"loopback_corruption", loopback_detects_corrupted_stream}};
    try {
        check(argc == 2, "provide a test case name");
        for (const auto& [name, test] : cases) {
            if (name == argv[1]) {
                test();
                std::cout << "PASS " << name << '\n';
                return 0;
            }
        }
        throw std::runtime_error("unknown test case");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
