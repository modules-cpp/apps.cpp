#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

import mm.audio;
import mm.display;
import mm.fonts;
import mm.gfx;
import mm.mcu;
import mm.touch;

namespace {

using mm::display::Status;
using mm::gfx::Rgb565;
using mm::gfx::Surface;

constexpr unsigned int maximum_width = 480;
constexpr unsigned int maximum_height = 320;
constexpr unsigned int first_octave = 3;
constexpr unsigned int last_octave = 6;
constexpr unsigned int requested_rate = 32'000;
constexpr std::array<unsigned int, 12> c3_note_millihertz{{
    130'813, 138'591, 146'832, 155'563, 164'814, 174'614,
    184'997, 195'998, 207'652, 220'000, 233'082, 246'942
}};
constexpr std::array<unsigned int, 7> white_notes{{0, 2, 4, 5, 7, 9, 11}};
constexpr std::array<unsigned int, 5> black_notes{{1, 3, 6, 8, 10}};
constexpr std::array<unsigned int, 5> black_after_white{{0, 1, 3, 4, 5}};
constexpr std::array<const char*, 12> note_names{{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
}};

std::array<std::byte, maximum_width * maximum_height * 2u> frame_bytes{};
std::array<std::byte, 256> text_bits{};

constexpr Rgb565 background = mm::gfx::rgb(10, 16, 25);
constexpr Rgb565 panel = mm::gfx::rgb(24, 37, 54);
constexpr Rgb565 accent = mm::gfx::rgb(234, 174, 77);
constexpr Rgb565 quiet = mm::gfx::rgb(147, 170, 185);
constexpr Rgb565 white = mm::gfx::rgb(241, 233, 214);
constexpr Rgb565 black = mm::gfx::rgb(20, 29, 42);
constexpr Rgb565 white_active = mm::gfx::rgb(249, 198, 116);
constexpr Rgb565 black_active = mm::gfx::rgb(155, 95, 42);

class Piano {
public:
    Piano() : out_(mm::audio::selected_out()) {}
    Piano(const Piano&) = delete;
    Piano& operator=(const Piano&) = delete;

    ~Piano() {
        if (started_) (void)out_.stop();
        if (configured_) (void)out_.sleep();
    }

    [[nodiscard]] bool initialize() {
        if (out_.initialize() != mm::audio::Status::Ok) return false;
        mm::audio::Format actual{};
        if (out_.configure({.rate_hz = requested_rate}, actual) != mm::audio::Status::Ok ||
            actual.rate_hz == 0) return false;
        configured_ = true;
        rate_hz_ = actual.rate_hz;
        return ring_.configure(std::span<std::int16_t>{samples_}, actual) ==
               mm::audio::Status::Ok;
    }

    [[nodiscard]] bool note_on(unsigned int semitone, unsigned int octave) {
        if (!configured_ || semitone >= c3_note_millihertz.size() ||
            octave < first_octave || octave > last_octave) return false;
        stop();
        if (ring_.reset() != mm::audio::Status::Ok) return false;
        const auto frequency = c3_note_millihertz[semitone] << (octave - first_octave);
        step_ = (static_cast<std::uint64_t>(frequency) << 32) /
                (static_cast<std::uint64_t>(rate_hz_) * 1000u);
        phase_ = 0;
        envelope_ = 0;
        held_ = true;
        fill();
        if (out_.start(ring_) != mm::audio::Status::Ok) {
            held_ = false;
            return false;
        }
        started_ = true;
        return true;
    }

    void note_off() { held_ = false; }

    [[nodiscard]] bool service() {
        if (!started_) return true;
        fill();
        if (out_.service() != mm::audio::Status::Ok) {
            stop();
            return false;
        }
        if (!held_ && envelope_ == 0) stop();
        return true;
    }

    [[nodiscard]] bool active() const { return started_; }

private:
    static std::int32_t triangle(std::uint32_t phase) {
        const auto x = static_cast<std::uint32_t>(phase >> 16);
        return static_cast<std::int32_t>((x < 32768u ? x : 65535u - x) * 2u) -
               32768;
    }

