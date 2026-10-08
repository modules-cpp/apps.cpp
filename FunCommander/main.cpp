#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "buzzer.hpp"
#include "chip8.hpp"
#include "launcher.hpp"

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
constexpr unsigned int text_buffer_size = 2048;

std::array<std::byte, maximum_width * maximum_height * 2u> frame_bytes{};
std::array<std::byte, text_buffer_size> text_bytes{};
std::array<std::uint8_t, chip8::max_rom_size> rom_buffer{};

// Retro Color Palette
constexpr Rgb565 col_black = mm::gfx::rgb565_black;
constexpr Rgb565 col_white = mm::gfx::rgb565_white;
constexpr Rgb565 col_bg_dark = mm::gfx::rgb(14, 18, 26);
constexpr Rgb565 col_header_bg = mm::gfx::rgb(22, 38, 58);
constexpr Rgb565 col_card_bg = mm::gfx::rgb(22, 30, 44);
constexpr Rgb565 col_card_border = mm::gfx::rgb(36, 50, 72);
constexpr Rgb565 col_card_sel = mm::gfx::rgb(32, 84, 120);
constexpr Rgb565 col_btn_bg = mm::gfx::rgb(34, 52, 76);
constexpr Rgb565 col_btn_active = mm::gfx::rgb(42, 130, 168);
constexpr Rgb565 col_accent = mm::gfx::rgb(50, 180, 140);
constexpr Rgb565 col_accent_bg = mm::gfx::rgb(18, 70, 54);
constexpr Rgb565 col_text_dim = mm::gfx::rgb(130, 150, 175);
constexpr Rgb565 col_text_bright = mm::gfx::rgb(240, 245, 255);
constexpr Rgb565 col_gold = mm::gfx::rgb(240, 190, 50);
constexpr Rgb565 col_green = mm::gfx::rgb(60, 200, 100);
constexpr Rgb565 col_red = mm::gfx::rgb(220, 60, 60);

// CRT Virtual Screen Colors
constexpr Rgb565 col_crt_bg = mm::gfx::rgb(16, 26, 20);
constexpr Rgb565 col_crt_ink = mm::gfx::rgb(80, 246, 140);
constexpr Rgb565 col_crt_border = mm::gfx::rgb(35, 55, 42);

// Keypad mapping: standard CHIP-8 4x4 layout
constexpr std::array<std::uint8_t, 16> keypad_keys{{
    0x1, 0x2, 0x3, 0xC,
    0x4, 0x5, 0x6, 0xD,
    0x7, 0x8, 0x9, 0xE,
    0xA, 0x0, 0xB, 0xF,
}};
constexpr char hex_chars[] = "0123456789ABCDEF";

enum class AppMode {
    Launcher,
    Playing
};

struct Layout {
    unsigned int width = 240;
    unsigned int height = 320;

    // Emulator scaling
    unsigned int chip8_scale = 3;
    unsigned int chip8_x = 24;
    unsigned int chip8_y = 28;
    unsigned int keypad_top = 142;
    unsigned int keypad_margin = 6;
    unsigned int keypad_gap = 4;

    // Launcher layout
    unsigned int list_top = 42;
    unsigned int list_row_h = 27;
    unsigned int toolbar_y = 244;
    unsigned int launch_btn_y = 276;

    [[nodiscard]] unsigned int key_width() const {
        return (width - 2u * keypad_margin - 3u * keypad_gap) / 4u;
    }

    [[nodiscard]] unsigned int key_height() const {
        const unsigned int avail_h = height > keypad_top ? (height - keypad_top - keypad_margin) : 100u;
        return (avail_h - 3u * keypad_gap) / 4u;
    }

    [[nodiscard]] mm::display::Rectangle key_box(unsigned int index) const {
        const auto col = index % 4u;
        const auto row = index / 4u;
        return {
            keypad_margin + col * (key_width() + keypad_gap),
            keypad_top + row * (key_height() + keypad_gap),
            key_width(),
            key_height()
        };
    }
};

