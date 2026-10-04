#pragma once

// 연속값은 float이나 double이 아니라 정수로 보낸다.
//
// 이유는 셋이며, 피해가 큰 순서다.
//
// 1. NaN이 검증을 통과한다. NaN과의 비교는 모두 거짓이라
//    `if (x < min || x > max) reject;` 같은 검사를 그냥 지나가고, 그 뒤로 이 값이
//    닿는 계산이 전부 NaN이 된다.
// 2. 서버 판정과 클라이언트 예측이 같은 결과에 도달해야 한다. 부동소수 연산은
//    컴파일러와 최적화 옵션에 따라 마지막 비트가 달라질 수 있다.
// 3. float 정밀도의 대부분은 화면에서 보이지 않으면서 snapshot마다 대역폭을 쓴다.
//
// 아래의 스케일은 상수가 아니라 인자다. 1미터를 몇 정수 단위로 표현할지는 전송 계약의 일부다.
// 배포 후에도 바꿀 수 있지만, 기존 버전의 의미를 몰래 바꾸면 같은 숫자가 다른 위치로 해석된다.
// 변경 시 규약 버전을 갱신하고 클라이언트도 함께 갱신하거나 별도 호환 처리를 구현해야 한다.
// 현재는 버전이 다르면 거절하므로, 전투 공간을 정한 뒤 첫 게임 메시지의 스케일을 결정한다.

#include <cmath>
#include <cstdint>
#include <limits>

namespace mo::protocol {

// NaN과 무한대를 거절한다. 상대에게서 받은 값은 게임 로직에 닿기 전에 이 검사를
// 통과해야 한다.
inline bool finite(double value) { return std::isfinite(value); }

// 값을 1/units_per_unit 격자에 맞춰 반올림하고, 결과가 요청한 정수 범위에 들어가는지
// 알려준다. 범위를 벗어난 입력은 프로토콜 오류이며 잘라 맞출 대상이 아니다.
// 잘라 맞추면 플레이어를 말없이 옮기게 된다.
inline bool quantize(double value, double units_per_unit, std::int64_t min, std::int64_t max,
                     std::int64_t& out) {
    if (!finite(value) || !finite(units_per_unit) || units_per_unit <= 0.0) {
        return false;
    }
    const double scaled = std::nearbyint(value * units_per_unit);
    // static_cast<double>(max)와 비교하면 안전하지 않다. INT64_MAX는 double로 정확히
    // 표현되지 않아 2^63으로 반올림되므로, 변환할 수 없는 값이 검사를 통과하고 이어지는
    // 캐스팅이 정의되지 않은 동작이 된다. 먼저 int64로 정확히 표현되는 경계로 자르고,
    // 호출자가 지정한 범위는 정수 공간에서 비교한다.
    constexpr double int64_low = -9223372036854775808.0;  // -2^63, 정확히 표현됨
    constexpr double int64_high = 9223372036854775808.0;  // 2^63, 정확히 표현됨, 배타적
    if (!finite(scaled) || scaled < int64_low || scaled >= int64_high) {
        return false;
    }
    const auto rounded = static_cast<std::int64_t>(scaled);
    if (rounded < min || rounded > max) {
        return false;
    }
    out = rounded;
    return true;
}

inline double dequantize(std::int64_t raw, double units_per_unit) {
    return static_cast<double>(raw) / units_per_unit;
}

// 각도는 순환하므로 범위 검사가 필요 없다. 16비트 값 전부가 유효한 방향이다.
// 0~65535가 한 바퀴에 대응하며 한 눈금이 약 0.0055도다.
inline std::uint16_t quantize_turn(double degrees) {
    if (!finite(degrees)) {
        return 0;
    }
    const double turns = degrees / 360.0;
    const double wrapped = turns - std::floor(turns);
    const auto scaled = static_cast<std::int64_t>(std::nearbyint(wrapped * 65536.0));
    return static_cast<std::uint16_t>(scaled & 0xFFFF);
}

inline double dequantize_turn(std::uint16_t raw) {
    return static_cast<double>(raw) * 360.0 / 65536.0;
}

} // mo::protocol 네임스페이스 끝

// 약어와 용어 설명
// NaN(Not a Number): 정상적인 수치로 표현할 수 없는 부동소수 값. 비교만으로 범위를 검사하면 놓칠 수 있다.
// 양자화(Quantization): 연속값을 정해진 간격의 정수로 표현하는 것. 예: 1.25미터를 125로 표현.
// 스케일(Scale): 실제 값 1단위에 대응하는 정수 단위 수. 위 예에서 1미터당 100단위.
// 스냅샷(Snapshot): 특정 시점의 게임 상태를 전달하는 데이터.
// 규약 버전(Wire Version): 전송 바이트를 해석하는 규칙의 버전. 의미가 바뀌면 호환 정책도 바뀌어야 한다.
