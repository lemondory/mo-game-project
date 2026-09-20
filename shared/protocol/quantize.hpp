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
// 아래의 스케일은 상수가 아니라 인자다. 1미터를 몇 단위로 볼지는 전송 계약의
// 일부여서 클라이언트 배포 후에는 바꿀 수 없다. 지금 추측하지 않고 전투 공간이
// 정해진 뒤에 결정한다.

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
    if (!finite(scaled) || scaled < static_cast<double>(min) ||
        scaled > static_cast<double>(max)) {
        return false;
    }
    out = static_cast<std::int64_t>(scaled);
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
