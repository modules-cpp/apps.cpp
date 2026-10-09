// GPIOCommander: the oscilloscope's and the logic analyzer's captures.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gpiocommander {

inline constexpr std::size_t scope_depth = 1024;
inline constexpr std::size_t logic_depth = 2048;
inline constexpr std::size_t logic_lanes = 4;

struct ScopeCapture {
    std::array<std::uint16_t, scope_depth> samples{};
    std::size_t count = 0;
    unsigned long rate_hz = 0;       // actually achieved
    bool paced = false;              // converter-paced, not polled
    std::uint32_t missed = 0;        // samples the platform dropped
    const char* error = "";
};

// The fastest rate the scope offers for channel: the converter's paced
// maximum, or a polled ceiling where the platform cannot pace.
[[nodiscard]] unsigned long scope_maximum_rate(unsigned int channel);

// Captures want samples (at most scope_depth) from an ADC channel at
// rate_hz. The caller has released every other ADC claim: pacing owns the
// converter. The channel is released again before returning.
[[nodiscard]] bool scope_capture(unsigned int channel, unsigned long rate_hz, std::size_t want,
                                 ScopeCapture& out);

enum class Trigger { None, Rising, Falling };

struct LogicCapture {
    // One sample per entry, lane n in bit n.
    std::array<std::uint8_t, logic_depth> samples{};
    std::size_t count = 0;
    unsigned long rate_hz = 0;       // actually achieved
    std::size_t late = 0;            // samples taken after their slot ended
    bool triggered = false;
    bool timed_out = false;          // the trigger never came
    const char* error = "";
};

// Samples up to logic_lanes GPIOs every period_us microseconds (zero: as
// fast as the loop runs), after an edge on lane 0 when trigger asks for one,
// waiting at most trigger_timeout_ms. Reading never reconfigures a pin.
[[nodiscard]] bool logic_capture(std::span<const unsigned int> gpios, unsigned long period_us,
                                 Trigger trigger, unsigned long trigger_timeout_ms,
                                 LogicCapture& out);

} // namespace gpiocommander
