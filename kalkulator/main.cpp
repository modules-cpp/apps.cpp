#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

import mm.display;
import mm.fonts;
import mm.gfx;
import mm.mcu;
import mm.touch;

namespace kalkulator {
void clear();
void press(char key);
const char* display();
char operation();
}

namespace {

constexpr unsigned int maximum_width = 480;
constexpr unsigned int maximum_height = 320;
constexpr unsigned int text_limit = 24;
constexpr unsigned int poll_ms = 25;
std::array<std::byte, maximum_width * maximum_height * 2u> pixels;
std::array<std::byte,
           ((text_limit * mm::fonts::kMono16.advance + 7u) / 8u) *
               mm::fonts::kMono16.height> text_bits;

using mm::display::Status;
using mm::gfx::Surface;
using mm::gfx::Rgb565;

constexpr Rgb565 background = mm::gfx::rgb(11, 18, 30);
constexpr Rgb565 field = mm::gfx::rgb(26, 39, 55);
constexpr Rgb565 digit_key = mm::gfx::rgb(38, 54, 72);
constexpr Rgb565 operation_key = mm::gfx::rgb(55, 75, 100);
constexpr Rgb565 selected_key = mm::gfx::rgb(25, 120, 133);
constexpr Rgb565 clear_key = mm::gfx::rgb(145, 48, 59);
constexpr Rgb565 equals_key = mm::gfx::rgb(35, 99, 185);
constexpr Rgb565 white = mm::gfx::rgb565_white;

struct Key {
    char action;
    const char* label;
    unsigned int row;
    unsigned int column;
    unsigned int columns = 1;
};

constexpr std::array<Key, 19> keys{{
    {'C', "AC", 0, 0}, {'S', "+/-", 0, 1},
    {'D', "DEL", 0, 2}, {'/', "/", 0, 3},
    {'7', "7", 1, 0}, {'8', "8", 1, 1},
    {'9', "9", 1, 2}, {'*', "*", 1, 3},
    {'4', "4", 2, 0}, {'5', "5", 2, 1},
    {'6', "6", 2, 2}, {'-', "-", 2, 3},
    {'1', "1", 3, 0}, {'2', "2", 3, 1},
    {'3', "3", 3, 2}, {'+', "+", 3, 3},
    {'0', "0", 4, 0}, {'.', ".", 4, 1},
    {'=', "=", 4, 2, 2},
}};

struct Layout {
    unsigned int width;
    unsigned int height;
    static constexpr unsigned int margin = 8;
    static constexpr unsigned int gap = 6;
    static constexpr unsigned int top = 90;
    static constexpr unsigned int bottom = 8;

