// GPIOCommander: the board's pins, which of them the app may change, and the
// state the app gave the ones it changed.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gpiocommander {

// The most GPIOs any supported part has: the RP2350B's bank 0.
inline constexpr std::size_t maximum_pins = 48;
inline constexpr unsigned int none = ~0u;

// What the app has done to a pin. Untouched means the app has not configured
// it, so its direction and pull are whatever firmware or reset left.
enum class Mode { Untouched, Input, Output, Analog, Pwm };
enum class Pull { None, Up, Down };

struct Pin {
    unsigned int gpio = 0;
    std::string_view name;
    // What the board wires the pin to, or empty for a free pin.
    const char* role = "";
    // A reserved pin drives the board itself -- display, touch, codec, SD,
    // power. The app reads it and never configures it: the Pico's
    // gpio_configure re-initialises a pad whatever peripheral owns it.
    bool reserved = true;
    // A reserved pin whose analog input is harmless to sample, such as a
    // battery-sense divider.
    bool analog_allowed = false;
    unsigned int adc_channel = none;
    unsigned int pwm_output = none;

    Mode mode = Mode::Untouched;
    Pull pull = Pull::None;
    bool level = false;              // the level written while an output
    unsigned int duty_percent = 50;  // while a PWM output
};

// Last values read, refreshed by Pins::sample.
struct Reading {
    bool valid = false;
    bool high = false;
    bool analog = false;
    unsigned int count = 0;          // ADC count when analog
    bool busy = false;               // the facility answered Busy
};

class Pins {
public:
    // Builds the inventory from mm::mcu::board() and the board's reserved
    // table. A board without a table is monitor-only: every pin reserved.
    void discover();

    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] Pin& operator[](std::size_t index) { return pins_[index]; }
    [[nodiscard]] const Pin& operator[](std::size_t index) const { return pins_[index]; }
    [[nodiscard]] const Reading& reading(std::size_t index) const { return readings_[index]; }
    [[nodiscard]] bool known_board() const { return known_board_; }
    [[nodiscard]] std::string_view board_name() const { return board_name_; }
    // The index of gpio, or none.
    [[nodiscard]] std::size_t find(unsigned int gpio) const;

    // Pin changes. Each refuses a reserved pin and releases the ADC or PWM
    // claim the pin held before taking the new mode. False on refusal or a
    // facility error, with the reason in last_error().
    [[nodiscard]] bool make_input(std::size_t index, Pull pull);
    [[nodiscard]] bool make_output(std::size_t index, bool level);
    [[nodiscard]] bool make_analog(std::size_t index);
    [[nodiscard]] bool make_pwm(std::size_t index, unsigned int duty_percent);
    [[nodiscard]] const char* last_error() const { return error_; }

    // Makes a free, untouched pin an input so reading it means something:
    // an RP2350 pad stays isolated until a function is selected. Reserved
    // and already-configured pins are left alone.
    void prepare_for_reading(std::size_t index);

    // Reads every pin: digital level, or ADC count for an analog pin.
    void sample();

    // The scope paces the whole converter, so every ADC claim the app holds
    // is released first and restored after.
    void release_analog_claims();
    void restore_analog_claims();

    // Releases every claim at exit.
    void release_all();

private:
    void release(Pin& pin);
    [[nodiscard]] bool fail(const char* reason);

    std::array<Pin, maximum_pins> pins_{};
    std::array<Reading, maximum_pins> readings_{};
    std::size_t count_ = 0;
    bool known_board_ = false;
    std::string_view board_name_;
    const char* error_ = "";
};

// Count to millivolts for an ADC channel when the board knows its reference;
// false when it does not.
[[nodiscard]] bool adc_millivolts(unsigned int channel, unsigned int count, unsigned int& mv);
// The full-scale count of an ADC channel, or 4095 when unknown.
[[nodiscard]] unsigned int adc_full_scale(unsigned int channel);

[[nodiscard]] const char* mode_name(Mode mode);
[[nodiscard]] const char* pull_name(Pull pull);

} // namespace gpiocommander
