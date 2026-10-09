#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

import mm.display;
import mm.fonts;
import mm.gfx;
import mm.mcu;
import mm.touch;

namespace chip8 {
bool load(std::span<const std::uint8_t> rom);
std::span<const std::uint8_t> game_rom();
void key(unsigned int number, bool down);
bool pixel(unsigned int x, unsigned int y);
bool dirty();
void acknowledge_frame();
void tick();
bool step();
}

namespace {

constexpr unsigned int maximum_width = 480;
constexpr unsigned int maximum_height = 320;
constexpr unsigned int text_limit = 24;
constexpr unsigned int game_top = 32;
constexpr unsigned int keypad_top = 160;
constexpr unsigned int game_bottom = 132;
constexpr unsigned int key_margin = 8;
constexpr unsigned int key_gap = 5;
std::array<std::byte, maximum_width * maximum_height * 2u> frame_bytes;
std::array<std::byte,
           ((text_limit * mm::fonts::kMono16.advance + 7u) / 8u) *
               mm::fonts::kMono16.height> text_bytes;

using mm::display::Status;
using mm::gfx::Rgb565;
using mm::gfx::Surface;

constexpr Rgb565 background = mm::gfx::rgb(8, 15, 27);
constexpr Rgb565 screen_background = mm::gfx::rgb(20, 42, 41);
constexpr Rgb565 screen_ink = mm::gfx::rgb(130, 246, 188);
constexpr Rgb565 key_background = mm::gfx::rgb(37, 56, 79);
constexpr Rgb565 active_background = mm::gfx::rgb(28, 117, 143);
constexpr Rgb565 white = mm::gfx::rgb565_white;

// Standard CHIP-8 keypad, shown in reading order.
constexpr std::array<std::uint8_t, 16> keypad{{
    0x1, 0x2, 0x3, 0xC,
    0x4, 0x5, 0x6, 0xD,
    0x7, 0x8, 0x9, 0xE,
    0xA, 0x0, 0xB, 0xF,
}};
constexpr char hex[] = "0123456789ABCDEF";

struct Layout {
    unsigned int width;
    unsigned int height;
    unsigned int scale;
    unsigned int screen_x;
    unsigned int screen_y;
    unsigned int keypad_top;
    unsigned int key_margin;
    unsigned int key_gap;
    bool compact;

