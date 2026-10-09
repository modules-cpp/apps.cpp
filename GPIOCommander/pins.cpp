#include "pins.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import mm.mcu;

namespace gpiocommander {
namespace {

// A board pin the app must not configure: what it is wired to, and whether
// sampling its analog input is harmless.
struct Reservation {
    unsigned int gpio;
    const char* role;
    bool analog_allowed = false;
};

// A free pin the board labels, such as an exposed UART.
struct Label {
    unsigned int gpio;
    const char* role;
};

struct BoardTable {
    std::string_view name;
    std::span<const Reservation> reserved;
    std::span<const Label> labels;
};

// Waveshare RP2350-Touch-LCD-2.8, from boards/rp2350_touch_lcd_28/board/mm.mdy.
constexpr Reservation touch_lcd_28_reserved[] = {
    {2, "I2S BCK"},  {3, "I2S LRCK"}, {4, "I2S DATA"}, {6, "I2C SDA"},
    {7, "I2C SCL"},  {10, "LCD SCK"}, {11, "LCD MOSI"}, {13, "LCD CS"},
    {14, "LCD DC"},  {15, "LCD RST"}, {16, "LCD BL"},  {17, "TP RST"},
    {18, "TP INT"},  {19, "SD CLK"},  {20, "SD CMD"},  {21, "SD D0"},
    {22, "SD D1"},   {23, "SD D2"},   {24, "SD D3"},   {25, "KEY"},
    {26, "BAT EN"},  {27, "BAT ADC", true},
};
constexpr Label touch_lcd_28_labels[] = {{0, "UART TX"}, {1, "UART RX"}};

// Waveshare RP2350-LCD-1.54 and its touch model, from
// boards/rp2350_touch_lcd_154/board-lcd/mm.mdy.
constexpr Reservation lcd_154_reserved[] = {
    {0, "AMP EN"},   {1, "I2S DOUT"}, {2, "I2S DIN"},  {3, "MCLK"},
    {4, "I2S BCK"},  {5, "I2S LRCK"}, {6, "I2C SDA"},  {7, "I2C SCL"},
    {9, "LCD CS"},   {10, "LCD SCK"}, {11, "LCD MOSI"}, {12, "LCD DC"},
    {13, "LCD BL"},  {14, "LCD RST"}, {15, "TP INT"},  {16, "TP RST"},
    {17, "CHARGE"},  {18, "SD"},      {19, "SD"},      {20, "SD"},
    {21, "SD"},      {22, "SD"},      {23, "SD"},      {24, "PWR KEY"},
    {25, "VOL KEY"}, {28, "BAT EN"},  {29, "BAT ADC", true},
};
constexpr Label lcd_154_labels[] = {{26, "UART TX"}, {27, "UART RX"}};

constexpr BoardTable boards[] = {
    {"rp2350_touch_lcd_28", touch_lcd_28_reserved, touch_lcd_28_labels},
    {"rp2350_touch_lcd_154", lcd_154_reserved, lcd_154_labels},
    {"rp2350_lcd_154", lcd_154_reserved, lcd_154_labels},
};

constexpr std::uint64_t pwm_period_ns = 1'000'000;    // 1 kHz

} // namespace

void Pins::discover() {
    const auto description = mm::mcu::board();
    board_name_ = description.name;
    const BoardTable* table = nullptr;
    for (const auto& candidate : boards)
        if (candidate.name == board_name_) table = &candidate;
    known_board_ = table != nullptr;

    count_ = 0;
    for (const auto& gpio : description.gpios) {
        if (count_ == pins_.size()) break;
        Pin pin{};
        pin.gpio = gpio.number;
        pin.name = gpio.name;
        pin.reserved = table == nullptr;
        pin.role = table == nullptr ? "UNKNOWN BOARD" : "";
        if (table != nullptr) {
            for (const auto& reservation : table->reserved) {
                if (reservation.gpio != gpio.number) continue;
                pin.reserved = true;
                pin.role = reservation.role;
                pin.analog_allowed = reservation.analog_allowed;
            }
            for (const auto& label : table->labels)
                if (label.gpio == gpio.number) pin.role = label.role;
        }
        unsigned int channel = 0;
        if (mm::mcu::adc_channel_for_gpio(gpio.number, channel) == mm::mcu::Status::Ok)
            pin.adc_channel = channel;
        unsigned int output = 0;
        if (mm::mcu::pwm_output_for_gpio(gpio.number, output) == mm::mcu::Status::Ok)
            pin.pwm_output = output;
        pins_[count_] = pin;
        readings_[count_] = {};
        ++count_;
    }
}

std::size_t Pins::find(unsigned int gpio) const {
    for (std::size_t i = 0; i < count_; ++i)
        if (pins_[i].gpio == gpio) return i;
    return none;
}

bool Pins::fail(const char* reason) {
    error_ = reason;
    return false;
}

void Pins::release(Pin& pin) {
    if (pin.mode == Mode::Analog && pin.adc_channel != none)
        (void)mm::mcu::adc_release(pin.adc_channel);
    if (pin.mode == Mode::Pwm && pin.pwm_output != none)
        (void)mm::mcu::pwm_release(pin.pwm_output);
}

bool Pins::make_input(std::size_t index, Pull pull) {
    if (index >= count_) return fail("NO SUCH PIN");
    auto& pin = pins_[index];
    if (pin.reserved) return fail("RESERVED PIN");
    release(pin);
    const auto mcu_pull = pull == Pull::Up     ? mm::mcu::Pull::Up
                          : pull == Pull::Down ? mm::mcu::Pull::Down
                                               : mm::mcu::Pull::None;
    if (mm::mcu::gpio_configure(pin.gpio, mm::mcu::Direction::In, mcu_pull) !=
        mm::mcu::Status::Ok) {
        pin.mode = Mode::Untouched;
        return fail("GPIO REFUSED");
    }
    pin.mode = Mode::Input;
    pin.pull = pull;
    return true;
}

bool Pins::make_output(std::size_t index, bool level) {
    if (index >= count_) return fail("NO SUCH PIN");
    auto& pin = pins_[index];
    if (pin.reserved) return fail("RESERVED PIN");
    if (pin.mode != Mode::Output) {
        release(pin);
        if (mm::mcu::gpio_configure(pin.gpio, mm::mcu::Direction::Out, mm::mcu::Pull::None) !=
            mm::mcu::Status::Ok) {
            pin.mode = Mode::Untouched;
            return fail("GPIO REFUSED");
        }
        pin.mode = Mode::Output;
        pin.pull = Pull::None;
    }
    if (mm::mcu::gpio_write(pin.gpio, level) != mm::mcu::Status::Ok)
        return fail("WRITE FAILED");
    pin.level = level;
    return true;
}

bool Pins::make_analog(std::size_t index) {
    if (index >= count_) return fail("NO SUCH PIN");
    auto& pin = pins_[index];
    if (pin.adc_channel == none) return fail("NO ADC CHANNEL");
    if (pin.reserved && !pin.analog_allowed) return fail("RESERVED PIN");
    if (pin.mode == Mode::Analog) return true;
    release(pin);
    const auto status = mm::mcu::adc_configure(pin.adc_channel);
    if (status != mm::mcu::Status::Ok)
        return fail(status == mm::mcu::Status::Busy ? "ADC BUSY" : "ADC REFUSED");
    pin.mode = Mode::Analog;
    return true;
}

bool Pins::make_pwm(std::size_t index, unsigned int duty_percent) {
    if (index >= count_) return fail("NO SUCH PIN");
    auto& pin = pins_[index];
    if (pin.reserved) return fail("RESERVED PIN");
    if (pin.pwm_output == none) return fail("NO PWM OUTPUT");
    if (duty_percent > 100) duty_percent = 100;
    if (pin.mode != Mode::Pwm) {
        release(pin);
        const auto status = mm::mcu::pwm_configure(pin.pwm_output, pwm_period_ns);
        if (status != mm::mcu::Status::Ok) {
            pin.mode = Mode::Untouched;
            return fail(status == mm::mcu::Status::Busy ? "PWM SLICE BUSY" : "PWM REFUSED");
        }
        pin.mode = Mode::Pwm;
    }
    std::uint64_t period = 0;
    if (mm::mcu::pwm_period(pin.pwm_output, period) != mm::mcu::Status::Ok ||
        mm::mcu::pwm_write(pin.pwm_output, period * duty_percent / 100u) != mm::mcu::Status::Ok)
        return fail("PWM WRITE FAILED");
    pin.duty_percent = duty_percent;
    return true;
}

void Pins::prepare_for_reading(std::size_t index) {
    if (index >= count_) return;
    auto& pin = pins_[index];
    if (pin.reserved || pin.mode != Mode::Untouched) return;
    (void)make_input(index, Pull::None);
}

void Pins::sample() {
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& pin = pins_[i];
        auto& reading = readings_[i];
        reading = {};
        if (pin.mode == Mode::Analog) {
            unsigned int count = 0;
            const auto status = mm::mcu::adc_read(pin.adc_channel, count);
            reading.analog = true;
            reading.busy = status == mm::mcu::Status::Busy;
            reading.valid = status == mm::mcu::Status::Ok;
            if (reading.valid) reading.count = count;
            continue;
        }
        bool high = false;
        const auto status = mm::mcu::gpio_read(pin.gpio, high);
        reading.busy = status == mm::mcu::Status::Busy;
        reading.valid = status == mm::mcu::Status::Ok;
        reading.high = reading.valid && high;
    }
}