struct State {
    AppMode mode = AppMode::Launcher;
    int selected_row = 0;
    std::size_t active_rom_size = 0;
    std::array<char, mm::fs::max_name + 1> active_game_name{ "DEMO" };
    std::array<char, 48> status_message{ "Welcome to FunCommander" };
    bool needs_redraw = true;
};

void clear_all_keys() {
    for (unsigned int k = 0; k < 16; ++k) {
        chip8::key(k, false);
    }
}

bool render_label(Surface target, std::string_view value, unsigned int x, unsigned int y,
                  Rgb565 color, const mm::fonts::Font& font = mm::fonts::kMono12) {
    if (value.empty() || x >= target.width || y >= target.height) return true;
    const unsigned int max_chars = (target.width - x) / font.advance;
    const unsigned int count = value.size() < max_chars ? static_cast<unsigned int>(value.size()) : max_chars;
    if (count == 0) return true;

    std::array<char8_t, 64> characters{};
    const unsigned int len = count < characters.size() ? count : static_cast<unsigned int>(characters.size());
    for (unsigned int i = 0; i < len; ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        characters[i] = static_cast<char8_t>(ch >= 32 && ch < 127 ? ch : '?');
    }

    const unsigned int width = len * font.advance;
    const unsigned int row_bytes = (width + 7u) / 8u;
    const unsigned int bytes = row_bytes * font.height;
    if (bytes > text_bytes.size()) return false;

    const Surface glyphs{width, font.height, 1, std::span<std::byte>{text_bytes}.first(bytes)};
    if (mm::gfx::fill(glyphs, mm::display::Color::Black) != Status::Ok ||
        mm::fonts::render(characters.data(), len, font, mm::display::Color::White, 0, 0, glyphs) != Status::Ok)
        return false;

    for (unsigned int row = 0; row < glyphs.height; ++row) {
        if (y + row >= target.height) break;
        for (unsigned int col = 0; col < glyphs.width; ++col) {
            if (x + col >= target.width) break;
            const auto bits = glyphs.pixels[row * row_bytes + col / 8u];
            if ((bits & static_cast<std::byte>(0x80u >> (col % 8u))) != std::byte{0}) {
                (void)mm::gfx::pixel(target, static_cast<int>(x + col), static_cast<int>(y + row), color);
            }
        }
    }
    return true;
}

void set_status(State& state, std::string_view msg) {
    const std::size_t len = msg.size() < state.status_message.size() - 1 ? msg.size() : state.status_message.size() - 1;
    std::memcpy(state.status_message.data(), msg.data(), len);
    state.status_message[len] = '\0';
    state.needs_redraw = true;
}

void format_size(std::uint64_t bytes, char* out, std::size_t out_size) {
    if (bytes < 1024) {
        const unsigned int b = static_cast<unsigned int>(bytes);
        if (b < 10) {
            out[0] = static_cast<char>('0' + b);
            out[1] = 'B';
            out[2] = '\0';
        } else if (b < 100) {
            out[0] = static_cast<char>('0' + (b / 10));
            out[1] = static_cast<char>('0' + (b % 10));
            out[2] = 'B';
            out[3] = '\0';
        } else if (b < 1000) {
            out[0] = static_cast<char>('0' + (b / 100));
            out[1] = static_cast<char>('0' + ((b / 10) % 10));
            out[2] = static_cast<char>('0' + (b % 10));
            out[3] = 'B';
            out[4] = '\0';
        } else {
            out[0] = static_cast<char>('0' + (b / 1000));
            out[1] = static_cast<char>('0' + ((b / 100) % 10));
            out[2] = static_cast<char>('0' + ((b / 10) % 10));
            out[3] = static_cast<char>('0' + (b % 10));
            out[4] = 'B';
            out[5] = '\0';
        }
    } else {
        const unsigned int kb = static_cast<unsigned int>(bytes / 1024);
        out[0] = static_cast<char>('0' + (kb % 10));
        out[1] = 'K';
        out[2] = '\0';
    }
}