    [[nodiscard]] unsigned int key_width() const {
        return (width - 2u * margin - 3u * gap) / 4u;
    }
    [[nodiscard]] unsigned int key_height() const {
        return (height - top - bottom - 4u * gap) / 5u;
    }
    [[nodiscard]] mm::display::Rectangle box(const Key& key) const {
        const auto unit_width = key_width();
        return {margin + key.column * (unit_width + gap),
                top + key.row * (key_height() + gap),
                key.columns * unit_width + (key.columns - 1u) * gap,
                key_height()};
    }
};

[[nodiscard]] bool text(Surface frame, const char* label,
                        unsigned int x, unsigned int y, Rgb565 ink) {
    std::size_t count = 0;
    char8_t characters[text_limit]{};
    while (label[count] != 0) {
        if (count == text_limit) return false;
        characters[count] = static_cast<char8_t>(label[count]);
        ++count;
    }
    if (count == 0) return true;
    const auto width = static_cast<unsigned int>(count) * mm::fonts::kMono16.advance;
    const auto row_bytes = (width + 7u) / 8u;
    const auto bytes = row_bytes * mm::fonts::kMono16.height;
    auto bitmap = Surface{width, mm::fonts::kMono16.height, 1,
                          std::span<std::byte>{text_bits}.first(bytes)};
    if (mm::gfx::fill(bitmap, mm::display::Color::Black) != Status::Ok ||
        mm::fonts::render(characters, count, mm::fonts::kMono16,
                          mm::display::Color::White, 0, 0, bitmap) != Status::Ok)
        return false;
    for (unsigned int row = 0; row < bitmap.height; ++row)
        for (unsigned int column = 0; column < bitmap.width; ++column) {
            const auto packed = bitmap.pixels[row * row_bytes + column / 8u];
            if ((packed & static_cast<std::byte>(0x80u >> (column % 8u))) !=
                    std::byte{0} &&
                mm::gfx::pixel(frame, static_cast<int>(x + column),
                               static_cast<int>(y + row), ink) != Status::Ok)
                return false;
        }
    return true;
}

[[nodiscard]] Rgb565 key_color(char action) {
    if (action == 'C') return clear_key;
    if (action == '=') return equals_key;
    if (action == '+' || action == '-' || action == '*' || action == '/') {
        return action == kalkulator::operation() ? selected_key : operation_key;
    }
    return digit_key;
}

[[nodiscard]] bool draw(mm::display::Display& display, const Layout& layout) {
    const auto byte_count = static_cast<std::size_t>(layout.width) *
                            layout.height * 2u;
    const Surface frame{layout.width, layout.height, 16,
                        std::span<std::byte>{pixels}.first(byte_count)};
    if (mm::gfx::fill(frame, background) != Status::Ok ||
        mm::gfx::fill_rectangle(frame, 8, 34, layout.width - 16u, 48,
                                field) != Status::Ok ||
        !text(frame, "KALKULATOR", 10, 7, white))
        return false;

    const auto* result = kalkulator::display();
    std::size_t length = 0;
    while (result[length] != 0) ++length;
    const auto result_width = static_cast<unsigned int>(length) *
                              mm::fonts::kMono16.advance;
    if (result_width > layout.width - 32u ||
        !text(frame, result, layout.width - 16u - result_width, 47, white))
        return false;
    const char selected[] = {kalkulator::operation(), 0};
    if (selected[0] != 0 && !text(frame, selected, 18, 47, white))
        return false;

    for (const auto& key : keys) {
        const auto box = layout.box(key);
        if (mm::gfx::fill_rectangle(frame, static_cast<int>(box.x),
                                    static_cast<int>(box.y), box.width,
                                    box.height, key_color(key.action)) != Status::Ok)
            return false;
        std::size_t label_length = 0;
        while (key.label[label_length] != 0) ++label_length;
        const auto label_width = static_cast<unsigned int>(label_length) *
                                 mm::fonts::kMono16.advance;
        const auto label_x = box.x + (box.width - label_width) / 2u;
        const auto label_y = box.y +
            (box.height - mm::fonts::kMono16.height) / 2u;
        if (!text(frame, key.label, label_x, label_y, white)) return false;
    }
    return mm::gfx::write(display, frame, 0, 0) == Status::Ok &&
           display.refresh(mm::display::Refresh::Full) == Status::Ok;
}

[[nodiscard]] char hit(const Layout& layout, unsigned int x, unsigned int y) {
    for (const auto& key : keys) {
        const auto box = layout.box(key);
        if (x >= box.x && y >= box.y &&
            x - box.x < box.width && y - box.y < box.height)
            return key.action;
    }
    return 0;
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
        panel.width > maximum_width || panel.height < 280 ||
        panel.height > maximum_height || sensor.width == 0 ||
        sensor.height == 0)
        return 3;

    const Layout layout{panel.width, panel.height};
    kalkulator::clear();
    if (!draw(display, layout)) return 4;

    bool was_down = false;
    std::array<mm::touch::Point, 1> points{};
    for (;;) {
        std::size_t count = 0;
        if (touch.read(points, count) != mm::touch::Status::Ok) return 5;
        const bool down = count != 0;
        if (down && !was_down) {
            const auto x = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].x) * panel.width /
                sensor.width);
            const auto y = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].y) * panel.height /
                sensor.height);
            const char key = hit(layout, x, y);
            if (key != 0) {
                kalkulator::press(key);
                if (!draw(display, layout)) return 6;
            }
        }
        was_down = down;
        if (mm::mcu::delay_ms(poll_ms) != mm::mcu::Status::Ok) return 7;
    }
}