    void fill() {
        const auto region = ring_.write_region();
        for (auto& sample : region) {
            if (held_) {
                if (envelope_ < 30000) {
                    envelope_ += 512;
                    if (envelope_ > 30000) envelope_ = 30000;
                } else if (envelope_ > 18000) {
                    --envelope_;
                }
            } else if (envelope_ > 0) {
                envelope_ = envelope_ > 16 ? envelope_ - 16 : 0;
            }

            const std::uint32_t p = static_cast<std::uint32_t>(phase_);
            const auto harmonic = 6 * triangle(p) +
                                  2 * triangle(p * 2u) + triangle(p * 3u);
            const std::int64_t scaled = static_cast<std::int64_t>(harmonic) *
                                        envelope_ * 9000;
            sample = static_cast<std::int16_t>(scaled / (9LL * 32768 * 32768));
            phase_ += step_;
        }
        (void)ring_.commit_write(region.size());
    }

    void stop() {
        if (started_) {
            (void)out_.stop();
            started_ = false;
        }
        held_ = false;
    }

    mm::audio::Out& out_;
    mm::audio::Ring ring_{};
    std::array<std::int16_t, 256> samples_{};
    std::uint64_t phase_ = 0;
    std::uint64_t step_ = 0;
    unsigned int rate_hz_ = requested_rate;
    int envelope_ = 0;
    bool configured_ = false;
    bool started_ = false;
    bool held_ = false;
};

struct Layout {
    unsigned int width = 240;
    unsigned int height = 320;

