#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import mm.display;
import mm.fonts;
import mm.gfx;
import mm.mcu;
import mm.rtc;
import mm.stdio;
import mm.touch;

bool tdset_poll(mm::rtc::Clock& clock, mm::stdio::Console& console);

namespace {
using mm::display::Color;
using mm::display::Status;
using mm::gfx::Surface;

constexpr unsigned int maximum_width = 480;
constexpr unsigned int maximum_height = 320;
constexpr unsigned int poll_ms = 50;
constexpr unsigned int read_every_polls = 5;
constexpr std::array<unsigned char, 10> digits{
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};
constexpr std::array<std::string_view, 12> months{
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};
constexpr std::array<std::string_view, 7> weekdays{
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

std::array<std::byte, ((maximum_width + 7u) / 8u) * maximum_height> pixels{};
std::array<std::byte, maximum_width * 2u> scanline{};

enum class Theme { Amber, Green };

struct State {
    Theme theme = Theme::Amber;
    mm::rtc::DateTime time{};
    mm::rtc::Status status = mm::rtc::Status::NotInitialized;
    bool ready = false;
    bool trusted = false;
    bool usable = false;
};

bool valid_date(const mm::rtc::DateTime& time) {
    if (time.year < 1900 || time.year > 9999 || time.month < 1 || time.month > 12 ||
        time.hour > 23 || time.minute > 59 || time.second > 59) return false;
    constexpr std::array<unsigned int, 12> days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    unsigned int last = days[time.month - 1u];
    if (time.month == 2 && time.year % 4u == 0 &&
        (time.year % 100u != 0 || time.year % 400u == 0)) ++last;
    return time.day >= 1 && time.day <= last;
}

unsigned int weekday(const mm::rtc::DateTime& time) {
    constexpr std::array<unsigned int, 12> offsets{0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    unsigned int year = time.year - (time.month < 3u ? 1u : 0u);
    return (year + year / 4u - year / 100u + year / 400u +
            offsets[time.month - 1u] + time.day) % 7u;
}

bool changed(const State& old, const State& next) {
    return old.theme != next.theme || old.ready != next.ready ||
           old.trusted != next.trusted || old.usable != next.usable ||
           old.status != next.status ||
           old.time.year != next.time.year || old.time.month != next.time.month ||
           old.time.day != next.time.day || old.time.hour != next.time.hour ||
           old.time.minute != next.time.minute || old.time.second != next.time.second;
}

bool rectangle(Surface surface, unsigned int x, unsigned int y,
               unsigned int width, unsigned int height) {
    return mm::gfx::fill_rectangle(surface, static_cast<int>(x), static_cast<int>(y),
                                   width, height, Color::Black) == Status::Ok;
}

bool label(Surface surface, std::string_view text, unsigned int x,
           unsigned int y, const mm::fonts::Font& font) {
    std::array<char8_t, 40> characters{};
    const std::size_t count = text.size() < characters.size() ?
        text.size() : characters.size();
    for (std::size_t i = 0; i < count; ++i)
        characters[i] = static_cast<char8_t>(text[i]);
    return mm::fonts::render(characters.data(), count, font, Color::Black,
                             x, y, surface) == Status::Ok;
}

bool centered_label(Surface surface, std::string_view text, unsigned int y,
                    const mm::fonts::Font& font) {
    const unsigned int width = static_cast<unsigned int>(text.size()) * font.advance;
    return label(surface, text, surface.width > width ? (surface.width - width) / 2u : 0u,
                 y, font);
}

bool digit(Surface surface, unsigned int x, unsigned int y,
           unsigned int width, unsigned int height, char character) {
    unsigned char segments = character == '-' ? 0x40u :
        character >= '0' && character <= '9' ? digits[character - '0'] : 0u;
    const unsigned int thick = width / 7u > 3u ? width / 7u : 3u;
    const unsigned int half = height / 2u;
    const unsigned int horizontal = width - 2u * thick;
    const unsigned int vertical = half - thick;
    if ((segments & 0x01u) && !rectangle(surface, x + thick, y, horizontal, thick)) return false;
    if ((segments & 0x02u) && !rectangle(surface, x + width - thick, y + thick,
                                        thick, vertical)) return false;
    if ((segments & 0x04u) && !rectangle(surface, x + width - thick, y + half,
                                        thick, vertical)) return false;
    if ((segments & 0x08u) && !rectangle(surface, x + thick, y + height - thick,
                                        horizontal, thick)) return false;
    if ((segments & 0x10u) && !rectangle(surface, x, y + half, thick, vertical)) return false;
    if ((segments & 0x20u) && !rectangle(surface, x, y + thick, thick, vertical)) return false;
    return !(segments & 0x40u) ||
           rectangle(surface, x + thick, y + half - thick / 2u, horizontal, thick);
}

bool draw_time(Surface surface, unsigned int y, const State& state) {
    const unsigned int width = (surface.width - 24u) / 8u;
    const unsigned int height = width * 2u;
    const unsigned int gap = width / 9u > 3u ? width / 9u : 3u;
    const unsigned int colon_width = width / 4u > 6u ? width / 4u : 6u;
    const unsigned int total = 6u * width + 2u * colon_width + 7u * gap;
    unsigned int x = (surface.width - total) / 2u;
    if (x >= 8u && y >= 10u &&
        mm::gfx::rectangle(surface, static_cast<int>(x - 8u), static_cast<int>(y - 10u),
                           total + 16u, height + 20u, Color::Black) != Status::Ok) return false;

    std::array<char, 8> time{'-', '-', ':', '-', '-', ':', '-', '-'};
    if (state.usable) {
        time[0] = static_cast<char>('0' + state.time.hour / 10u);
        time[1] = static_cast<char>('0' + state.time.hour % 10u);
        time[3] = static_cast<char>('0' + state.time.minute / 10u);
        time[4] = static_cast<char>('0' + state.time.minute % 10u);
        time[6] = static_cast<char>('0' + state.time.second / 10u);
        time[7] = static_cast<char>('0' + state.time.second % 10u);
    }
    for (unsigned int i = 0; i < time.size(); ++i) {
        if (time[i] == ':') {
            const unsigned int thick = width / 7u > 3u ? width / 7u : 3u;
            const unsigned int center = x + colon_width / 2u - thick / 2u;
            if (!rectangle(surface, center, y + height / 3u - thick / 2u,
                           thick, thick) ||
                !rectangle(surface, center, y + 2u * height / 3u - thick / 2u,
                           thick, thick)) return false;
            x += colon_width;
        } else {
            if (!digit(surface, x, y, width, height, time[i])) return false;
            x += width;
        }
        if (i + 1u < time.size()) x += gap;
    }
    return true;
}

std::array<char, 15> date_text(const mm::rtc::DateTime& time) {
    std::array<char, 15> text{};
    const auto day_name = weekdays[weekday(time)];
    const auto month_name = months[time.month - 1u];
    for (unsigned int i = 0; i < 3u; ++i) {
        text[i] = day_name[i];
        text[7u + i] = month_name[i];
    }
    text[3] = ' ';
    text[4] = static_cast<char>('0' + time.day / 10u);
    text[5] = static_cast<char>('0' + time.day % 10u);
    text[6] = ' ';
    text[10] = ' ';
    text[11] = static_cast<char>('0' + time.year / 1000u % 10u);
    text[12] = static_cast<char>('0' + time.year / 100u % 10u);
    text[13] = static_cast<char>('0' + time.year / 10u % 10u);
    text[14] = static_cast<char>('0' + time.year % 10u);
    return text;
}

const char* message(const State& state) {
    if (!state.ready) return "RTC NOT AVAILABLE";
    if (state.status != mm::rtc::Status::Ok) return "RTC READ FAILED";
    if (!valid_date(state.time)) return "INVALID RTC DATE";
#if defined(__linux__)
    return "UTC SYSTEM TIME";
#else
    return state.trusted ? "RTC VERIFIED" : "RTC NEEDS SETTING";
#endif
}

bool render(mm::display::Display& display, const State& state) {
    const auto geometry = display.geometry();
    Surface surface{geometry.width, geometry.height, 1,
        std::span<std::byte>{pixels}.first(((geometry.width + 7u) / 8u) * geometry.height)};
    if (mm::gfx::fill(surface, Color::White) != Status::Ok) return false;
    const auto theme = state.theme == Theme::Amber ? "AMBER >" : "GREEN >";
    const unsigned int theme_width = 7u * mm::fonts::kMono12.advance;
    if (!label(surface, "CLOCKAL", 8, 6, mm::fonts::kMono12) ||
        !label(surface, theme, surface.width - theme_width - 8u, 6,
               mm::fonts::kMono12) ||
        !rectangle(surface, 8, 29, surface.width - 16u, 1)) return false;
    const unsigned int digit_height = ((surface.width - 24u) / 8u) * 2u;
    const unsigned int clock_y = (surface.height - digit_height) / 2u - 22u;
    if (!draw_time(surface, clock_y, state)) return false;
    const unsigned int date_y = clock_y + digit_height + 27u;
    if (state.usable) {
        const auto date = date_text(state.time);
        if (!centered_label(surface, {date.data(), date.size()}, date_y,
                            mm::fonts::kMono16)) return false;
    } else if (!centered_label(surface, "-- --- ----", date_y,
                                mm::fonts::kMono16)) return false;
    if (!centered_label(surface, message(state), surface.height - 39u,
                        mm::fonts::kMono12) ||
        !centered_label(surface, "24-HOUR CLOCK", surface.height - 20u,
                        mm::fonts::kMono12)) return false;

    const auto foreground = state.theme == Theme::Amber ?
        mm::gfx::rgb(255, 187, 54) : mm::gfx::rgb(83, 255, 107);
    const auto background = state.theme == Theme::Amber ?
        mm::gfx::rgb(8, 5, 2) : mm::gfx::rgb(2, 8, 4);
    const auto written = geometry.bits_per_pixel == 1 ?
        mm::gfx::write(display, surface, 0, 0) :
        mm::gfx::write(display, surface, 0, 0, {foreground, background}, {},
                       std::span<std::byte>{scanline}.first(surface.width * 2u));
    return written == Status::Ok &&
           display.refresh(mm::display::Refresh::Full) == Status::Ok;
}

void sample(mm::rtc::Clock& clock, State& state) {
    if (!state.ready) {
        state.status = clock.initialize();
        state.ready = state.status == mm::rtc::Status::Ok;
        if (!state.ready) { state.usable = false; return; }
    }
    state.status = clock.read(state.time, state.trusted);
#if defined(__linux__)
    state.usable = state.status == mm::rtc::Status::Ok && valid_date(state.time);
#else
    state.usable = state.status == mm::rtc::Status::Ok && state.trusted &&
                   valid_date(state.time);
#endif
}

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
}

int main() {
    auto& display = mm::display::selected_display();
    auto& touch = mm::touch::selected_touch();
    auto& clock = mm::rtc::selected_clock();
#if !defined(__linux__)
    auto& console = mm::stdio::selected_console();
    const bool console_ready = console.initialize() == mm::stdio::Status::Ok;
#endif
    Devices devices{display, touch};
    if (display.initialize() != Status::Ok) return 1;
    devices.display_ready = true;
    if (touch.initialize() != mm::touch::Status::Ok) return 2;
    devices.touch_ready = true;
    const auto panel = display.geometry();
    const auto sensor = touch.geometry();
    if ((panel.bits_per_pixel != 1 && panel.bits_per_pixel != 16) ||
        panel.width < 220u || panel.width > maximum_width ||
        panel.height < 240u || panel.height > maximum_height ||
        sensor.width == 0u || sensor.height == 0u) return 3;

    State state{};
    sample(clock, state);
    if (!render(display, state)) return 4;
    bool was_down = false;
    unsigned int polls = 0;
    std::array<mm::touch::Point, 1> points{};
    for (;;) {
        State next = state;
#if !defined(__linux__)
        if (console_ready && tdset_poll(clock, console)) {
            sample(clock, next);
            polls = 0;
        }
#endif
        if (++polls == read_every_polls) {
            polls = 0;
            sample(clock, next);
        }
        std::size_t contacts = 0;
        if (touch.read(points, contacts) != mm::touch::Status::Ok) return 5;
        const bool down = contacts != 0;
        if (down && !was_down) {
            const unsigned int y = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].y) * panel.height / sensor.height);
            if (y < 32u)
                next.theme = next.theme == Theme::Amber ? Theme::Green : Theme::Amber;
        }
        was_down = down;
        if (changed(state, next)) {
            state = next;
            if (!render(display, state)) return 6;
        }
        if (mm::mcu::delay_ms(poll_ms) != mm::mcu::Status::Ok) return 7;
    }
}