void Pins::release_analog_claims() {
    for (std::size_t i = 0; i < count_; ++i)
        if (pins_[i].mode == Mode::Analog && pins_[i].adc_channel != none)
            (void)mm::mcu::adc_release(pins_[i].adc_channel);
}

void Pins::restore_analog_claims() {
    for (std::size_t i = 0; i < count_; ++i)
        if (pins_[i].mode == Mode::Analog && pins_[i].adc_channel != none &&
            mm::mcu::adc_configure(pins_[i].adc_channel) != mm::mcu::Status::Ok)
            pins_[i].mode = Mode::Untouched;
}

void Pins::release_all() {
    for (std::size_t i = 0; i < count_; ++i) release(pins_[i]);
}

bool adc_millivolts(unsigned int channel, unsigned int count, unsigned int& mv) {
    for (const auto& entry : mm::mcu::adc_description().channels) {
        if (entry.number != channel) continue;
        if (entry.reference_millivolts == 0 || entry.bits == 0 || entry.bits > 24) return false;
        const auto full = (1u << entry.bits) - 1u;
        mv = static_cast<unsigned int>(static_cast<std::uint64_t>(count) *
                                       entry.reference_millivolts / full);
        return true;
    }
    return false;
}

unsigned int adc_full_scale(unsigned int channel) {
    for (const auto& entry : mm::mcu::adc_description().channels)
        if (entry.number == channel && entry.bits != 0 && entry.bits <= 24)
            return (1u << entry.bits) - 1u;
    return 4095;
}

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Untouched: return "--";
        case Mode::Input: return "IN";
        case Mode::Output: return "OUT";
        case Mode::Analog: return "ADC";
        case Mode::Pwm: return "PWM";
    }
    return "?";
}

const char* pull_name(Pull pull) {
    switch (pull) {
        case Pull::None: return "NONE";
        case Pull::Up: return "UP";
        case Pull::Down: return "DOWN";
    }
    return "?";
}

} // namespace gpiocommander
