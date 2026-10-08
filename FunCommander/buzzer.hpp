#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import mm.audio;

namespace funcommander {

class Buzzer {
public:
    static constexpr unsigned int sample_rate = 32'000;
    static constexpr unsigned int tone_hz = 500;
    static constexpr std::int16_t amplitude = 6000;
    static constexpr std::size_t buffer_frames = 512;

    bool initialize() {
        auto& speaker = mm::audio::selected_out();
        if (speaker.initialize() != mm::audio::Status::Ok) {
            return false;
        }
        mm::audio::Format actual{};
        if (speaker.configure({.rate_hz = sample_rate}, actual) != mm::audio::Status::Ok) {
            return false;
        }
        if (ring_.configure(storage_, actual) != mm::audio::Status::Ok) {
            return false;
        }
        if (speaker.start(ring_) != mm::audio::Status::Ok) {
            return false;
        }
        rate_hz_ = actual.rate_hz;
        ready_ = true;
        return true;
    }

    void shutdown() {
        if (!ready_) return;
        auto& speaker = mm::audio::selected_out();
        (void)speaker.stop();
        (void)speaker.sleep();
        ready_ = false;
    }

    void update(bool beeping) {
        if (!ready_) return;
        auto& speaker = mm::audio::selected_out();

        if (beeping) {
            const auto half_period = rate_hz_ / (2u * tone_hz);
            auto region = ring_.write_region();
            for (auto& s : region) {
                s = ((phase_ / half_period) % 2u == 0) ? amplitude : -amplitude;
                ++phase_;
            }
            if (!region.empty()) {
                (void)ring_.commit_write(region.size());
            }
        } else {
            phase_ = 0;
        }

        (void)speaker.service();
    }

    [[nodiscard]] bool is_ready() const { return ready_; }

private:
    std::array<std::int16_t, buffer_frames> storage_{};
    mm::audio::Ring ring_{};
    unsigned int rate_hz_ = sample_rate;
    std::uint64_t phase_ = 0;
    bool ready_ = false;
};

}  // namespace funcommander
