#pragma once

// 식별자마다 별도의 정수 타입을 둔다.
//
// 세션·엔티티·파티 id가 모두 64비트 정수라, 하나를 다른 자리에 넘겨도 컴파일이
// 통과하고 실행 중에 오동작한다. 각각 타입을 나눠 그 혼동을 컴파일 오류로 만든다.

#include <compare>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace mo::protocol {

template <typename Tag, typename Rep>
class StrongId {
public:
    using rep = Rep;

    constexpr StrongId() = default;
    constexpr explicit StrongId(Rep value) : value_(value) {}

    constexpr Rep raw() const { return value_; }
    // 0은 "설정되지 않음"으로 예약한다. 0으로 채운 버퍼가 유효한 id가 되지 않는다.
    constexpr bool valid() const { return value_ != Rep{}; }

    friend constexpr bool operator==(StrongId, StrongId) = default;
    friend constexpr auto operator<=>(StrongId, StrongId) = default;

private:
    Rep value_{};
};

template <typename T>
struct is_strong_id : std::false_type {};

template <typename Tag, typename Rep>
struct is_strong_id<StrongId<Tag, Rep>> : std::true_type {};

template <typename T>
inline constexpr bool is_strong_id_v = is_strong_id<T>::value;

using SessionId = StrongId<struct SessionIdTag, std::uint64_t>;
using EntityId = StrongId<struct EntityIdTag, std::uint64_t>;
using PartyId = StrongId<struct PartyIdTag, std::uint64_t>;

} // mo::protocol 네임스페이스 끝

template <typename Tag, typename Rep>
struct std::hash<mo::protocol::StrongId<Tag, Rep>> {
    std::size_t operator()(mo::protocol::StrongId<Tag, Rep> id) const noexcept {
        return std::hash<Rep>{}(id.raw());
    }
};