    [[nodiscard]] unsigned int key_width() const {
        return (width - 2u * key_margin - 3u * key_gap) / 4u;
    }
    [[nodiscard]] unsigned int key_height() const {
        return (height - keypad_top - key_margin - 3u * key_gap) / 4u;
    }
    [[nodiscard]] mm::display::Rectangle key_box(unsigned int index) const {
        const auto column = index % 4u;
        const auto row = index / 4u;
        return {key_margin + column * (key_width() + key_gap),
                keypad_top + row * (key_height() + key_gap),
                key_width(), key_height()};
    }
};

[[nodiscard]] bool label(Surface target, const char* value, unsigned int x,
                         unsigned int y, Rgb565 color,
                         const mm::fonts::Font& font = mm::fonts::kMono16) {
    std::size_t count = 0;
    char8_t characters[text_limit]{};
    while (value[count] != 0) {
        if (count == text_limit) return false;
        characters[count] = static_cast<char8_t>(value[count]);
        ++count;
    }
    if (count == 0) return true;
    const auto width = static_cast<unsigned int>(count) * font.advance;
    const auto row_bytes = (width + 7u) / 8u;
    const auto bytes = row_bytes * font.height;
    const Surface glyphs{width, font.height, 1,
                         std::span<std::byte>{text_bytes}.first(bytes)};
    if (mm::gfx::fill(glyphs, mm::display::Color::Black) != Status::Ok ||
        mm::fonts::render(characters, count, font,
                          mm::display::Color::White, 0, 0, glyphs) != Status::Ok)
        return false;
    for (unsigned int row = 0; row < glyphs.height; ++row)
        for (unsigned int column = 0; column < glyphs.width; ++column) {
            const auto bits = glyphs.pixels[row * row_bytes + column / 8u];
            if ((bits & static_cast<std::byte>(0x80u >> (column % 8u))) !=
                    std::byte{0} &&
                mm::gfx::pixel(target, static_cast<int>(x + column),
                               static_cast<int>(y + row), color) != Status::Ok)
                return false;
        }
    return true;
}

[[nodiscard]] bool render(mm::display::Display& display, const Layout& layout,
                          int held_key) {
    const auto bytes = static_cast<std::size_t>(layout.width) * layout.height * 2u;
    const Surface target{layout.width, layout.height, 16,
                         std::span<std::byte>{frame_bytes}.first(bytes)};
    if (mm::gfx::fill(target, background) != Status::Ok)
        return false;

    if (layout.compact) {
        if (!label(target, "CATCH THE DOT", 6, 2, white, mm::fonts::kMono12) ||
            !label(target, "4< >6",
                   layout.width - 5u * mm::fonts::kMono12.advance - 6u, 2,
                   white, mm::fonts::kMono12))
            return false;
    } else {
        if (!label(target, "CATCH THE DOT", 8, 5, white, mm::fonts::kMono16) ||
            !label(target, "4 LEFT  6 RIGHT", 8, 136, white, mm::fonts::kMono16))
            return false;
    }

    const unsigned int screen_width = 64u * layout.scale;
    const unsigned int screen_height = 32u * layout.scale;
    if (mm::gfx::fill_rectangle(target, static_cast<int>(layout.screen_x),
                                static_cast<int>(layout.screen_y),
                                screen_width, screen_height,
                                screen_background) != Status::Ok)
        return false;
    for (unsigned int y = 0; y < 32; ++y)
        for (unsigned int x = 0; x < 64; ++x)
            if (chip8::pixel(x, y) &&
                mm::gfx::fill_rectangle(
                    target, static_cast<int>(layout.screen_x + x * layout.scale),
                    static_cast<int>(layout.screen_y + y * layout.scale),
                    layout.scale, layout.scale, screen_ink) != Status::Ok)
                return false;

    const auto& key_font = layout.compact ? mm::fonts::kMono12 : mm::fonts::kMono16;
    for (unsigned int index = 0; index < keypad.size(); ++index) {
        const auto box = layout.key_box(index);
        if (mm::gfx::fill_rectangle(
                target, static_cast<int>(box.x), static_cast<int>(box.y),
                box.width, box.height,
                held_key == static_cast<int>(keypad[index])
                    ? active_background : key_background) != Status::Ok)
            return false;
        const char character[] = {hex[keypad[index]], 0};
        const auto x = box.x +
            (box.width - key_font.advance) / 2u;
        const auto y = box.y +
            (box.height - key_font.height) / 2u;
        if (!label(target, character, x, y, white, key_font)) return false;
    }
    if (mm::gfx::write(display, target, 0, 0) != Status::Ok ||
        display.refresh(mm::display::Refresh::Full) != Status::Ok)
        return false;
    chip8::acknowledge_frame();
    return true;
}

[[nodiscard]] int touch_key(const Layout& layout, unsigned int x,
                            unsigned int y) {
    for (unsigned int index = 0; index < keypad.size(); ++index) {
        const auto box = layout.key_box(index);
        if (x >= box.x && y >= box.y &&
            x - box.x < box.width && y - box.y < box.height)
            return keypad[index];
    }
    return -1;
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

}  // namespace

int main() {
    auto& display = mm::display::selected_display();
    auto& touch = mm::touch::selected_touch();
    Devices devices{display, touch};
    if (display.initialize() != Status::Ok) return 1;
    devices.display_ready = true;
    if (touch.initialize() != mm::touch::Status::Ok) return 2;
    devices.touch_ready = true;

    const auto panel = display.geometry();
    const auto sensor = touch.geometry();
    if (panel.bits_per_pixel != 16 || panel.width < 220 ||
        panel.width > maximum_width || panel.height < 220 ||
        panel.height > maximum_height || sensor.width == 0 ||
        sensor.height == 0)
        return 3;
    const bool compact = panel.height < 280u;
    const unsigned int scale = compact ? 3u : (
        (panel.width / 64u) < ((game_bottom - game_top) / 32u) ?
        (panel.width / 64u) : ((game_bottom - game_top) / 32u));
    const unsigned int screen_x = (panel.width - 64u * scale) / 2u;
    const unsigned int screen_y = compact ? 20u : (game_top + (game_bottom - game_top - 32u * scale) / 2u);
    const unsigned int kp_top = compact ? 120u : keypad_top;
    const unsigned int kp_margin = compact ? 6u : key_margin;
    const unsigned int kp_gap = compact ? 4u : key_gap;
    const Layout layout{panel.width, panel.height, scale,
                        screen_x, screen_y, kp_top, kp_margin, kp_gap, compact};
    if (!chip8::load(chip8::game_rom())) return 4;
    if (!render(display, layout, -1)) return 5;

    unsigned long last_ms = 0;
    if (mm::mcu::ticks_ms(last_ms) != mm::mcu::Status::Ok) return 6;
    unsigned long cpu_fraction = 0;
    unsigned long timer_fraction = 0;
    int held_key = -1;
    std::array<mm::touch::Point, 1> points{};

    for (;;) {
        std::size_t count = 0;
        if (touch.read(points, count) != mm::touch::Status::Ok) return 7;
        int next_key = -1;
        if (count != 0) {
            const auto x = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].x) * panel.width /
                sensor.width);
            const auto y = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].y) * panel.height /
                sensor.height);
            next_key = touch_key(layout, x, y);
        }
        const bool key_changed = next_key != held_key;
        if (key_changed) {
            if (held_key >= 0) chip8::key(static_cast<unsigned int>(held_key), false);
            if (next_key >= 0) chip8::key(static_cast<unsigned int>(next_key), true);
            held_key = next_key;
        }

        unsigned long now_ms = 0;
        if (mm::mcu::ticks_ms(now_ms) != mm::mcu::Status::Ok) return 8;
        unsigned long elapsed = now_ms - last_ms;
        last_ms = now_ms;
        if (elapsed > 100u) elapsed = 100u;
        cpu_fraction += elapsed * 700u;
        timer_fraction += elapsed * 60u;
        while (cpu_fraction >= 1000u) {
            cpu_fraction -= 1000u;
            if (!chip8::step()) return 9;
        }
        while (timer_fraction >= 1000u) {
            timer_fraction -= 1000u;
            chip8::tick();
        }
        if ((chip8::dirty() || key_changed) &&
            !render(display, layout, held_key))
            return 10;
        if (mm::mcu::delay_ms(4) != mm::mcu::Status::Ok) return 11;
    }
}
