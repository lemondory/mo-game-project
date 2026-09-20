#include "loopback.hpp"

#include <algorithm>

namespace mo::transport {

namespace {

// 링크의 한 방향이다. 받는 쪽 조립기로 이어지는 바이트 파이프다.
struct Pipe {
    std::vector<std::byte> in_flight;
    std::size_t drop_next{};
};

} // 익명 네임스페이스 끝

class LoopbackLink::Impl {
public:
    class Side final : public Endpoint {
    public:
        Side(Impl& link, bool is_first)
            : link_(link), is_first_(is_first), assembler_(link.config_.max_payload) {}

        SendStatus send(std::span<const std::byte> payload) override {
            if (closed_ || peer().closed_) {
                return SendStatus::closed;
            }
            auto& pipe = outbound();
            if (payload.size() > link_.config_.max_payload) {
                return SendStatus::too_large;
            }
            const auto framed = payload.size() + length_prefix_bytes;
            if (pipe.in_flight.size() + framed > link_.config_.send_buffer_bytes) {
                return SendStatus::would_block;
            }
            write_frame(pipe.in_flight, payload, link_.config_.max_payload);
            return SendStatus::sent;
        }

        std::size_t receive(std::vector<std::vector<std::byte>>& out,
                            std::size_t max_frames) override {
            std::size_t produced = 0;
            while (produced < max_frames) {
                std::span<const std::byte> frame;
                const auto result = assembler_.next_frame(frame);
                if (result == FrameResult::frame) {
                    out.emplace_back(frame.begin(), frame.end());
                    ++produced;
                    continue;
                }
                if (result == FrameResult::too_large) {
                    faulted_ = true;
                }
                break;
            }
            return produced;
        }

        void close() override { closed_ = true; }
        bool closed() const override { return closed_; }
        bool faulted() const override { return faulted_ || assembler_.poisoned(); }

        void deliver(std::span<const std::byte> bytes) { assembler_.push(bytes); }

    private:
        Pipe& outbound() { return is_first_ ? link_.first_to_second_ : link_.second_to_first_; }
        Side& peer() { return is_first_ ? *link_.second_ : *link_.first_; }

        Impl& link_;
        bool is_first_;
        FrameAssembler assembler_;
        bool closed_{};
        bool faulted_{};
    };

    explicit Impl(LoopbackConfig config) : config_(config) {
        first_ = std::make_unique<Side>(*this, true);
        second_ = std::make_unique<Side>(*this, false);
    }

    Side& first() { return *first_; }
    Side& second() { return *second_; }

    std::size_t pump() {
        return move(first_to_second_, *second_) + move(second_to_first_, *first_);
    }

    void drop_from_first(std::size_t drop_bytes) { first_to_second_.drop_next += drop_bytes; }

private:
    std::size_t move(Pipe& pipe, Side& receiver) {
        if (pipe.in_flight.empty()) {
            return 0;
        }
        auto take = pipe.in_flight.size();
        if (config_.chunk_bytes != 0) {
            take = std::min(take, config_.chunk_bytes);
        }
        const auto dropped = std::min(pipe.drop_next, take);
        pipe.drop_next -= dropped;
        const std::span<const std::byte> payload(pipe.in_flight.data() + dropped, take - dropped);
        if (!payload.empty()) {
            receiver.deliver(payload);
        }
        pipe.in_flight.erase(pipe.in_flight.begin(),
                             pipe.in_flight.begin() + static_cast<std::ptrdiff_t>(take));
        return take;
    }

    LoopbackConfig config_;
    Pipe first_to_second_;
    Pipe second_to_first_;
    std::unique_ptr<Side> first_;
    std::unique_ptr<Side> second_;
};

LoopbackLink::LoopbackLink(LoopbackConfig config) : impl_(std::make_unique<Impl>(config)) {}
LoopbackLink::~LoopbackLink() = default;
Endpoint& LoopbackLink::first() { return impl_->first(); }
Endpoint& LoopbackLink::second() { return impl_->second(); }
std::size_t LoopbackLink::pump() { return impl_->pump(); }
void LoopbackLink::drop_from_first(std::size_t drop_bytes) { impl_->drop_from_first(drop_bytes); }

} // mo::transport 네임스페이스 끝
