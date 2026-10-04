#include "framing.hpp"

#include <algorithm>
#include <stdexcept>

namespace mo::transport {

namespace {

std::uint32_t read_length(std::span<const std::byte> bytes) {
    std::uint32_t value{};
    for (std::size_t i = 0; i < length_prefix_bytes; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
    }
    return value;
}

} // 익명 네임스페이스 끝

FrameAssembler::FrameAssembler(std::uint32_t max_payload, std::size_t max_buffered)
    : max_payload_(max_payload), max_buffered_(max_buffered) {
    if (max_payload == 0) {
        throw std::invalid_argument("max_payload must be positive");
    }
    const std::size_t one_frame = length_prefix_bytes + max_payload;
    if (max_buffered_ == 0) {
        // 한 번 읽기에 프레임 몇 개가 함께 와도 막히지 않을 만큼 여유를 두면서,
        // 계속 보내기만 하는 상대에게는 상한이 걸리게 한다.
        max_buffered_ = one_frame * 4;
    }
    if (max_buffered_ < one_frame) {
        // 그렇지 않으면 정상 프레임 하나도 조립할 수 없어, 여기서 실패하는 대신
        // 연결이 영원히 멈춘다.
        throw std::invalid_argument("max_buffered must hold at least one full frame");
    }
}

std::size_t FrameAssembler::capacity() const {
    if (poisoned_) {
        return 0;
    }
    const auto held = buffered();
    return held >= max_buffered_ ? 0 : max_buffered_ - held;
}

bool FrameAssembler::push(std::span<const std::byte> bytes) {
    if (poisoned_ || bytes.size() > capacity()) {
        return false;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return true;
}

FrameResult FrameAssembler::next_frame(std::span<const std::byte>& out) {
    if (poisoned_) {
        return FrameResult::too_large;
    }
    compact();
    const std::span<const std::byte> available(buffer_.data() + consumed_, buffer_.size() - consumed_);
    if (available.size() < length_prefix_bytes) {
        return FrameResult::incomplete;
    }
    const auto length = read_length(available);
    if (length > max_payload_) {
        // 잘못된 길이를 본 순간 다음 프레임의 경계를 알 수 없으므로, 다시 이어갈
        // 안전한 지점이 없다.
        poisoned_ = true;
        return FrameResult::too_large;
    }
    if (available.size() < length_prefix_bytes + length) {
        return FrameResult::incomplete;
    }
    out = available.subspan(length_prefix_bytes, length);
    consumed_ += length_prefix_bytes + length;
    return FrameResult::frame;
}

void FrameAssembler::compact() {
    if (consumed_ == 0) {
        return;
    }
    // 소비한 바이트가 버퍼의 절반을 넘으면 버린다. 오래 유지되는 연결이 내부 저장소를
    // 계속 키우지 않게 하기 위해서다.
    if (consumed_ == buffer_.size()) {
        buffer_.clear();
        consumed_ = 0;
    } else if (consumed_ >= buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
}

bool write_frame(std::vector<std::byte>& out, std::span<const std::byte> payload,
                 std::uint32_t max_payload) {
    if (payload.size() > max_payload) {
        return false;
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (std::size_t i = 0; i < length_prefix_bytes; ++i) {
        out.push_back(static_cast<std::byte>((length >> (8 * i)) & 0xFF));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

} // mo::transport 네임스페이스 끝
