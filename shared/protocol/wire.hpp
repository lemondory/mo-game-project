#pragma once

// 전송 바이트를 읽고 쓰는 경계 검사 기본 도구다.
//
// 모든 읽기가 남은 길이를 확인하므로, 잘렸거나 악의적인 프레임은 버퍼 밖을 읽지 않고
// 첫 번째 짧은 필드에서 실패한다. 여기서는 메모리를 할당하지 않고, 잘못된 입력에
// 예외를 던지지도 않는다. 호출자가 반환값을 확인하고 프레임을 버린다.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mo::protocol {

// 고정 리틀엔디안으로 인코딩해 서로 다른 기기에서 뜬 캡처와 덤프를 비교할 수 있게 한다.
// 구조체를 통째로 복사하는 방식으로 바꾸지 않는다. 패딩과 호스트 바이트 순서가
// 전송 형식에 새어 들어간다.
class Writer {
public:
    explicit Writer(std::span<std::byte> buffer) : buffer_(buffer) {}

    bool u8(std::uint8_t value) { return raw(&value, 1); }
    bool u16(std::uint16_t value) { return integer(value); }
    bool u32(std::uint32_t value) { return integer(value); }
    bool u64(std::uint64_t value) { return integer(value); }
    bool i32(std::int32_t value) { return integer(static_cast<std::uint32_t>(value)); }

    bool bytes(std::span<const std::byte> value) {
        return raw(value.data(), value.size());
    }

    std::size_t written() const { return written_; }
    bool overflowed() const { return overflowed_; }
    std::span<const std::byte> view() const { return buffer_.first(written_); }

private:
    template <typename T>
    bool integer(T value) {
        std::byte encoded[sizeof(T)];
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            encoded[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
        }
        return raw(encoded, sizeof(T));
    }

    bool raw(const void* source, std::size_t size) {
        if (written_ + size > buffer_.size()) {
            overflowed_ = true;
            return false;
        }
        if (size != 0) {
            std::memcpy(buffer_.data() + written_, source, size);
            written_ += size;
        }
        return true;
    }

    std::span<std::byte> buffer_;
    std::size_t written_{};
    bool overflowed_{};
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> buffer) : buffer_(buffer) {}

    bool u8(std::uint8_t& out) { return raw(&out, 1); }
    bool u16(std::uint16_t& out) { return integer(out); }
    bool u32(std::uint32_t& out) { return integer(out); }
    bool u64(std::uint64_t& out) { return integer(out); }
    bool i32(std::int32_t& out) {
        std::uint32_t value{};
        if (!integer(value)) {
            return false;
        }
        out = static_cast<std::int32_t>(value);
        return true;
    }

    // 호출자의 버퍼를 빌려 쓴다. 그 버퍼가 살아 있는 동안에만 유효하다.
    bool bytes(std::size_t size, std::span<const std::byte>& out) {
        if (remaining() < size) {
            failed_ = true;
            return false;
        }
        out = buffer_.subspan(read_, size);
        read_ += size;
        return true;
    }

    std::size_t read() const { return read_; }
    std::size_t remaining() const { return buffer_.size() - read_; }
    bool failed() const { return failed_; }
    // 남는 바이트가 있는 프레임은 잘못된 것이다. 보낸 쪽과 받는 쪽이 메시지를 다르게
    // 알고 있다는 뜻이라, 받아들이면 그 불일치를 덮게 된다.
    bool done() const { return !failed_ && remaining() == 0; }

private:
    template <typename T>
    bool integer(T& out) {
        if (remaining() < sizeof(T)) {
            failed_ = true;
            return false;
        }
        T value{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<T>(std::to_integer<std::uint8_t>(buffer_[read_ + i])) << (8 * i);
        }
        read_ += sizeof(T);
        out = value;
        return true;
    }

    bool raw(void* destination, std::size_t size) {
        if (remaining() < size) {
            failed_ = true;
            return false;
        }
        std::memcpy(destination, buffer_.data() + read_, size);
        read_ += size;
        return true;
    }

    std::span<const std::byte> buffer_;
    std::size_t read_{};
    bool failed_{};
};

} // mo::protocol 네임스페이스 끝
