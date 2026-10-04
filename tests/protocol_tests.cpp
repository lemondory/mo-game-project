#include "codec.hpp"
#include "framing.hpp"
#include "ids.hpp"
#include "loopback.hpp"
#include "message.hpp"
#include "quantize.hpp"
#include "wire.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
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
        Reader reader{std::span{full}.first(length)};
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
    check(writer.u16(9999), "write unknown id");
    Reader reader(writer.view());
    Header header;
    check(!decode(reader, header), "unknown message id must be refused");

    std::array<std::byte, 16> ack_storage{};
    Writer ack_writer(ack_storage);
    check(ack_writer.u8(200) && ack_writer.u16(wire_version) && ack_writer.u64(1), "write bad status");
    Reader ack_reader(ack_writer.view());
    HandshakeAck ack;
    check(!decode(ack_reader, ack), "unknown handshake status must be refused");

    // 위의 한 값만이 아니라 선언한 집합 밖의 모든 값이 거절되어야 한다.
    for (int raw = 0; raw < 256; ++raw) {
        std::array<std::byte, 16> probe{};
        Writer probe_writer(probe);
        check(probe_writer.u8(static_cast<std::uint8_t>(raw)) && probe_writer.u16(1) &&
                  probe_writer.u64(1),
              "write probe");
        Reader probe_reader(probe_writer.view());
        HandshakeAck out;
        const bool accepted = decode(probe_reader, out);
        check(accepted == (raw <= 3), "only declared handshake status values may decode");
    }

    // 선언한 적 없는 값은 전송 경로에도 올라가면 안 된다.
    std::array<std::byte, 8> out_storage{};
    Writer out_writer(out_storage);
    check(!put_enum(out_writer, static_cast<HandshakeStatus>(200)),
          "an undeclared enum value must not be encoded");
}

void all_messages_round_trip() {
    const Header header{MessageId::pong};
    // 버퍼를 변수에 묶는다. Reader가 빌려 쓰므로 임시 객체보다 오래 살면 안 된다.
    const auto header_bytes = encoded(header);
    check(header_bytes.size() == 2, "the header carries the id only, not a per-packet version");
    Reader header_reader(header_bytes);
    Header header_out;
    check(decode(header_reader, header_out) && header_reader.done(), "header round trip");
    check(header_out.id == header.id, "header values");

    const HandshakeAck ack{HandshakeStatus::version_mismatch, 7, SessionId{0x1234567890ABCDEFull}};
    auto ack_bytes = encoded(ack);
    Reader ack_reader(ack_bytes);
    HandshakeAck ack_out;
    check(decode(ack_reader, ack_out) && ack_reader.done(), "ack round trip");
    check(ack_out.status == ack.status && ack_out.server_version == ack.server_version &&
              ack_out.session == ack.session,
          "ack values");

    const Pong pong{111, 222};
    const auto pong_bytes = encoded(pong);
    Reader pong_reader(pong_bytes);
    Pong pong_out;
    check(decode(pong_reader, pong_out) && pong_reader.done(), "pong round trip");
    check(pong_out.echoed_sent_at_us == 111 && pong_out.replied_at_us == 222, "pong values");
}

// id가 모두 64비트 정수라, 타입을 나눠야 하나를 다른 자리에 넘기는 것을 막는다.
void strong_ids_survive_round_trip() {
    static_assert(!std::is_convertible_v<SessionId, EntityId>,
                  "distinct id types must not convert into each other");
    static_assert(!std::is_convertible_v<std::uint64_t, SessionId>,
                  "a raw integer must not become an id implicitly");
    check(!SessionId{}.valid(), "a zeroed id must not read as a live id");
    check(SessionId{1}.valid(), "a non-zero id is live");

    const HandshakeAck ack{HandshakeStatus::accepted, wire_version, SessionId{0xFFFFFFFFFFFFFFFFull}};
    const auto bytes = encoded(ack);
    Reader reader(bytes);
    HandshakeAck out;
    check(decode(reader, out) && reader.done(), "ack with max id decodes");
    check(out.session == ack.session, "id survives the round trip");
}