bool render_launcher(mm::display::Display& display, const Layout& layout, const State& state) {
    const auto bytes = static_cast<std::size_t>(layout.width) * layout.height * 2u;
    const Surface target{layout.width, layout.height, 16, std::span<std::byte>{frame_bytes}.first(bytes)};

    if (mm::gfx::fill(target, col_bg_dark) != Status::Ok) return false;

    // Header bar
    (void)mm::gfx::fill_rectangle(target, 0, 0, layout.width, 40, col_header_bg);
    (void)render_label(target, "FUN COMMANDER", 6, 4, col_text_bright, mm::fonts::kMono16);

    // Current Path & storage badge
    (void)render_label(target, funcommander::current_directory(), 6, 24, col_text_dim, mm::fonts::kMono12);

    const bool is_lfs = (funcommander::current_source() == funcommander::StorageSource::LittleFs);
    const char* badge_text = is_lfs ? "[LFS: OK]" : (funcommander::is_sd_mounted() ? "[SD: OK]" : "[NO SD]");
    const Rgb565 badge_color = (is_lfs || funcommander::is_sd_mounted()) ? col_green : col_red;

    const unsigned int badge_x = layout.width > 90 ? layout.width - 85 : layout.width - 60;
    (void)render_label(target, badge_text, badge_x, 4, badge_color, mm::fonts::kMono12);

    // List of entries
    for (unsigned int i = 0; i < funcommander::page_size; ++i) {
        const auto* entry = funcommander::get_page_entry(i);
        const unsigned int y = layout.list_top + i * (layout.list_row_h + 1u);
        const unsigned int w = layout.width - 8u;
        const bool is_selected = (state.selected_row == static_cast<int>(i));

        if (entry != nullptr) {
            // Row background box
            (void)mm::gfx::fill_rectangle(target, 4, static_cast<int>(y), w, layout.list_row_h,
                                         is_selected ? col_card_sel : col_card_bg);
            (void)mm::gfx::rectangle(target, 4, static_cast<int>(y), w, layout.list_row_h,
                                     is_selected ? col_accent : col_card_border);

            // Icon prefix
            const char* tag = "[F]";
            Rgb565 tag_col = col_text_dim;
            if (entry->kind == funcommander::EntryKind::DemoGame) {
                tag = "[*]";
                tag_col = col_gold;
            } else if (entry->kind == funcommander::EntryKind::Parent) {
                tag = "[..]";
                tag_col = col_text_bright;
            } else if (entry->kind == funcommander::EntryKind::Directory) {
                tag = "[D]";
                tag_col = col_gold;
            } else if (entry->kind == funcommander::EntryKind::Chip8Game) {
                tag = "[C8]";
                tag_col = col_green;
            }

            (void)render_label(target, tag, 8, y + 5, tag_col, mm::fonts::kMono12);
            (void)render_label(target, entry->name.data(), 42, y + 5,
                               is_selected ? col_white : col_text_bright, mm::fonts::kMono12);

            if (entry->kind == funcommander::EntryKind::Chip8Game ||
                entry->kind == funcommander::EntryKind::DemoGame ||
                entry->kind == funcommander::EntryKind::OtherFile) {
                char size_str[16]{};
                format_size(entry->size, size_str, sizeof(size_str));
                const unsigned int size_x = layout.width > 50 ? layout.width - 48 : layout.width - 30;
                (void)render_label(target, size_str, size_x, y + 5, col_text_dim, mm::fonts::kMono12);
            }
        } else {
            // Empty slot
            (void)mm::gfx::fill_rectangle(target, 4, static_cast<int>(y), w, layout.list_row_h, col_bg_dark);
        }
    }

    // Toolbar buttons (PREV, NEXT, UP, REMOUNT)
    const unsigned int btn_w = (layout.width - 8u - 3u * 4u) / 4u;
    const unsigned int y_tool = layout.toolbar_y;
    auto draw_btn = [&](unsigned int col, const char* label_str, Rgb565 text_col = col_text_bright) {
        const unsigned int bx = 4u + col * (btn_w + 4u);
        (void)mm::gfx::fill_rectangle(target, static_cast<int>(bx), static_cast<int>(y_tool),
                                     btn_w, 28, col_btn_bg);
        (void)mm::gfx::rectangle(target, static_cast<int>(bx), static_cast<int>(y_tool),
                                 btn_w, 28, col_card_border);
        const auto len = static_cast<unsigned int>(std::strlen(label_str));
        const unsigned int tx = bx + (btn_w > len * 7u ? (btn_w - len * 7u) / 2u : 2u);
        (void)render_label(target, label_str, tx, y_tool + 6, text_col, mm::fonts::kMono12);
    };

    draw_btn(0, "< PREV");
    draw_btn(1, "NEXT >");
    draw_btn(2, "[UP]");
    if (funcommander::current_source() == funcommander::StorageSource::LittleFs) {
        draw_btn(3, "[TO SD]", col_gold);
    } else {
        draw_btn(3, "[TO LFS]", col_accent);
    }

    // Big Action Button: PLAY / LAUNCH GAME
    const unsigned int y_launch = layout.launch_btn_y;
    (void)mm::gfx::fill_rectangle(target, 4, static_cast<int>(y_launch), layout.width - 8u, 26, col_accent_bg);
    (void)mm::gfx::rectangle(target, 4, static_cast<int>(y_launch), layout.width - 8u, 26, col_accent);
    const char* launch_title = "> LAUNCH SELECTED GAME <";
    const unsigned int title_len = static_cast<unsigned int>(std::strlen(launch_title));
    const unsigned int l_tx = 4u + (layout.width - 8u > title_len * 7u ? (layout.width - 8u - title_len * 7u) / 2u : 4u);
    (void)render_label(target, launch_title, l_tx, y_launch + 5, col_text_bright, mm::fonts::kMono12);

    // Status message at bottom
    (void)render_label(target, state.status_message.data(), 6, layout.height - 15, col_text_dim, mm::fonts::kMono12);

    if (mm::gfx::write(display, target, 0, 0) != Status::Ok ||
        display.refresh(mm::display::Refresh::Full) != Status::Ok)
        return false;

    return true;
}

