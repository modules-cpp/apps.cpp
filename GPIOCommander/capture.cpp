#include "capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

import mm.mcu;

namespace gpiocommander {
namespace {

// Where a platform cannot pace its converter, the scope times single reads
// itself; above this the loop's own jitter dominates.
constexpr unsigned long polled_ceiling_hz = 20'000;

const mm::mcu::AdcChannel* channel_entry(unsigned int channel) {
    for (const auto& entry : mm::mcu::adc_description().channels)
        if (entry.number == channel) return &entry;
    return nullptr;
}

[[nodiscard]] bool now_us(unsigned long& ticks) {
    return mm::mcu::ticks_us(ticks) == mm::mcu::Status::Ok;
}

[[nodiscard]] bool fail(const char*& error, const char* reason) {
    error = reason;
    return false;
}

bool paced_capture(unsigned int channel, unsigned long rate_hz, std::size_t want,
                   ScopeCapture& out) {
    if (mm::mcu::adc_pace(channel, rate_hz) != mm::mcu::Status::Ok)
        return fail(out.error, "PACE REFUSED");
    mm::mcu::Frequency actual{};
    if (mm::mcu::adc_pace_rate(channel, actual) == mm::mcu::Status::Ok && actual.denominator != 0)
        out.rate_hz = static_cast<unsigned long>(actual.numerator / actual.denominator);
    else
        out.rate_hz = rate_hz;
    if (mm::mcu::adc_pace_start(channel) != mm::mcu::Status::Ok) {
        (void)mm::mcu::adc_release(channel);
        return fail(out.error, "PACE START FAILED");
    }
    // Twice the capture's own duration, and a floor for slow rates.
    const unsigned long budget_ms =
        static_cast<unsigned long>(static_cast<std::uint64_t>(want) * 2000u / out.rate_hz) + 100u;
    unsigned long start_ms = 0;
    (void)mm::mcu::ticks_ms(start_ms);
    bool ok = true;
    while (out.count < want) {
        std::size_t taken = 0;
        const auto room = std::span<std::uint16_t>{out.samples}.subspan(out.count, want - out.count);
        if (mm::mcu::adc_take(channel, room, taken) != mm::mcu::Status::Ok) {
            ok = fail(out.error, "TAKE FAILED");
            break;
        }
        out.count += taken;
        unsigned long now_ms = 0;
        if (mm::mcu::ticks_ms(now_ms) != mm::mcu::Status::Ok || now_ms - start_ms > budget_ms) {
            if (out.count == 0) ok = fail(out.error, "NO SAMPLES");
            break;
        }
    }
    mm::mcu::Progress progress{};
    if (mm::mcu::adc_pace_progress(channel, progress) == mm::mcu::Status::Ok)
        out.missed = progress.missed;
    (void)mm::mcu::adc_pace_stop(channel);
    (void)mm::mcu::adc_release(channel);
    return ok;
}

bool polled_capture(unsigned int channel, unsigned long rate_hz, std::size_t want,
                    ScopeCapture& out) {
    if (mm::mcu::adc_configure(channel) != mm::mcu::Status::Ok)
        return fail(out.error, "ADC REFUSED");
    const unsigned long period_us = 1'000'000ul / rate_hz;
    unsigned long start = 0;
    bool ok = now_us(start);
    unsigned long now = start;
    for (std::size_t i = 0; ok && i < want; ++i) {
        const auto due = static_cast<unsigned long>(i * period_us);
        while (ok && now - start < due) ok = now_us(now);
        unsigned int count = 0;
        if (!ok || mm::mcu::adc_read(channel, count) != mm::mcu::Status::Ok) {
            ok = fail(out.error, "READ FAILED");
            break;
        }
        out.samples[i] = static_cast<std::uint16_t>(count);
        out.count = i + 1;
    }
    if (ok && now_us(now) && now != start && out.count > 1)
        out.rate_hz = static_cast<unsigned long>(static_cast<std::uint64_t>(out.count - 1) *
                                                 1'000'000u / (now - start));
    else
        out.rate_hz = rate_hz;
    (void)mm::mcu::adc_release(channel);
    return ok;
}

} // namespace

unsigned long scope_maximum_rate(unsigned int channel) {
    const auto* entry = channel_entry(channel);
    if (entry == nullptr) return 0;
    return entry->maximum_pace_hz != 0 ? entry->maximum_pace_hz : polled_ceiling_hz;
}

bool scope_capture(unsigned int channel, unsigned long rate_hz, std::size_t want,
                   ScopeCapture& out) {
    out.count = 0;
    out.missed = 0;
    out.error = "";
    if (want > scope_depth) want = scope_depth;
    const auto* entry = channel_entry(channel);
    if (entry == nullptr || rate_hz == 0 || want == 0) return fail(out.error, "BAD CHANNEL");
    out.paced = entry->maximum_pace_hz != 0 && rate_hz <= entry->maximum_pace_hz;
    if (out.paced) return paced_capture(channel, rate_hz, want, out);
    if (rate_hz > polled_ceiling_hz) rate_hz = polled_ceiling_hz;
    return polled_capture(channel, rate_hz, want, out);
}

bool logic_capture(std::span<const unsigned int> gpios, unsigned long period_us,
                   Trigger trigger, unsigned long trigger_timeout_ms, LogicCapture& out) {
    out.count = 0;
    out.late = 0;
    out.triggered = false;
    out.timed_out = false;
    out.error = "";
    if (gpios.empty() || gpios.size() > logic_lanes) return fail(out.error, "NO LANES");

    // Wait for the edge on lane 0, polling as fast as the loop runs.
    if (trigger != Trigger::None) {
        bool previous = false;
        if (mm::mcu::gpio_read(gpios[0], previous) != mm::mcu::Status::Ok)
            return fail(out.error, "TRIGGER PIN BUSY");
        unsigned long start_ms = 0;
        if (mm::mcu::ticks_ms(start_ms) != mm::mcu::Status::Ok) return fail(out.error, "NO CLOCK");
        for (std::uint32_t spin = 0;; ++spin) {
            bool level = false;
            if (mm::mcu::gpio_read(gpios[0], level) != mm::mcu::Status::Ok)
                return fail(out.error, "TRIGGER PIN BUSY");
            if ((trigger == Trigger::Rising && !previous && level) ||
                (trigger == Trigger::Falling && previous && !level)) {
                out.triggered = true;
                break;
            }
            previous = level;
            // The clock is read every 1024 polls to keep the loop tight.
            if ((spin & 1023u) == 0) {
                unsigned long now_ms = 0;
                if (mm::mcu::ticks_ms(now_ms) != mm::mcu::Status::Ok ||
                    now_ms - start_ms > trigger_timeout_ms) {
                    out.timed_out = true;
                    return true;
                }
            }
        }
    }

    unsigned long start = 0;
    if (!now_us(start)) return fail(out.error, "NO CLOCK");
    unsigned long now = start;
    for (std::size_t i = 0; i < logic_depth; ++i) {
        if (period_us != 0) {
            const auto due = static_cast<unsigned long>(i * period_us);
            while (now - start < due)
                if (!now_us(now)) return fail(out.error, "NO CLOCK");
        }
        std::uint8_t bits = 0;
        for (std::size_t lane = 0; lane < gpios.size(); ++lane) {
            bool level = false;
            if (mm::mcu::gpio_read(gpios[lane], level) == mm::mcu::Status::Ok && level)
                bits = static_cast<std::uint8_t>(bits | (1u << lane));
        }
        out.samples[i] = bits;
        out.count = i + 1;
        if (period_us != 0) {
            if (!now_us(now)) return fail(out.error, "NO CLOCK");
            if (now - start > static_cast<unsigned long>((i + 1) * period_us)) ++out.late;
        }
    }
    if (!now_us(now)) return fail(out.error, "NO CLOCK");
    const auto elapsed = now - start;
    out.rate_hz = elapsed == 0 ? 0
                               : static_cast<unsigned long>(static_cast<std::uint64_t>(out.count) *
                                                            1'000'000u / elapsed);
    return true;
}

} // namespace gpiocommander