// 연속값은 정수로 전송한다. NaN과 무한대는 비교를 오염시키기 전에 거절한다.
void quantization_rejects_bad_values() {
    std::int64_t out = 0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    check(!mo::protocol::finite(nan) && !mo::protocol::finite(inf),
          "NaN and infinity are not finite");
    check(!quantize(nan, 1000.0, -1000000, 1000000, out), "NaN must not quantize");
    check(!quantize(inf, 1000.0, -1000000, 1000000, out), "infinity must not quantize");
    check(!quantize(1.0, 0.0, -1000, 1000, out), "a non-positive scale is a programming error");

    // 범위를 벗어난 값은 프로토콜 오류이며 잘라 맞출 대상이 아니다. 잘라 맞추면 그
    // 값이 가리키는 대상을 말없이 옮기게 된다.
    check(!quantize(5000.0, 1000.0, -1000000, 1000000, out), "out of range must fail");
    check(quantize(1.2345, 1000.0, -1000000, 1000000, out), "in range succeeds");
    check(out == 1234 || out == 1235, "value lands on the grid");
    check(std::abs(dequantize(out, 1000.0) - 1.2345) < 0.001, "round trip within one step");

    // 각도는 순환하므로 16비트 값 전부가 유효한 방향이다.
    check(quantize_turn(0.0) == 0, "zero degrees");
    check(quantize_turn(360.0) == 0, "a full turn wraps to zero");
    check(quantize_turn(-90.0) == quantize_turn(270.0), "negative angles wrap");
    for (double degrees = 0.0; degrees < 360.0; degrees += 7.5) {
        const auto raw = quantize_turn(degrees);
        check(std::abs(dequantize_turn(raw) - degrees) < 0.01, "angle round trip");
    }
    check(quantize_turn(nan) == 0, "NaN angle falls back instead of producing garbage");

    // INT64_MAX는 double로 정확히 표현되지 않아 2^63으로 반올림되므로, double 공간에서
    // 비교하면 변환할 수 없는 값이 통과했다.
    constexpr auto int64_max = std::numeric_limits<std::int64_t>::max();
    constexpr auto int64_min = std::numeric_limits<std::int64_t>::min();
    check(!quantize(9223372036854775808.0, 1.0, int64_min, int64_max, out),
          "2^63 is outside int64 and must be refused");
    // 2^63 부근의 double은 간격이 2048이라, -2^63 바로 아래의 표현 가능한 값은
    // -2^63 - 2048이다. -2^63 - 1은 다시 -2^63으로 반올림된다.
    check(!quantize(-9223372036854777856.0, 1.0, int64_min, int64_max, out),
          "the first representable value below -2^63 must be refused");
    check(!quantize(1e30, 1.0, int64_min, int64_max, out), "far out of range must be refused");
    check(quantize(-9223372036854775808.0, 1.0, int64_min, int64_max, out) &&
              out == int64_min,
          "the exactly representable lower edge is accepted");
    check(!quantize(1e300, 1e300, int64_min, int64_max, out),
          "an overflowing product must be refused");
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

// 프레이밍은 경계를 복원한다. 손상은 탐지하지 못하며 테스트도 그렇게 말해야 한다.
// 길이 접두사는 프레임이 어디서 끝나는지 알려줄 뿐 내용이 온전한지는 말하지 않는다.
// 무결성은 보안 계층에서 온다.
void framing_does_not_provide_integrity() {
    LoopbackConfig config;
    config.max_payload = 128;
    config.chunk_bytes = 4;
    LoopbackLink link(config);

    const auto first_ping = encoded(Ping{1});
    check(link.first().send(first_ping) == SendStatus::sent, "send first ping");
    link.pump();              // 4바이트 길이 접두사만 전달한다.
    link.drop_from_first(1);  // payload의 첫 바이트를 잃게 한다.
    const auto second_ping = encoded(Ping{2});
    check(link.first().send(second_ping) == SendStatus::sent, "send second ping");
    while (link.pump() != 0) {
    }

    std::vector<std::vector<std::byte>> received;
    link.second().receive(received, 8);

    bool fabricated = false;
    for (const auto& frame : received) {
        Reader reader(frame);
        Ping out;
        if (decode(reader, out) && reader.done() && out.sent_at_us != 1 && out.sent_at_us != 2) {
            fabricated = true;
        }
    }
    // 문서로 남긴 한계를 단언한다. 나중에 누구도 프레이밍을 무결성으로 착각하지 않게
    // 하기 위해서다. 사라진 바이트가 이후의 모든 경계를 밀어내고, 다음 프레임의 길이
    // 접두사가 payload로 읽히며, 보낸 적 없는 값이 정상적으로 디코딩된다.
    check(fabricated, "framing alone cannot detect a lost payload byte");
}

// 송신 버퍼 상한만으로는 부족하다. 수신 쪽 상한이 없으면 옮기는 동작이 송신 버퍼를
// 비워줄 뿐이고, 바이트는 반대편에 계속 쌓여 프로세스가 메모리를 다 쓸 때까지 간다.
void receive_buffer_is_bounded() {
    FrameAssembler assembler(16, 64);
    check(assembler.capacity() == 64, "capacity starts at the configured bound");
    const std::vector<std::byte> too_much(65, std::byte{1});
    check(!assembler.push(too_much), "a push beyond capacity must be refused");
    check(assembler.buffered() == 0, "a refused push must consume nothing");

    LoopbackConfig config;
    config.max_payload = 16;
    config.send_buffer_bytes = 64;
    config.receive_buffer_bytes = 64;
    LoopbackLink link(config);

    int accepted = 0;
    for (int i = 0; i < 256; ++i) {
        if (link.first().send(payload_of(16, std::byte{1})) != SendStatus::sent) {
            break;
        }
        ++accepted;
        link.pump(); // 상대는 receive()를 부르지 않는다.
    }
    check(accepted < 256, "a peer that never reads must eventually stall the sender");
    check(!link.second().faulted(), "backpressure is not a fault; the link stays usable");

    // 비워주면 다시 흐른다. 이 점이 연결을 끊는 것과 역압을 가르는 차이다.
    std::vector<std::vector<std::byte>> received;
    check(link.second().receive(received, 100) > 0, "buffered frames are readable");
    link.pump();
    check(link.first().send(payload_of(16, std::byte{1})) == SendStatus::sent,
          "sending resumes once the reader catches up");
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
        {"strong_ids", strong_ids_survive_round_trip},
        {"quantization", quantization_rejects_bad_values},
        {"framing_split", framing_reassembles_split_streams},
        {"framing_oversized", framing_rejects_oversized_frames},
        {"framing_buffer", framing_reclaims_buffer},
        {"loopback_delivery", loopback_delivers_messages},
        {"loopback_backpressure", loopback_applies_backpressure},
        {"loopback_integrity_limit", framing_does_not_provide_integrity},
        {"receive_bound", receive_buffer_is_bounded}};
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
