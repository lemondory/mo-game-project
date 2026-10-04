#pragma once

// 메시지마다 필드 목록을 하나만 두고 읽기와 쓰기가 같은 목록을 쓴다.
//
// encode()와 decode()를 손으로 따로 쓰면 목록 두 벌을 맞춰 유지해야 한다. 한쪽에서만
// 필드 타입을 바꿔도 컴파일은 통과하고 실행 중에 조용히 잘못 읽는다. 여기서는 각
// 메시지가 필드를 한 번 선언하고 양방향이 그 목록을 따라가므로 어긋날 수 없다.

#include "ids.hpp"
#include "wire.hpp"

#include <type_traits>

namespace mo::protocol {

// 전송되는 enum은 신뢰할 수 없는 상대가 보낸 정수다. 모르는 값을 enum으로 캐스팅하면
// 이후의 모든 분기가 예측 불가가 되므로, 유효한 값을 선언하고 검사해야 한다.
//
// 기본 템플릿은 일부러 정의하지 않았다. 유효 값 목록 없이 메시지에 enum을 추가하면
// 아무 값이나 받아들이는 대신 컴파일이 실패한다.
template <typename T>
struct enum_traits;

namespace detail {

template <typename T>
bool put_integer(Writer& writer, T value) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "wire integers are fixed width; bool has no defined size");
    if constexpr (sizeof(T) == 1) {
        return writer.u8(static_cast<std::uint8_t>(value));
    } else if constexpr (sizeof(T) == 2) {
        return writer.u16(static_cast<std::uint16_t>(value));
    } else if constexpr (sizeof(T) == 4) {
        return writer.u32(static_cast<std::uint32_t>(value));
    } else {
        static_assert(sizeof(T) == 8, "unsupported integer width");
        return writer.u64(static_cast<std::uint64_t>(value));
    }
}

template <typename T>
bool get_integer(Reader& reader, T& out) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "wire integers are fixed width; bool has no defined size");
    if constexpr (sizeof(T) == 1) {
        std::uint8_t raw{};
        if (!reader.u8(raw)) {
            return false;
        }
        out = static_cast<T>(raw);
    } else if constexpr (sizeof(T) == 2) {
        std::uint16_t raw{};
        if (!reader.u16(raw)) {
            return false;
        }
        out = static_cast<T>(raw);
    } else if constexpr (sizeof(T) == 4) {
        std::uint32_t raw{};
        if (!reader.u32(raw)) {
            return false;
        }
        out = static_cast<T>(raw);
    } else {
        static_assert(sizeof(T) == 8, "unsupported integer width");
        std::uint64_t raw{};
        if (!reader.u64(raw)) {
            return false;
        }
        out = static_cast<T>(raw);
    }
    return true;
}

} // detail 네임스페이스 끝

// 보낼 때도 검사한다. 잘못된 enum을 만드는 버그는 상대가 거절해야 할 값을 내보내기
// 전에 여기서 걸려야 한다.
template <typename T>
bool put_enum(Writer& writer, T value) {
    static_assert(std::is_enum_v<T>);
    using Underlying = std::underlying_type_t<T>;
    for (const auto known : enum_traits<T>::values) {
        if (known == value) {
            return detail::put_integer(writer, static_cast<Underlying>(value));
        }
    }
    return false;
}

template <typename T>
bool get_enum(Reader& reader, T& out) {
    static_assert(std::is_enum_v<T>);
    using Underlying = std::underlying_type_t<T>;
    Underlying raw{};
    if (!detail::get_integer(reader, raw)) {
        return false;
    }
    for (const auto known : enum_traits<T>::values) {
        if (static_cast<Underlying>(known) == raw) {
            out = known;
            return true;
        }
    }
    return false; // 모르는 값이다. 추측하지 말고 프레임을 버린다.
}

template <typename T>
bool put(Writer& writer, const T& value) {
    if constexpr (std::is_enum_v<T>) {
        return put_enum(writer, value);
    } else if constexpr (is_strong_id_v<T>) {
        return detail::put_integer(writer, value.raw());
    } else {
        return detail::put_integer(writer, value);
    }
}

template <typename T>
bool get(Reader& reader, T& out) {
    if constexpr (std::is_enum_v<T>) {
        return get_enum(reader, out);
    } else if constexpr (is_strong_id_v<T>) {
        typename T::rep raw{};
        if (!detail::get_integer(reader, raw)) {
            return false;
        }
        out = T{raw};
        return true;
    } else {
        return detail::get_integer(reader, out);
    }
}

// 첫 실패에서 멈춘다. 짧은 프레임을 계속 읽지 않기 위해서다.
class FieldWriter {
public:
    explicit FieldWriter(Writer& writer) : writer_(writer) {}
    template <typename T>
    void operator()(const T& value) {
        if (ok_) {
            ok_ = put(writer_, value);
        }
    }
    bool ok() const { return ok_; }

private:
    Writer& writer_;
    bool ok_{true};
};

class FieldReader {
public:
    explicit FieldReader(Reader& reader) : reader_(reader) {}
    template <typename T>
    void operator()(T& value) {
        if (ok_) {
            ok_ = get(reader_, value);
        }
    }
    bool ok() const { return ok_; }

private:
    Reader& reader_;
    bool ok_{true};
};

// 메시지는 다음과 같이 선언한다.
//     template <typename Self, typename V>
//     static void fields(Self& self, V& visit) { visit(self.a); visit(self.b); }
// static으로 둬야 목록 하나가 const인 원본과 수정 가능한 대상 양쪽에 쓰인다.
template <typename T>
bool encode(Writer& writer, const T& value) {
    FieldWriter visitor(writer);
    std::remove_cvref_t<T>::fields(value, visitor);
    return visitor.ok();
}

template <typename T>
bool decode(Reader& reader, T& out) {
    FieldReader visitor(reader);
    std::remove_cvref_t<T>::fields(out, visitor);
    return visitor.ok();
}

} // mo::protocol 네임스페이스 끝