bool render_game(mm::display::Display& display, const Layout& layout, const State& state, int held_key) {
    const auto bytes = static_cast<std::size_t>(layout.width) * layout.height * 2u;
    const Surface target{layout.width, layout.height, 16, std::span<std::byte>{frame_bytes}.first(bytes)};

    if (mm::gfx::fill(target, col_bg_dark) != Status::Ok) return false;

    // Header bar (Game Title, BEEP, RST, EXIT)
    (void)mm::gfx::fill_rectangle(target, 0, 0, layout.width, 26, col_header_bg);
    (void)render_label(target, state.active_game_name.data(), 6, 5, col_text_bright, mm::fonts::kMono12);

    // Sound indicator
    if (chip8::sound_timer() > 0) {
        (void)render_label(target, "[*BEEP*]", 110, 5, col_gold, mm::fonts::kMono12);
    }

    // Reset button
    (void)mm::gfx::fill_rectangle(target, static_cast<int>(layout.width - 82), 2, 36, 22, col_btn_bg);
    (void)mm::gfx::rectangle(target, static_cast<int>(layout.width - 82), 2, 36, 22, col_card_border);
    (void)render_label(target, "RST", layout.width - 74, 5, col_text_bright, mm::fonts::kMono12);

    // Exit button
    (void)mm::gfx::fill_rectangle(target, static_cast<int>(layout.width - 42), 2, 38, 22, col_btn_bg);
    (void)mm::gfx::rectangle(target, static_cast<int>(layout.width - 42), 2, 38, 22, col_card_border);
    (void)render_label(target, "EXIT", layout.width - 38, 5, col_red, mm::fonts::kMono12);

    // CHIP-8 Screen frame & pixels
    const unsigned int screen_w = 64u * layout.chip8_scale;
    const unsigned int screen_h = 32u * layout.chip8_scale;

    (void)mm::gfx::rectangle(target, static_cast<int>(layout.chip8_x - 1),
                             static_cast<int>(layout.chip8_y - 1),
                             screen_w + 2u, screen_h + 2u, col_crt_border);
    (void)mm::gfx::fill_rectangle(target, static_cast<int>(layout.chip8_x),
                                 static_cast<int>(layout.chip8_y),
                                 screen_w, screen_h, col_crt_bg);

    for (unsigned int y = 0; y < 32; ++y) {
        for (unsigned int x = 0; x < 64; ++x) {
            if (chip8::pixel(x, y)) {
                (void)mm::gfx::fill_rectangle(
                    target,
                    static_cast<int>(layout.chip8_x + x * layout.chip8_scale),
                    static_cast<int>(layout.chip8_y + y * layout.chip8_scale),
                    layout.chip8_scale, layout.chip8_scale, col_crt_ink);
            }
        }
    }

    // Helper text
    (void)render_label(target, "HOLD KEYS TO PLAY", layout.chip8_x + 30, layout.chip8_y + screen_h + 3,
                       col_text_dim, mm::fonts::kMono12);

    // Keypad (4x4 buttons)
    for (unsigned int i = 0; i < keypad_keys.size(); ++i) {
        const auto box = layout.key_box(i);
        const bool pressed = (held_key == static_cast<int>(keypad_keys[i]));
        (void)mm::gfx::fill_rectangle(target, static_cast<int>(box.x), static_cast<int>(box.y),
                                     box.width, box.height, pressed ? col_btn_active : col_btn_bg);
        (void)mm::gfx::rectangle(target, static_cast<int>(box.x), static_cast<int>(box.y),
                                 box.width, box.height, pressed ? col_accent : col_card_border);

        const char char_str[] = { hex_chars[keypad_keys[i]], '\0' };
        const auto tx = box.x + (box.width - mm::fonts::kMono16.advance) / 2u;
        const auto ty = box.y + (box.height - mm::fonts::kMono16.height) / 2u;
        (void)render_label(target, char_str, tx, ty, col_white, mm::fonts::kMono16);
    }

    if (mm::gfx::write(display, target, 0, 0) != Status::Ok ||
        display.refresh(mm::display::Refresh::Full) != Status::Ok)
        return false;

    chip8::acknowledge_frame();
    return true;
}