    [[nodiscard]] unsigned int keyboard_top() const { return 123; }
    [[nodiscard]] unsigned int keyboard_bottom() const { return height - 8; }
    [[nodiscard]] unsigned int white_width() const { return (width - 8) / 7; }
    [[nodiscard]] unsigned int black_width() const { return white_width() * 2 / 3; }
    [[nodiscard]] unsigned int black_height() const {
        return (keyboard_bottom() - keyboard_top()) * 3 / 5;
    }
    [[nodiscard]] unsigned int white_x(unsigned int index) const {
        return 4 + index * white_width();
    }
    [[nodiscard]] unsigned int black_x(unsigned int index) const {
        return white_x(black_after_white[index] + 1) - black_width() / 2;
    }
};

[[nodiscard]] bool text(Surface frame, const char* value, unsigned int x,
                        unsigned int y, Rgb565 color,
                        const mm::fonts::Font& font = mm::fonts::kMono12) {
    if (x >= frame.width || y >= frame.height) return true;
    std::array<char8_t, 24> characters{};
    unsigned int count = 0;
    while (value[count] != 0 && count < characters.size()) {
        characters[count] = static_cast<char8_t>(value[count]);
        ++count;
    }
    const auto width = count * font.advance;
    if (width == 0 || width > frame.width - x) return true;
    const auto row_bytes = (width + 7u) / 8u;
    const auto size = row_bytes * font.height;
    if (size > text_bits.size()) return false;
    const Surface mask{width, font.height, 1,
                       std::span<std::byte>{text_bits}.first(size)};
    if (mm::gfx::fill(mask, mm::display::Color::Black) != Status::Ok ||
        mm::fonts::render(characters.data(), count, font,
                          mm::display::Color::White, 0, 0, mask) != Status::Ok)
        return false;
    for (unsigned int row = 0; row < mask.height; ++row) {
        for (unsigned int column = 0; column < mask.width; ++column) {
            const auto bits = mask.pixels[row * row_bytes + column / 8u];
            if ((bits & static_cast<std::byte>(0x80u >> (column % 8u))) != std::byte{0})
                if (mm::gfx::pixel(frame, static_cast<int>(x + column),
                                   static_cast<int>(y + row), color) != Status::Ok)
                    return false;
        }
    }
    return true;
}

[[nodiscard]] bool draw(mm::display::Display& display, const Layout& layout,
                        unsigned int octave, int selected_note) {
    const Surface frame{layout.width, layout.height, 16,
                        std::span<std::byte>{frame_bytes}.first(
                            static_cast<std::size_t>(layout.width) * layout.height * 2u)};
    if (mm::gfx::fill(frame, background) != Status::Ok) return false;
    (void)mm::gfx::fill_rectangle(frame, 4, 4, layout.width - 8, 34, panel);
    if (!text(frame, "AUDIO COMMANDER", 12, 12, accent)) return false;
    (void)mm::gfx::fill_rectangle(frame, 4, 44, layout.width - 8, 32, panel);
    if (!text(frame, "PIANO", 13, 52, white, mm::fonts::kMono16)) return false;
    char note_label[6] = {'-', 0, 0, 0, 0, 0};
    if (selected_note >= 0) {
        const char* name = note_names[static_cast<unsigned int>(selected_note)];
        note_label[0] = name[0];
        note_label[1] = name[1] == '#' ? '#' : static_cast<char>('0' + octave);
        note_label[2] = name[1] == '#' ? static_cast<char>('0' + octave) : 0;
    }
    if (!text(frame, note_label, layout.width - 64, 52, accent,
              mm::fonts::kMono16)) return false;
    (void)mm::gfx::fill_rectangle(frame, 4, 82, 69, 35, panel);
    (void)mm::gfx::fill_rectangle(frame, layout.width - 73, 82, 69, 35, panel);
    char octave_label[] = {'O', 'C', 'T', ' ', static_cast<char>('0' + octave), 0};
    if (!text(frame, "< OCT", 10, 93, white) ||
        !text(frame, octave_label, layout.width / 2 - 20, 93, accent) ||
        !text(frame, "OCT >", layout.width - 67, 93, white)) return false;

    const auto top = layout.keyboard_top();
    const auto height = layout.keyboard_bottom() - top;
    for (unsigned int i = 0; i < white_notes.size(); ++i) {
        const auto x = layout.white_x(i);
        const auto color = selected_note == static_cast<int>(white_notes[i]) ?
                           white_active : white;
        (void)mm::gfx::fill_rectangle(frame, x, top, layout.white_width() - 2,
                                      height, color);
        const char label[] = {note_names[white_notes[i]][0], 0};
        if (!text(frame, label, x + 7, layout.keyboard_bottom() - 21, black))
            return false;
    }
    for (unsigned int i = 0; i < black_notes.size(); ++i) {
        const auto color = selected_note == static_cast<int>(black_notes[i]) ?
                           black_active : black;
        (void)mm::gfx::fill_rectangle(frame, layout.black_x(i), top,
                                      layout.black_width(), layout.black_height(),
                                      color);
    }
    return mm::gfx::write(display, frame, 0, 0) == Status::Ok &&
           display.refresh(mm::display::Refresh::Full) == Status::Ok;
}

[[nodiscard]] int hit_note(const Layout& layout, unsigned int x, unsigned int y) {
    if (x < 4 || x >= layout.width - 4 || y < layout.keyboard_top() ||
        y >= layout.keyboard_bottom()) return -1;
    if (y < layout.keyboard_top() + layout.black_height()) {
        for (unsigned int i = 0; i < black_notes.size(); ++i) {
            const auto bx = layout.black_x(i);
            if (x >= bx && x - bx < layout.black_width())
                return static_cast<int>(black_notes[i]);
        }
    }
    const auto index = (x - 4) / layout.white_width();
    return index < white_notes.size() ? static_cast<int>(white_notes[index]) : -1;
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

} // namespace

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
        panel.height > maximum_height || sensor.width == 0 || sensor.height == 0)
        return 3;

    Piano piano;
    if (!piano.initialize()) return 4;
    const Layout layout{panel.width, panel.height};
    unsigned int octave = 4;
    int selected = -1;
    bool was_touched = false;
    if (!draw(display, layout, octave, selected)) return 5;

    std::array<mm::touch::Point, 1> points{};
    for (;;) {
        std::size_t count = 0;
        if (touch.read(points, count) != mm::touch::Status::Ok) return 6;
        const bool touched = count != 0;
        unsigned int x = 0;
        unsigned int y = 0;
        if (touched) {
            x = static_cast<unsigned int>(static_cast<std::uint64_t>(points[0].x) *
                                          panel.width / sensor.width);
            y = static_cast<unsigned int>(static_cast<std::uint64_t>(points[0].y) *
                                          panel.height / sensor.height);
        }

        bool redraw = false;
        if (touched && !was_touched && y >= 82 && y < 117) {
            if (x < 73 && octave > first_octave) {
                --octave;
                redraw = true;
            } else if (x >= layout.width - 73 && octave < last_octave) {
                ++octave;
                redraw = true;
            }
        }
        const int note = touched ? hit_note(layout, x, y) : -1;
        if (note != selected) {
            if (note >= 0) {
                if (!piano.note_on(static_cast<unsigned int>(note), octave)) return 7;
            } else {
                piano.note_off();
            }
            selected = note;
            redraw = true;
        }
        was_touched = touched;
        if (!piano.service()) return 8;
        if (redraw) {
            if (!draw(display, layout, octave, selected)) return 9;
            if (!piano.service()) return 10;
        }
        if (mm::mcu::delay_ms(piano.active() ? 1 : 12) != mm::mcu::Status::Ok)
            return 11;
    }
}
