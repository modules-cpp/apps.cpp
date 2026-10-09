#include "capture.hpp"
#include "pins.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

import mm.display;
import mm.fonts;
import mm.gfx;
import mm.mcu;
import mm.touch;

namespace {

using gpiocommander::LogicCapture;
using gpiocommander::Mode;
using gpiocommander::Pins;
using gpiocommander::Pull;
using gpiocommander::ScopeCapture;
using gpiocommander::Trigger;
using gpiocommander::none;
using mm::display::Status;
using mm::gfx::Rgb565;
using mm::gfx::Surface;

constexpr unsigned int maximum_width = 320;
constexpr unsigned int maximum_height = 320;
constexpr unsigned int tab_height = 28;
constexpr unsigned int content_top = 32;
constexpr unsigned int bar_height = 34;
constexpr unsigned long pin_refresh_ms = 250;

std::array<std::byte, maximum_width * maximum_height * 2u> frame_bytes{};
std::array<std::byte, 1024> text_bits{};
ScopeCapture scope_capture{};
LogicCapture logic_capture{};

constexpr Rgb565 background = mm::gfx::rgb(10, 16, 25);
constexpr Rgb565 panel = mm::gfx::rgb(24, 37, 54);
constexpr Rgb565 panel_active = mm::gfx::rgb(52, 78, 108);
constexpr Rgb565 grid = mm::gfx::rgb(30, 46, 64);
constexpr Rgb565 accent = mm::gfx::rgb(234, 174, 77);
constexpr Rgb565 quiet = mm::gfx::rgb(110, 128, 142);
constexpr Rgb565 white = mm::gfx::rgb(232, 236, 240);
constexpr Rgb565 high_color = mm::gfx::rgb(96, 230, 128);
constexpr Rgb565 low_color = mm::gfx::rgb(70, 110, 150);
constexpr Rgb565 analog_color = mm::gfx::rgb(250, 200, 80);
constexpr Rgb565 pwm_color = mm::gfx::rgb(220, 120, 230);
constexpr Rgb565 error_color = mm::gfx::rgb(255, 96, 86);
constexpr std::array<Rgb565, gpiocommander::logic_lanes> lane_colors{{
    mm::gfx::rgb(96, 230, 128), mm::gfx::rgb(250, 200, 80),
    mm::gfx::rgb(110, 190, 255), mm::gfx::rgb(220, 120, 230),
}};

enum class Screen { Pins, Pin, Scope, Logic };
constexpr std::array<const char*, 4> tab_names{{"PINS", "PIN", "SCOPE", "LOGIC"}};

constexpr std::array<unsigned long, 10> scope_rates{{
    500, 1'000, 2'000, 5'000, 10'000, 20'000, 50'000, 100'000, 200'000, 500'000}};
// Sample periods in microseconds; zero samples as fast as the loop runs.
constexpr std::array<unsigned long, 10> logic_periods{{0, 2, 5, 10, 20, 50, 100, 200, 500, 1'000}};
constexpr std::array<const char*, 10> logic_period_names{{
    "MAX", "500k", "200k", "100k", "50k", "20k", "10k", "5k", "2k", "1k"}};
constexpr std::array<unsigned int, 5> logic_zooms{{1, 2, 4, 8, 16}};
constexpr unsigned long logic_trigger_timeout_ms = 2'000;

struct Rect {
    unsigned int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool contains(unsigned int px, unsigned int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// ---------------------------------------------------------------- drawing

[[nodiscard]] bool text(Surface frame, const char* value, unsigned int x, unsigned int y,
                        Rgb565 color, const mm::fonts::Font& font = mm::fonts::kMono12) {
    if (x >= frame.width || y >= frame.height) return true;
    std::array<char8_t, 48> characters{};
    const unsigned int fit = (frame.width - x) / font.advance;
    unsigned int count = 0;
    while (value[count] != 0 && count < characters.size() && count < fit) {
        characters[count] = static_cast<char8_t>(value[count]);
        ++count;
    }
    if (count == 0) return true;
    const auto width = count * font.advance;
    const auto row_bytes = (width + 7u) / 8u;
    const auto size = row_bytes * font.height;
    if (size > text_bits.size()) return false;
    const Surface mask{width, font.height, 1, std::span<std::byte>{text_bits}.first(size)};
    if (mm::gfx::fill(mask, mm::display::Color::Black) != Status::Ok ||
        mm::fonts::render(characters.data(), count, font, mm::display::Color::White, 0, 0,
                          mask) != Status::Ok)
        return false;
    for (unsigned int row = 0; row < mask.height; ++row) {
        for (unsigned int column = 0; column < mask.width; ++column) {
            const auto bits = mask.pixels[row * row_bytes + column / 8u];
            if ((bits & static_cast<std::byte>(0x80u >> (column % 8u))) != std::byte{0})
                (void)mm::gfx::pixel(frame, static_cast<int>(x + column),
                                     static_cast<int>(y + row), color);
        }
    }
    return true;
}

[[nodiscard]] bool button(Surface frame, const Rect& r, const char* label, bool enabled,
                          bool active = false) {
    (void)mm::gfx::fill_rectangle(frame, static_cast<int>(r.x), static_cast<int>(r.y), r.w, r.h,
                                  active ? panel_active : panel);
    unsigned int length = 0;
    while (label[length] != 0) ++length;
    const auto& font = mm::fonts::kMono12;
    const auto width = length * font.advance;
    const auto x = r.x + (r.w > width ? (r.w - width) / 2u : 0u);
    const auto y = r.y + (r.h > font.height ? (r.h - font.height) / 2u : 0u);
    return text(frame, label, x, y, enabled ? white : quiet);
}

void volts(char* out, std::size_t size, unsigned int mv) {
    std::snprintf(out, size, "%u.%02u", mv / 1000u, (mv % 1000u) / 10u);
}

void hertz(char* out, std::size_t size, unsigned long hz) {
    if (hz >= 1'000'000ul)
        std::snprintf(out, size, "%lu.%02luM", hz / 1'000'000ul, (hz % 1'000'000ul) / 10'000ul);
    else if (hz >= 1'000ul)
        std::snprintf(out, size, "%lu.%02luk", hz / 1'000ul, (hz % 1'000ul) / 10ul);
    else
        std::snprintf(out, size, "%lu", hz);
}

// ---------------------------------------------------------------- the app

class App {
public:
    App(mm::display::Display& display, unsigned int width, unsigned int height)
        : display_(display), width_(width), height_(height) {}

    void start() {
        pins_.discover();
        for (std::size_t i = 0; i < pins_.size(); ++i) pins_.prepare_for_reading(i);
        collect_scope_channels();
        choose_default_lanes();
        pins_.sample();
        if (!pins_.known_board()) message_ = "UNKNOWN BOARD: MONITOR ONLY";
    }

    void stop() { pins_.release_all(); }

    [[nodiscard]] bool draw() {
        const Surface frame{width_, height_, 16,
                            std::span<std::byte>{frame_bytes}.first(
                                static_cast<std::size_t>(width_) * height_ * 2u)};
        if (mm::gfx::fill(frame, background) != Status::Ok) return false;
        if (!draw_tabs(frame)) return false;
        bool ok = true;
        switch (screen_) {
            case Screen::Pins: ok = draw_pins(frame); break;
            case Screen::Pin: ok = draw_pin(frame); break;
            case Screen::Scope: ok = draw_scope(frame); break;
            case Screen::Logic: ok = draw_logic(frame); break;
        }
        return ok && mm::gfx::write(display_, frame, 0, 0) == Status::Ok &&
               display_.refresh(mm::display::Refresh::Full) == Status::Ok;
    }

    // A press at (x, y); true when the screen must be redrawn.
    [[nodiscard]] bool press(unsigned int x, unsigned int y) {
        if (y < tab_height) {
            const auto tab = x * tab_names.size() / width_;
            const auto next = static_cast<Screen>(tab < tab_names.size() ? tab : 0);
            if (next == screen_) return false;
            screen_ = next;
            message_ = "";
            if (screen_ == Screen::Scope && scope_run_) run_scope();
            return true;
        }
        switch (screen_) {
            case Screen::Pins: return press_pins(x, y);
            case Screen::Pin: return press_pin(x, y);
            case Screen::Scope: return press_scope(x, y);
            case Screen::Logic: return press_logic(x, y);
        }
        return false;
    }

    // Periodic work; true when the screen must be redrawn.
    [[nodiscard]] bool tick(unsigned long now_ms) {
        if (screen_ == Screen::Scope) {
            if (!scope_run_) return false;
            run_scope();
            return true;
        }
        if (screen_ == Screen::Pins || screen_ == Screen::Pin) {
            if (now_ms - last_sample_ms_ < pin_refresh_ms) return false;
            last_sample_ms_ = now_ms;
            pins_.sample();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool busy() const { return screen_ == Screen::Scope && scope_run_; }

private:
    // ------------------------------------------------------------ layout

    [[nodiscard]] Rect bar_button(unsigned int index, unsigned int count) const {
        const auto w = (width_ - 4u) / count;
        return {2u + index * w + 1u, height_ - bar_height - 2u, w - 2u, bar_height};
    }

    [[nodiscard]] unsigned int pin_rows() const {
        return (height_ - content_top - bar_height - 26u) / 20u;
    }
    [[nodiscard]] std::size_t pins_per_page() const { return 3u * pin_rows(); }
    [[nodiscard]] std::size_t page_count() const {
        const auto per = pins_per_page();
        return per == 0 ? 1 : (pins_.size() + per - 1) / per;
    }

    [[nodiscard]] Rect pin_cell(std::size_t slot) const {
        const auto column = static_cast<unsigned int>(slot / pin_rows());
        const auto row = static_cast<unsigned int>(slot % pin_rows());
        const auto w = (width_ - 4u) / 3u;
        return {2u + column * w, content_top + row * 20u, w - 2u, 19u};
    }

    // The PIN screen's button grid: four rows of three, below the four
    // text lines that end at y = 115.
    [[nodiscard]] Rect pin_button(unsigned int row, unsigned int column) const {
        const unsigned int top = 120u;
        const unsigned int pitch = (height_ - top - 2u) / 4u;
        const auto w = (width_ - 8u) / 3u;
        return {4u + column * w, top + row * pitch, w - 4u, pitch - 4u};
    }

    [[nodiscard]] Rect trace_area() const {
        return {4u, content_top + 20u, width_ - 8u, height_ - content_top - 20u - bar_height - 44u};
    }

    // ------------------------------------------------------------ tabs

    [[nodiscard]] bool draw_tabs(Surface frame) {
        const auto w = width_ / static_cast<unsigned int>(tab_names.size());
        for (unsigned int i = 0; i < tab_names.size(); ++i) {
            const Rect r{i * w + 1u, 0u, w - 2u, tab_height - 2u};
            if (!button(frame, r, tab_names[i], true, static_cast<unsigned int>(screen_) == i))
                return false;
        }
        return true;
    }

    // ------------------------------------------------------------ PINS

    void cell_label(std::size_t index, char* out, std::size_t size, Rgb565& color) const {
        const auto& pin = pins_[index];
        const auto& reading = pins_.reading(index);
        char value[12] = "?";
        color = quiet;
        if (reading.busy) {
            std::snprintf(value, sizeof value, "busy");
        } else if (reading.valid && reading.analog) {
            unsigned int mv = 0;
            if (gpiocommander::adc_millivolts(pin.adc_channel, reading.count, mv))
                volts(value, sizeof value, mv);
            else
                std::snprintf(value, sizeof value, "%u", reading.count);
            color = analog_color;
        } else if (pin.mode == Mode::Pwm) {
            std::snprintf(value, sizeof value, "%u%%", pin.duty_percent);
            color = pwm_color;
        } else if (reading.valid) {
            std::snprintf(value, sizeof value, "%c", reading.high ? '1' : '0');
            color = reading.high ? high_color : low_color;
        }
        char mode = '-';
        switch (pin.mode) {
            case Mode::Untouched: mode = pin.reserved ? 'r' : '-'; break;
            case Mode::Input: mode = 'i'; break;
            case Mode::Output: mode = 'o'; break;
            case Mode::Analog: mode = 'a'; break;
            case Mode::Pwm: mode = 'p'; break;
        }
        const std::string_view name = pin.name;
        std::snprintf(out, size, "%-4.*s %c%s", static_cast<int>(name.size()), name.data(), mode,
                      value);
    }

    [[nodiscard]] bool draw_pins(Surface frame) {
        const auto per = pins_per_page();
        const auto first = page_ * per;
        for (std::size_t slot = 0; slot < per && first + slot < pins_.size(); ++slot) {
            const auto index = first + slot;
            const auto r = pin_cell(slot);
            if (index == selected_)
                (void)mm::gfx::fill_rectangle(frame, static_cast<int>(r.x), static_cast<int>(r.y),
                                              r.w, r.h, panel_active);
            char label[24]{};
            Rgb565 color = quiet;
            cell_label(index, label, sizeof label, color);
            // The pin name in white for a pin the app may change, grey for a
            // reserved one; the value in its state's color.
            if (!text(frame, label, r.x + 2u, r.y + 1u, pins_[index].reserved ? quiet : white))
                return false;
            if (!text(frame, label + 5, r.x + 2u + 5u * mm::fonts::kMono12.advance, r.y + 1u,
                      color))
                return false;
        }
        const auto info_y = height_ - bar_height - 22u;
        char info[48]{};
        if (message_[0] != 0)
            std::snprintf(info, sizeof info, "%s", message_);
        else
            std::snprintf(info, sizeof info, "%.*s  r=reserved",
                          static_cast<int>(pins_.board_name().size()), pins_.board_name().data());
        if (!text(frame, info, 4u, info_y, message_[0] != 0 ? error_color : quiet)) return false;
        char page[16]{};
        std::snprintf(page, sizeof page, "%zu/%zu", page_ + 1, page_count());
        return button(frame, bar_button(0, 3), "< PAGE", page_ > 0) &&
               button(frame, bar_button(1, 3), page, false) &&
               button(frame, bar_button(2, 3), "PAGE >", page_ + 1 < page_count());
    }

    [[nodiscard]] bool press_pins(unsigned int x, unsigned int y) {
        if (bar_button(0, 3).contains(x, y)) {
            if (page_ == 0) return false;
            --page_;
            return true;
        }
        if (bar_button(2, 3).contains(x, y)) {
            if (page_ + 1 >= page_count()) return false;
            ++page_;
            return true;
        }
        const auto per = pins_per_page();
        for (std::size_t slot = 0; slot < per; ++slot) {
            const auto index = page_ * per + slot;
            if (index >= pins_.size()) break;
            if (!pin_cell(slot).contains(x, y)) continue;
            selected_ = index;
            screen_ = Screen::Pin;
            message_ = "";
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------ PIN

    [[nodiscard]] bool adc_allowed(std::size_t index) const {
        const auto& pin = pins_[index];
        return pin.adc_channel != none && (!pin.reserved || pin.analog_allowed);
    }

    [[nodiscard]] bool draw_pin(Surface frame) {
        if (selected_ >= pins_.size()) return text(frame, "NO GPIO ON THIS BOARD", 4, 40, quiet);
        const auto& pin = pins_[selected_];
        const auto& reading = pins_.reading(selected_);
        char line[48]{};
        const std::string_view name = pin.name;
        std::snprintf(line, sizeof line, "%.*s", static_cast<int>(name.size()), name.data());
        if (!text(frame, line, 6, content_top + 2u, accent, mm::fonts::kMono16)) return false;
        if (!text(frame, pin.role[0] != 0 ? pin.role : "FREE", 90, content_top + 6u,
                  pin.reserved ? quiet : white))
            return false;

        std::snprintf(line, sizeof line, "MODE %s  PULL %s", gpiocommander::mode_name(pin.mode),
                      pin.mode == Mode::Input ? gpiocommander::pull_name(pin.pull) : "-");
        if (!text(frame, line, 6, content_top + 26u, white)) return false;

        Rgb565 color = white;
        if (reading.busy) {
            std::snprintf(line, sizeof line, "VALUE: BUSY");
            color = quiet;
        } else if (reading.valid && reading.analog) {
            unsigned int mv = 0;
            char v[12]{};
            if (gpiocommander::adc_millivolts(pin.adc_channel, reading.count, mv)) {
                volts(v, sizeof v, mv);
                std::snprintf(line, sizeof line, "ADC%u %u = %s V", pin.adc_channel, reading.count,
                              v);
            } else {
                std::snprintf(line, sizeof line, "ADC%u COUNT %u", pin.adc_channel, reading.count);
            }
            color = analog_color;
        } else if (pin.mode == Mode::Pwm) {
            std::snprintf(line, sizeof line, "PWM 1 kHz  DUTY %u%%", pin.duty_percent);
            color = pwm_color;
        } else if (reading.valid) {
            std::snprintf(line, sizeof line, "LEVEL %c", reading.high ? '1' : '0');
            color = reading.high ? high_color : low_color;
        } else {
            std::snprintf(line, sizeof line, "VALUE: ?");
        }
        if (!text(frame, line, 6, content_top + 46u, color)) return false;

        const char* note = message_;
        if (note[0] == 0 && pin.reserved)
            note = pin.analog_allowed ? "RESERVED: ANALOG READ ONLY" : "RESERVED: READ ONLY";
        if (!text(frame, note, 6, content_top + 66u, message_[0] != 0 ? error_color : quiet))
            return false;

        const bool free = !pin.reserved;
        const bool output = pin.mode == Mode::Output;
        const bool pwm = free && pin.pwm_output != none;
        const bool in_pwm = pin.mode == Mode::Pwm;
        return button(frame, pin_button(0, 0), "IN", free, pin.mode == Mode::Input) &&
               button(frame, pin_button(0, 1), "OUT", free, output) &&
               button(frame, pin_button(0, 2), "ADC", adc_allowed(selected_),
                      pin.mode == Mode::Analog) &&
               button(frame, pin_button(1, 0), "PULL", free) &&
               button(frame, pin_button(1, 1), output && pin.level ? "SET LOW" : "SET HIGH",
                      free) &&
               button(frame, pin_button(1, 2), "PWM", pwm, in_pwm) &&
               button(frame, pin_button(2, 0), "DUTY -", in_pwm) &&
               button(frame, pin_button(2, 1), "DUTY +", in_pwm) &&
               button(frame, pin_button(2, 2), "SCOPE", adc_allowed(selected_)) &&
               button(frame, pin_button(3, 0), "< PIN", selected_ > 0) &&
               button(frame, pin_button(3, 1), "LOGIC", true) &&
               button(frame, pin_button(3, 2), "PIN >", selected_ + 1 < pins_.size());
    }

    [[nodiscard]] bool apply(bool ok) {
        message_ = ok ? "" : pins_.last_error();
        pins_.sample();
        return true;
    }

    [[nodiscard]] bool press_pin(unsigned int x, unsigned int y) {
        if (selected_ >= pins_.size()) return false;
        auto& pin = pins_[selected_];
        if (pin_button(0, 0).contains(x, y)) return apply(pins_.make_input(selected_, pin.pull));
        if (pin_button(0, 1).contains(x, y)) return apply(pins_.make_output(selected_, false));
        if (pin_button(0, 2).contains(x, y)) return apply(pins_.make_analog(selected_));
        if (pin_button(1, 0).contains(x, y)) {
            const auto next = pin.pull == Pull::None ? Pull::Up
                              : pin.pull == Pull::Up ? Pull::Down
                                                     : Pull::None;
            return apply(pins_.make_input(selected_, next));
        }
        if (pin_button(1, 1).contains(x, y))
            return apply(pins_.make_output(selected_,
                                           !(pin.mode == Mode::Output && pin.level)));
        if (pin_button(1, 2).contains(x, y)) return apply(pins_.make_pwm(selected_, pin.duty_percent));
        if (pin.mode == Mode::Pwm && pin_button(2, 0).contains(x, y))
            return apply(pins_.make_pwm(selected_, pin.duty_percent >= 10 ? pin.duty_percent - 10 : 0));
        if (pin.mode == Mode::Pwm && pin_button(2, 1).contains(x, y))
            return apply(pins_.make_pwm(selected_, pin.duty_percent + 10));
        if (pin_button(2, 2).contains(x, y) && adc_allowed(selected_)) {
            for (std::size_t i = 0; i < scope_channel_count_; ++i)
                if (scope_channels_[i] == pin.adc_channel) scope_choice_ = i;
            screen_ = Screen::Scope;
            message_ = "";
            if (scope_run_) run_scope();
            return true;
        }
        if (pin_button(3, 0).contains(x, y) && selected_ > 0) {
            --selected_;
            message_ = "";
            return true;
        }
        if (pin_button(3, 1).contains(x, y)) {
            lanes_[0] = selected_;
            screen_ = Screen::Logic;
            message_ = "";
            return true;
        }
        if (pin_button(3, 2).contains(x, y) && selected_ + 1 < pins_.size()) {
            ++selected_;
            message_ = "";
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------ SCOPE

    void collect_scope_channels() {
        scope_channel_count_ = 0;
        for (const auto& channel : mm::mcu::adc_description().channels) {
            if (scope_channel_count_ == scope_channels_.size()) break;
            bool allowed = !channel.gpio.has_value();    // an internal source
            if (channel.gpio) {
                const auto index = pins_.find(*channel.gpio);
                allowed = index != none && adc_allowed(index);
            }
            if (allowed) scope_channels_[scope_channel_count_++] = channel.number;
        }
    }

    [[nodiscard]] unsigned long scope_rate() const {
        return scope_rates[scope_rate_index_];
    }

    void run_scope() {
        if (scope_channel_count_ == 0) {
            message_ = "NO ADC CHANNEL ALLOWED";
            return;
        }
        const auto channel = scope_channels_[scope_choice_];
        const auto maximum = gpiocommander::scope_maximum_rate(channel);
        while (scope_rate_index_ > 0 && scope_rates[scope_rate_index_] > maximum)
            --scope_rate_index_;
        const auto area = trace_area();
        const std::size_t want = std::min<std::size_t>(gpiocommander::scope_depth, 2u * area.w);
        pins_.release_analog_claims();
        const bool ok = gpiocommander::scope_capture(channel, scope_rate(), want, scope_capture);
        pins_.restore_analog_claims();
        message_ = ok ? "" : scope_capture.error;
    }

    // The first rising crossing of the midpoint, with hysteresis, in the
    // first half of the capture; zero when there is none.
    [[nodiscard]] std::size_t scope_trigger_index(unsigned int low, unsigned int high) const {
        if (!scope_trigger_ || high <= low + 8u) return 0;
        const auto mid = (low + high) / 2u;
        const auto band = (high - low) / 10u;
        bool armed = false;
        for (std::size_t i = 0; i < scope_capture.count / 2u; ++i) {
            const auto s = scope_capture.samples[i];
            if (s < mid - band) armed = true;
            else if (armed && s >= mid) return i;
        }
        return 0;
    }

    [[nodiscard]] bool draw_scope(Surface frame) {
        if (scope_channel_count_ == 0)
            return text(frame, "NO ADC CHANNEL ALLOWED", 6, content_top + 4u, quiet);
        const auto channel = scope_channels_[scope_choice_];
        char header[48]{};
        char rate[16]{};
        hertz(rate, sizeof rate, scope_capture.rate_hz != 0 ? scope_capture.rate_hz : scope_rate());
        const char* pin_name = "INT";
        for (const auto& entry : mm::mcu::adc_description().channels)
            if (entry.number == channel && entry.gpio) {
                const auto index = pins_.find(*entry.gpio);
                if (index != none) pin_name = pins_[index].name.data();
            }
        std::snprintf(header, sizeof header, "ADC%u %.5s %sS/s %s%s", channel, pin_name, rate,
                      scope_trigger_ ? "TRIG" : "FREE", scope_capture.paced ? "" : " POLL");
        if (!text(frame, header, 4, content_top, accent)) return false;

        const auto area = trace_area();
        (void)mm::gfx::rectangle(frame, static_cast<int>(area.x), static_cast<int>(area.y), area.w,
                                 area.h, grid);
        for (unsigned int i = 1; i < 4; ++i) {
            const auto gy = static_cast<int>(area.y + area.h * i / 4u);
            (void)mm::gfx::line(frame, static_cast<int>(area.x), gy,
                                static_cast<int>(area.x + area.w - 1u), gy, grid);
            const auto gx = static_cast<int>(area.x + area.w * i / 4u);
            (void)mm::gfx::line(frame, gx, static_cast<int>(area.y), gx,
                                static_cast<int>(area.y + area.h - 1u), grid);
        }

        const auto full = gpiocommander::adc_full_scale(channel);
        unsigned int low = full, high = 0;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < scope_capture.count; ++i) {
            const auto s = scope_capture.samples[i];
            if (s < low) low = s;
            if (s > high) high = s;
            sum += s;
        }
        const auto start = scope_trigger_index(low, high);
        int previous_y = -1;
        for (unsigned int px = 0; px < area.w && start + px < scope_capture.count; ++px) {
            const auto s = std::min<unsigned int>(scope_capture.samples[start + px], full);
            const auto y = static_cast<int>(area.y + area.h - 1u -
                                            static_cast<unsigned int>(
                                                static_cast<std::uint64_t>(s) * (area.h - 1u) / full));
            const auto x = static_cast<int>(area.x + px);
            if (previous_y < 0)
                (void)mm::gfx::pixel(frame, x, y, high_color);
            else
                (void)mm::gfx::line(frame, x - 1, previous_y, x, y, high_color);
            previous_y = y;
        }

        // Readouts: volts when the reference is known, counts otherwise.
        char line[48]{};
        if (scope_capture.count != 0) {
            const auto mean = static_cast<unsigned int>(sum / scope_capture.count);
            unsigned int lo_mv = 0, hi_mv = 0, mean_mv = 0;
            if (gpiocommander::adc_millivolts(channel, low, lo_mv) &&
                gpiocommander::adc_millivolts(channel, high, hi_mv) &&
                gpiocommander::adc_millivolts(channel, mean, mean_mv)) {
                char a[12]{}, b[12]{}, c[12]{};
                volts(a, sizeof a, lo_mv);
                volts(b, sizeof b, hi_mv);
                volts(c, sizeof c, mean_mv);
                std::snprintf(line, sizeof line, "MIN %s MAX %s AVG %s V", a, b, c);
            } else {
                std::snprintf(line, sizeof line, "MIN %u MAX %u AVG %u", low, high, mean);
            }
        } else {
            std::snprintf(line, sizeof line, "NO SAMPLES");
        }
        const auto readout_y = area.y + area.h + 4u;
        if (!text(frame, line, 4, readout_y, white)) return false;

        char freq[16] = "-";
        const auto f = scope_frequency(low, high);
        if (f != 0) hertz(freq, sizeof freq, f);
        char window[16]{};
        const auto rate_hz = scope_capture.rate_hz != 0 ? scope_capture.rate_hz : scope_rate();
        hertz(window, sizeof window, rate_hz / std::max(1u, area.w));
        if (message_[0] != 0)
            std::snprintf(line, sizeof line, "%s", message_);
        else
            std::snprintf(line, sizeof line, "F %sHz  SCREEN 1/%sHz%s", freq, window,
                          scope_capture.missed != 0 ? " DROP" : "");
        if (!text(frame, line, 4, readout_y + 18u, message_[0] != 0 ? error_color : quiet))
            return false;

        return button(frame, bar_button(0, 5), "CH", scope_channel_count_ > 1) &&
               button(frame, bar_button(1, 5), "RATE-", scope_rate_index_ > 0) &&
               button(frame, bar_button(2, 5), "RATE+", scope_rate_index_ + 1 < scope_rates.size()) &&
               button(frame, bar_button(3, 5), "TRIG", true, scope_trigger_) &&
               button(frame, bar_button(4, 5), scope_run_ ? "HOLD" : "RUN", true, scope_run_);
    }

    // Rising midpoint crossings across the capture, as a frequency.
    [[nodiscard]] unsigned long scope_frequency(unsigned int low, unsigned int high) const {
        if (high <= low + 8u || scope_capture.count < 4) return 0;
        const auto mid = (low + high) / 2u;
        const auto band = (high - low) / 10u;
        bool armed = false;
        std::size_t first = 0, last = 0, crossings = 0;
        for (std::size_t i = 0; i < scope_capture.count; ++i) {
            const auto s = scope_capture.samples[i];
            if (s < mid - band) {
                armed = true;
            } else if (armed && s >= mid) {
                armed = false;
                if (crossings == 0) first = i;
                last = i;
                ++crossings;
            }
        }
        if (crossings < 2 || last == first) return 0;
        const auto rate = scope_capture.rate_hz != 0 ? scope_capture.rate_hz : scope_rate();
        return static_cast<unsigned long>(static_cast<std::uint64_t>(crossings - 1) * rate /
                                          (last - first));
    }

    [[nodiscard]] bool press_scope(unsigned int x, unsigned int y) {
        if (bar_button(0, 5).contains(x, y) && scope_channel_count_ > 1)
            scope_choice_ = (scope_choice_ + 1) % scope_channel_count_;
        else if (bar_button(1, 5).contains(x, y) && scope_rate_index_ > 0)
            --scope_rate_index_;
        else if (bar_button(2, 5).contains(x, y) && scope_rate_index_ + 1 < scope_rates.size())
            ++scope_rate_index_;
        else if (bar_button(3, 5).contains(x, y))
            scope_trigger_ = !scope_trigger_;
        else if (bar_button(4, 5).contains(x, y))
            scope_run_ = !scope_run_;
        else
            return false;
        if (scope_run_) run_scope();
        return true;
    }

    // ------------------------------------------------------------ LOGIC

    void choose_default_lanes() {
        std::size_t lane = 0;
        for (std::size_t i = 0; i < pins_.size() && lane < lanes_.size(); ++i)
            if (!pins_[i].reserved) lanes_[lane++] = i;
        for (std::size_t i = 0; lane < lanes_.size(); ++i, ++lane)
            lanes_[lane] = i < pins_.size() ? i : 0;
    }

    void run_logic() {
        if (pins_.size() == 0) {
            message_ = "NO GPIO ON THIS BOARD";
            return;
        }
        std::array<unsigned int, gpiocommander::logic_lanes> gpios{};
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
            pins_.prepare_for_reading(lanes_[lane]);
            gpios[lane] = pins_[lanes_[lane]].gpio;
        }
        const bool ok = gpiocommander::logic_capture(gpios, logic_periods[logic_rate_index_],
                                                     logic_trigger_, logic_trigger_timeout_ms,
                                                     logic_capture);
        logic_offset_ = 0;
        message_ = !ok                     ? logic_capture.error
                   : logic_capture.timed_out ? "TRIGGER TIMEOUT"
                                             : "";
    }

    [[nodiscard]] Rect lane_rect(std::size_t lane) const {
        const auto area = trace_area();
        const auto h = area.h / static_cast<unsigned int>(lanes_.size());
        return {area.x, area.y + static_cast<unsigned int>(lane) * h, area.w, h};
    }

    [[nodiscard]] unsigned int logic_label_width() const { return 44u; }

    [[nodiscard]] bool draw_logic(Surface frame) {
        char header[48]{};
        char rate[16]{};
        hertz(rate, sizeof rate, logic_capture.rate_hz);
        const char* trig = logic_trigger_ == Trigger::Rising    ? "RISE"
                           : logic_trigger_ == Trigger::Falling ? "FALL"
                                                                : "NONE";
        std::snprintf(header, sizeof header, "SET %s GOT %sS/s TRIG %s",
                      logic_period_names[logic_rate_index_],
                      logic_capture.count != 0 ? rate : "-", trig);
        if (!text(frame, header, 4, content_top, accent)) return false;

        const auto zoom = logic_zooms[logic_zoom_index_];
        const auto label_w = logic_label_width();
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
            const auto r = lane_rect(lane);
            const auto& pin = pins_[lanes_[lane]];
            (void)mm::gfx::fill_rectangle(frame, static_cast<int>(r.x), static_cast<int>(r.y + 1u),
                                          label_w - 2u, r.h - 2u, panel);
            char label[12]{};
            const std::string_view name = pin.name;
            std::snprintf(label, sizeof label, "%.*s", static_cast<int>(name.size()), name.data());
            if (!text(frame, label, r.x + 3u, r.y + 2u, lane_colors[lane])) return false;
            if (r.h >= 40u) {
                char f[16] = "-";
                const auto hz = lane_frequency(lane);
                if (hz != 0) hertz(f, sizeof f, hz);
                if (!text(frame, f, r.x + 3u, r.y + 20u, quiet)) return false;
            }
            // The trace: high near the lane's top, low near its bottom.
            const auto x0 = r.x + label_w;
            const auto w = r.w - label_w;
            const auto top = static_cast<int>(r.y + 4u);
            const auto bottom = static_cast<int>(r.y + r.h - 5u);
            (void)mm::gfx::line(frame, static_cast<int>(x0), static_cast<int>(r.y + r.h - 1u),
                                static_cast<int>(x0 + w - 1u), static_cast<int>(r.y + r.h - 1u),
                                grid);
            int previous = -1;
            for (unsigned int px = 0; px < w; ++px) {
                const auto index = logic_offset_ + static_cast<std::size_t>(px) * zoom;
                if (index >= logic_capture.count) break;
                // A pixel covering several samples is high if any is, and
                // draws an edge if they differ, so no pulse disappears.
                bool any_high = false, any_low = false;
                for (unsigned int k = 0; k < zoom && index + k < logic_capture.count; ++k) {
                    const bool level = (logic_capture.samples[index + k] >> lane) & 1u;
                    any_high = any_high || level;
                    any_low = any_low || !level;
                }
                const auto x = static_cast<int>(x0 + px);
                const int y = any_high ? top : bottom;
                if ((any_high && any_low) || (previous >= 0 && previous != y))
                    (void)mm::gfx::line(frame, x, top, x, bottom, lane_colors[lane]);
                else
                    (void)mm::gfx::pixel(frame, x, y, lane_colors[lane]);
                previous = any_high && !any_low ? top : (any_low && !any_high ? bottom : previous);
            }
        }

        const auto area = trace_area();
        char line[48]{};
        if (message_[0] != 0)
            std::snprintf(line, sizeof line, "%s", message_);
        else if (logic_capture.count == 0)
            std::snprintf(line, sizeof line, "TAP A LANE NAME TO CHANGE PIN");
        else
            std::snprintf(line, sizeof line, "%zu-%zu OF %zu  x%u  LATE %zu", logic_offset_,
                          std::min(logic_capture.count,
                                   logic_offset_ + static_cast<std::size_t>(area.w - label_w) * zoom),
                          logic_capture.count, zoom, logic_capture.late);
        if (!text(frame, line, 4, area.y + area.h + 4u, message_[0] != 0 ? error_color : quiet))
            return false;
        if (!text(frame, "LANE 1 IS THE TRIGGER", 4, area.y + area.h + 22u, quiet)) return false;

        return button(frame, bar_button(0, 6), "RATE", true) &&
               button(frame, bar_button(1, 6), "TRIG", true, logic_trigger_ != Trigger::None) &&
               button(frame, bar_button(2, 6), "RUN", true) &&
               button(frame, bar_button(3, 6), "<", logic_offset_ > 0) &&
               button(frame, bar_button(4, 6), ">", logic_capture.count != 0) &&
               button(frame, bar_button(5, 6), "ZOOM", true);
    }

    // Rising edges across the capture, as a frequency.
    [[nodiscard]] unsigned long lane_frequency(std::size_t lane) const {
        std::size_t first = 0, last = 0, edges = 0;
        for (std::size_t i = 1; i < logic_capture.count; ++i) {
            const bool before = (logic_capture.samples[i - 1] >> lane) & 1u;
            const bool now = (logic_capture.samples[i] >> lane) & 1u;
            if (before || !now) continue;
            if (edges == 0) first = i;
            last = i;
            ++edges;
        }
        if (edges < 2 || last == first || logic_capture.rate_hz == 0) return 0;
        return static_cast<unsigned long>(static_cast<std::uint64_t>(edges - 1) *
                                          logic_capture.rate_hz / (last - first));
    }

    [[nodiscard]] bool press_logic(unsigned int x, unsigned int y) {
        const auto area = trace_area();
        const auto window = static_cast<std::size_t>(area.w - logic_label_width()) *
                            logic_zooms[logic_zoom_index_];
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
            const auto r = lane_rect(lane);
            if (x < r.x + logic_label_width() && r.contains(x, y) && pins_.size() != 0) {
                lanes_[lane] = (lanes_[lane] + 1) % pins_.size();
                return true;
            }
        }
        if (bar_button(0, 6).contains(x, y)) {
            logic_rate_index_ = (logic_rate_index_ + 1) % logic_periods.size();
        } else if (bar_button(1, 6).contains(x, y)) {
            logic_trigger_ = logic_trigger_ == Trigger::None     ? Trigger::Rising
                             : logic_trigger_ == Trigger::Rising ? Trigger::Falling
                                                                 : Trigger::None;
        } else if (bar_button(2, 6).contains(x, y)) {
            run_logic();
        } else if (bar_button(3, 6).contains(x, y)) {
            logic_offset_ = logic_offset_ > window / 2 ? logic_offset_ - window / 2 : 0;
        } else if (bar_button(4, 6).contains(x, y)) {
            if (logic_offset_ + window / 2 < logic_capture.count) logic_offset_ += window / 2;
        } else if (bar_button(5, 6).contains(x, y)) {
            logic_zoom_index_ = (logic_zoom_index_ + 1) % logic_zooms.size();
        } else {
            return false;
        }
        return true;
    }

    mm::display::Display& display_;
    unsigned int width_;
    unsigned int height_;
    Pins pins_{};
    Screen screen_ = Screen::Pins;
    std::size_t selected_ = 0;
    std::size_t page_ = 0;
    const char* message_ = "";
    unsigned long last_sample_ms_ = 0;

    std::array<unsigned int, 16> scope_channels_{};
    std::size_t scope_channel_count_ = 0;
    std::size_t scope_choice_ = 0;
    std::size_t scope_rate_index_ = 4;    // 10 kHz
    bool scope_trigger_ = true;
    bool scope_run_ = true;

    std::array<std::size_t, gpiocommander::logic_lanes> lanes_{};
    std::size_t logic_rate_index_ = 5;    // 20 kS/s
    Trigger logic_trigger_ = Trigger::None;
    std::size_t logic_offset_ = 0;
    std::size_t logic_zoom_index_ = 0;
};

struct Devices {
    mm::display::Display& display;
    mm::touch::Touch& touch;
    bool display_ready = false;
    bool touch_ready = false;
    ~Devices() {
        if (touch_ready) (void)touch.sleep();
        if (display_ready) (void)display.sleep();
    }
};


} // namespace

int main() {
    auto& display = mm::display::selected_display();
    auto& touch = mm::touch::selected_touch();
    Devices devices{display, touch};
    if (display.initialize() != Status::Ok) return 1;
    devices.display_ready = true;
    if (touch.initialize() != mm::touch::Status::Ok) return 2;
    devices.touch_ready = true;
    const auto panel_geometry = display.geometry();
    const auto sensor = touch.geometry();
    if (panel_geometry.bits_per_pixel != 16 || panel_geometry.width < 220 ||
        panel_geometry.width > maximum_width || panel_geometry.height < 220 ||
        panel_geometry.height > maximum_height || sensor.width == 0 || sensor.height == 0)
        return 3;

    static App app{display, panel_geometry.width, panel_geometry.height};
    app.start();
    if (!app.draw()) return 4;

    std::array<mm::touch::Point, 1> points{};
    bool was_touched = false;
    unsigned long last_touch_ms = 0;
    for (;;) {
        std::size_t count = 0;
        if (touch.read(points, count) != mm::touch::Status::Ok) {
            app.stop();
            return 5;
        }
        unsigned long now_ms = 0;
        if (mm::mcu::ticks_ms(now_ms) != mm::mcu::Status::Ok) {
            app.stop();
            return 6;
        }
        bool touched = count != 0;
        bool redraw = false;
        if (touched) {
            const auto x = static_cast<unsigned int>(static_cast<std::uint64_t>(points[0].x) *
                                                     panel_geometry.width / sensor.width);
            const auto y = static_cast<unsigned int>(static_cast<std::uint64_t>(points[0].y) *
                                                     panel_geometry.height / sensor.height);
            if (!was_touched) redraw = app.press(x, y);
            last_touch_ms = now_ms;
        } else if (was_touched && now_ms - last_touch_ms < 35u) {
            // The touch controller can report one empty frame during a hold.
            touched = true;
        }
        was_touched = touched;
        if (app.tick(now_ms)) redraw = true;
        if (redraw && !app.draw()) {
            app.stop();
            return 7;
        }
        if (!app.busy() && mm::mcu::delay_ms(15) != mm::mcu::Status::Ok) {
            app.stop();
            return 8;
        }
    }
}