int get_touched_key(const Layout& layout, unsigned int x, unsigned int y) {
    for (unsigned int i = 0; i < keypad_keys.size(); ++i) {
        const auto box = layout.key_box(i);
        if (x >= box.x && y >= box.y &&
            x - box.x < box.width && y - box.y < box.height) {
            return keypad_keys[i];
        }
    }
    return -1;
}

struct Devices {
    mm::display::Display& display;
    mm::touch::Touch& touch;
    funcommander::Buzzer buzzer{};
    bool display_ready = false;
    bool touch_ready = false;

    ~Devices() {
        buzzer.shutdown();
        funcommander::shutdown_storage();
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
    (void)devices.buzzer.initialize();

    const auto panel = display.geometry();
    const auto sensor = touch.geometry();
    if (panel.bits_per_pixel != 16 || panel.width < 220 ||
        panel.width > maximum_width || panel.height < 280 ||
        panel.height > maximum_height || sensor.width == 0 ||
        sensor.height == 0)
        return 3;

    // Calculate layout
    Layout layout;
    layout.width = panel.width;
    layout.height = panel.height;

    // CHIP-8 screen sizing: 64x32
    const unsigned int scale_w = (panel.width - 16u) / 64u;
    const unsigned int scale_h = 96u / 32u;
    layout.chip8_scale = scale_w < scale_h ? scale_w : scale_h;
    if (layout.chip8_scale < 1) layout.chip8_scale = 1;

    layout.chip8_x = (panel.width - 64u * layout.chip8_scale) / 2u;
    layout.chip8_y = 28u;
    layout.keypad_top = layout.chip8_y + 32u * layout.chip8_scale + 16u;
    layout.keypad_margin = 6u;
    layout.keypad_gap = 4u;

    layout.list_top = 42u;
    layout.list_row_h = 27u;
    layout.toolbar_y = panel.height - 76u;
    layout.launch_btn_y = panel.height - 44u;

    State state;

    // Initialize storage: mounts LittleFS at /games and FAT at /sd if available
    auto fs_status = funcommander::initialize_storage();
    if (fs_status == mm::fs::Status::Ok) {
        set_status(state, "LittleFS /games mounted");
    } else {
        set_status(state, "Storage initialized");
    }

    if (!render_launcher(display, layout, state)) return 4;

    unsigned long last_ms = 0;
    if (mm::mcu::ticks_ms(last_ms) != mm::mcu::Status::Ok) return 5;
    unsigned long cpu_fraction = 0;
    unsigned long timer_fraction = 0;
    int held_key = -1;
    bool was_touched = false;
    std::array<mm::touch::Point, 1> points{};

    for (;;) {
        std::size_t touch_count = 0;
        if (touch.read(points, touch_count) != mm::touch::Status::Ok) return 6;

        const bool is_touched = (touch_count > 0);
        unsigned int tx = 0;
        unsigned int ty = 0;
        if (is_touched) {
            tx = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].x) * panel.width / sensor.width);
            ty = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].y) * panel.height / sensor.height);
        }

        if (state.mode == AppMode::Launcher) {
            // Touch event in Launcher (only process on initial touch down)
            if (is_touched && !was_touched) {
                // 1. Check list items
                if (ty >= layout.list_top && ty < layout.list_top + funcommander::page_size * (layout.list_row_h + 1u)) {
                    const unsigned int row = (ty - layout.list_top) / (layout.list_row_h + 1u);
                    const auto* entry = funcommander::get_page_entry(row);
                    if (entry != nullptr) {
                        if (state.selected_row == static_cast<int>(row)) {
                            // Double tapped selected row: activate!
                            if (entry->kind == funcommander::EntryKind::Directory) {
                                (void)funcommander::navigate_to(funcommander::find_entry_index(row));
                                state.selected_row = 0;
                                set_status(state, funcommander::current_directory());
                            } else if (entry->kind == funcommander::EntryKind::Parent) {
                                (void)funcommander::navigate_up();
                                state.selected_row = 0;
                                set_status(state, funcommander::current_directory());
                            } else {
                                // Launch game!
                                std::size_t rom_size = 0;
                                auto load_res = funcommander::load_game_rom(*entry, rom_buffer, rom_size);
                                if (load_res == mm::fs::Status::Ok && chip8::load(std::span{rom_buffer.data(), rom_size})) {
                                    state.mode = AppMode::Playing;
                                    state.active_rom_size = rom_size;
                                    const std::size_t n_len = std::strlen(entry->name.data());
                                    const std::size_t c_len = n_len < state.active_game_name.size() - 1 ? n_len : state.active_game_name.size() - 1;
                                    std::memcpy(state.active_game_name.data(), entry->name.data(), c_len);
                                    state.active_game_name[c_len] = '\0';
                                    state.needs_redraw = true;
                                    clear_all_keys();
                                    held_key = -1;
                                } else {
                                    set_status(state, "Failed to load ROM");
                                }
                            }
                        } else {
                            state.selected_row = static_cast<int>(row);
                            state.needs_redraw = true;
                        }
                    }
                }
                // 2. Toolbar buttons: PREV, NEXT, UP, REMOUNT
                else if (ty >= layout.toolbar_y && ty < layout.toolbar_y + 28u) {
                    const unsigned int btn_w = (layout.width - 8u - 3u * 4u) / 4u;
                    if (tx >= 4u && tx < 4u + btn_w) {
                        if (funcommander::prev_page()) {
                            state.selected_row = 0;
                            state.needs_redraw = true;
                        }
                    } else if (tx >= 4u + btn_w + 4u && tx < 4u + 2u * (btn_w + 4u)) {
                        if (funcommander::next_page()) {
                            state.selected_row = 0;
                            state.needs_redraw = true;
                        }
                    } else if (tx >= 4u + 2u * (btn_w + 4u) && tx < 4u + 3u * (btn_w + 4u)) {
                        (void)funcommander::navigate_up();
                        state.selected_row = 0;
                        set_status(state, funcommander::current_directory());
                    } else if (tx >= 4u + 3u * (btn_w + 4u)) {
                        auto sw_res = funcommander::switch_source();
                        state.selected_row = 0;
                        if (sw_res == mm::fs::Status::Ok) {
                            set_status(state, funcommander::storage_source_name());
                        } else {
                            set_status(state, "Failed to switch storage");
                        }
                    }
                }
                // 3. Launch button
                else if (ty >= layout.launch_btn_y && ty < layout.launch_btn_y + 26u) {
                    const auto* entry = funcommander::get_page_entry(state.selected_row);
                    if (entry != nullptr) {
                        if (entry->kind == funcommander::EntryKind::Directory) {
                            (void)funcommander::navigate_to(funcommander::find_entry_index(state.selected_row));
                            state.selected_row = 0;
                            set_status(state, funcommander::current_directory());
                        } else if (entry->kind == funcommander::EntryKind::Parent) {
                            (void)funcommander::navigate_up();
                            state.selected_row = 0;
                            set_status(state, funcommander::current_directory());
                        } else {
                            std::size_t rom_size = 0;
                            auto load_res = funcommander::load_game_rom(*entry, rom_buffer, rom_size);
                            if (load_res == mm::fs::Status::Ok && chip8::load(std::span{rom_buffer.data(), rom_size})) {
                                state.mode = AppMode::Playing;
                                state.active_rom_size = rom_size;
                                const std::size_t n_len = std::strlen(entry->name.data());
                                const std::size_t c_len = n_len < state.active_game_name.size() - 1 ? n_len : state.active_game_name.size() - 1;
                                std::memcpy(state.active_game_name.data(), entry->name.data(), c_len);
                                state.active_game_name[c_len] = '\0';
                                state.needs_redraw = true;
                                clear_all_keys();
                                held_key = -1;
                            } else {
                                set_status(state, "Failed to load ROM");
                            }
                        }
                    }
                }
            }

            was_touched = is_touched;

            if (state.needs_redraw) {
                state.needs_redraw = false;
                if (!render_launcher(display, layout, state)) return 7;
            }

            if (mm::mcu::delay_ms(20) != mm::mcu::Status::Ok) return 8;

        } else {
            // Playing mode
            int next_key = -1;
            if (is_touched) {
                // Check Top Bar: Reset and Exit buttons
                if (ty < 26u) {
                    if (tx >= layout.width - 42u) {
                        // EXIT button pressed: Return to Launcher
                        state.mode = AppMode::Launcher;
                        devices.buzzer.update(false);
                        clear_all_keys();
                        held_key = -1;
                        state.needs_redraw = true;
                        was_touched = is_touched;
                        continue;
                    }
                    if (tx >= layout.width - 82u && tx < layout.width - 42u) {
                        // RST button pressed: Restart current game
                        if (state.active_rom_size > 0) {
                            (void)chip8::load(std::span<const std::uint8_t>{rom_buffer.data(), state.active_rom_size});
                        }
                        clear_all_keys();
                        held_key = -1;
                        state.needs_redraw = true;
                        was_touched = is_touched;
                        continue;
                    }
                } else if (ty >= layout.keypad_top) {
                    next_key = get_touched_key(layout, tx, ty);
                }
            }

            const bool key_changed = (next_key != held_key);
            if (key_changed) {
                if (held_key >= 0) chip8::key(static_cast<unsigned int>(held_key), false);
                if (next_key >= 0) chip8::key(static_cast<unsigned int>(next_key), true);
                held_key = next_key;
            }

            was_touched = is_touched;

            // CPU step and timer ticks
            unsigned long now_ms = 0;
            if (mm::mcu::ticks_ms(now_ms) != mm::mcu::Status::Ok) return 9;
            unsigned long elapsed = now_ms - last_ms;
            last_ms = now_ms;
            if (elapsed > 100u) elapsed = 100u;
            cpu_fraction += elapsed * 700u;
            timer_fraction += elapsed * 60u;

            while (cpu_fraction >= 1000u) {
                cpu_fraction -= 1000u;
                (void)chip8::step();
            }
            while (timer_fraction >= 1000u) {
                timer_fraction -= 1000u;
                chip8::tick();
            }

            devices.buzzer.update(chip8::sound_timer() > 0);

            if (chip8::dirty() || key_changed || state.needs_redraw) {
                state.needs_redraw = false;
                if (!render_game(display, layout, state, held_key)) return 10;
            }

            if (mm::mcu::delay_ms(3) != mm::mcu::Status::Ok) return 11;
        }
    }
}
