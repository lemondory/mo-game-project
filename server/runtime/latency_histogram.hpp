#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mo {

namespace histogram_layout {

inline constexpr std::uint64_t linear_limit_us = 32;
inline constexpr std::size_t sub_buckets = 16;
inline constexpr std::uint64_t max_trackable_us = 60ull * 1000 * 1000;

// 선형 한계 미만은 1us마다 버킷을 하나씩 둔다. 그 위로는 2의 거듭제곱 구간마다
// sub_buckets개로 나누므로 버킷 폭은 최대 6.25%다.
constexpr std::size_t bucket_index(std::uint64_t value_us) {
    if (value_us < linear_limit_us) {
        return static_cast<std::size_t>(value_us);
    }
    const auto exponent = static_cast<std::size_t>(std::bit_width(value_us) - 1);
    const std::size_t shift = exponent - 4;
    const auto sub = static_cast<std::size_t>((value_us >> shift) & (sub_buckets - 1));
    return static_cast<std::size_t>(linear_limit_us) + (shift - 1) * sub_buckets + sub;
}

constexpr std::uint64_t bucket_upper_us(std::size_t index) {
    if (index < linear_limit_us) {
        return index;
    }
    const std::size_t offset = index - static_cast<std::size_t>(linear_limit_us);
    const std::size_t shift = offset / sub_buckets + 1;
    const auto sub = static_cast<std::uint64_t>(offset % sub_buckets);
    return ((sub_buckets + sub + 1) << shift) - 1;
}

inline constexpr std::size_t bucket_count = bucket_index(max_trackable_us) + 1;

} // histogram_layout 네임스페이스 끝

// 마이크로초 단위의 고정 크기 로그-선형 지연 히스토그램이다. 메모리를 할당하지 않고
// 잠금도 쓰지 않는다.
//
// 보고하는 분위수는 표본이 속한 버킷의 상한이다. 실제 값보다 크거나 같고 오차는
// 6.25% 이내이며, 정확한 표본값이 아니다. max_trackable_us를 넘는 표본은 마지막
// 버킷에 넣되 개수를 따로 세어, 보고서가 그 존재를 감추지 못하게 한다.
// 해상도 하한은 1us이며 그보다 짧은 작업은 0으로 기록한다.
class Histogram {
public:
    static constexpr std::size_t bucket_count = histogram_layout::bucket_count;
    static constexpr std::uint64_t max_trackable_us = histogram_layout::max_trackable_us;

    void record(std::chrono::steady_clock::duration value) {
        const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(value).count();
        record_us(ticks <= 0 ? 0 : static_cast<std::uint64_t>(ticks));
    }

    void record_us(std::uint64_t value_us) {
        if (value_us > max_trackable_us) {
            ++overflows_;
            value_us = max_trackable_us;
        }
        ++buckets_[histogram_layout::bucket_index(value_us)];
        ++count_;
        sum_us_ += value_us;
        min_us_ = count_ == 1 || value_us < min_us_ ? value_us : min_us_;
        max_us_ = value_us > max_us_ ? value_us : max_us_;
    }

    void merge(const Histogram& other) {
        if (other.count_ == 0) {
            return;
        }
        for (std::size_t i = 0; i < bucket_count; ++i) {
            buckets_[i] += other.buckets_[i];
        }
        min_us_ = count_ == 0 || other.min_us_ < min_us_ ? other.min_us_ : min_us_;
        max_us_ = other.max_us_ > max_us_ ? other.max_us_ : max_us_;
        count_ += other.count_;
        sum_us_ += other.sum_us_;
        overflows_ += other.overflows_;
    }

    // 요청한 분위수가 속한 버킷의 상한. 표본이 없으면 0이다.
    std::uint64_t percentile_us(double percentile) const {
        if (count_ == 0) {
            return 0;
        }
        // 요청한 비율 이상을 포함하는 가장 작은 값이므로 순위는 올림한다.
        // 표본 38개의 p99.9는 37번째가 아니라 가장 큰 값이다.
        const double clamped = percentile < 0.0 ? 0.0 : (percentile > 100.0 ? 100.0 : percentile);
        auto wanted = static_cast<std::uint64_t>(
            std::ceil(static_cast<double>(count_) * clamped / 100.0));
        wanted = wanted == 0 ? 1 : (wanted > count_ ? count_ : wanted);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < bucket_count; ++i) {
            seen += buckets_[i];
            if (seen >= wanted) {
                const auto upper = histogram_layout::bucket_upper_us(i);
                return upper > max_us_ ? max_us_ : upper;
            }
        }
        return max_us_;
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t min_us() const { return count_ == 0 ? 0 : min_us_; }
    std::uint64_t max_us() const { return max_us_; }
    std::uint64_t sum_us() const { return sum_us_; }
    std::uint64_t overflows() const { return overflows_; }
    double mean_us() const {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_us_) / static_cast<double>(count_);
    }

private:
    std::array<std::uint64_t, bucket_count> buckets_{};
    std::uint64_t count_{};
    std::uint64_t sum_us_{};
    std::uint64_t min_us_{};
    std::uint64_t max_us_{};
    std::uint64_t overflows_{};
};

} // mo 네임스페이스 끝
