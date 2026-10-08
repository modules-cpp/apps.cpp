#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

import mm.display;
import mm.fonts;
import mm.fs;
import mm.gfx;
import mm.mcu;
import mm.touch;

namespace filecommander {
mm::fs::Status initialize(bool format_if_blank);
void shutdown();
bool ready();
const char* path();
unsigned int count();
bool has_more();
const char* name(unsigned int);
bool directory(unsigned int);
std::uint64_t size(unsigned int);
mm::fs::Status refresh(unsigned int, unsigned int);
mm::fs::Status enter(unsigned int);
mm::fs::Status up();
mm::fs::Status create(std::string_view, bool);
mm::fs::Status rename(unsigned int, std::string_view);
mm::fs::Status remove(unsigned int);
mm::fs::Status read(unsigned int, std::span<std::byte>, std::size_t&);
mm::fs::Status append(unsigned int, std::string_view);
}

namespace {
using mm::display::Status;
using mm::gfx::Surface;
constexpr unsigned int max_width = 480;
constexpr unsigned int max_height = 320;
constexpr unsigned int max_rows = 8;
constexpr unsigned int row_height = 21;
constexpr unsigned int poll_ms = 25;
constexpr unsigned int name_limit = 24;
std::array<std::byte, ((max_width + 7u) / 8u) * max_height> frame_bytes{};
std::array<std::byte, max_width * 2u> scratch{};
std::array<std::byte, 512> preview{};

struct Layout {
    unsigned int width, height;
    unsigned int rows() const {
        const auto available = (height - 136u) / row_height;
        return available < max_rows ? available : max_rows;
    }
    unsigned int toolbar_y() const { return height - 68u; }
    unsigned int keyboard_y() const { return height - 168u; }
};

enum class Mode { List, View, Edit, DeleteConfirm, FormatConfirm };
enum class Edit { NewFile, NewDir, Rename, Append };
enum class Theme { Amber, Green };

struct State {
    Mode mode = Mode::List;
    Edit edit = Edit::NewFile;
    Theme theme = Theme::Amber;
    unsigned int page = 0;
    unsigned int selected = 0;
    unsigned int rows = 0;
    std::array<char, name_limit + 1> input{};
    unsigned int input_size = 0;
    const char* message = "";
    std::size_t preview_size = 0;
    mm::fs::Status mount_error = mm::fs::Status::Ok;
};

const char* error_text(mm::fs::Status status) {
    using mm::fs::Status;
    switch (status) {
    case Status::Ok: return "Ready";
    case Status::BadArgument: return "Invalid name or path";
    case Status::Unsupported: return "Storage unsupported";
    case Status::NotFound: return "Not found";
    case Status::Exists: return "Already exists";
    case Status::NotDirectory: return "Not a folder";
    case Status::IsDirectory: return "Folder, not a file";
    case Status::NotEmpty: return "Folder not empty";
    case Status::NoSpace: return "Storage full";
    case Status::ReadOnly: return "Read only";
    case Status::NameTooLong: return "Name too long";
    case Status::TooMany: return "Too many open files";
    case Status::Busy: return "File busy";
    case Status::CrossVolume: return "Other volume";
    case Status::Corrupt: return "No valid file system";
    case Status::Timeout: return "Storage timeout";
    case Status::TransportError: return "Storage error";
    }
    return "Unknown error";
}

bool box(Surface frame, unsigned int x, unsigned int y,
         unsigned int width, unsigned int height, bool dark) {
    return mm::gfx::fill_rectangle(frame, static_cast<int>(x), static_cast<int>(y),
                                   width, height, dark ? mm::display::Color::Black
                                                       : mm::display::Color::White) == Status::Ok;
}

bool label(Surface frame, std::string_view value, unsigned int x,
           unsigned int y, bool inverse = false) {
    if (x >= frame.width || y + mm::fonts::kMono12.height > frame.height) return true;
    const unsigned int possible = (frame.width - x) / mm::fonts::kMono12.advance;
    const unsigned int count = value.size() < possible ?
        static_cast<unsigned int>(value.size()) : possible;
    std::array<char8_t, 48> text{};
    const unsigned int length = count < text.size() ? count : text.size();
    for (unsigned int i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        text[i] = static_cast<char8_t>(ch >= 32 && ch < 127 ? ch : '?');
    }
    return mm::fonts::render(text.data(), length, mm::fonts::kMono12,
                             inverse ? mm::display::Color::White
                                     : mm::display::Color::Black,
                             x, y, frame) == Status::Ok;
}

bool button(Surface frame, const Layout& layout, unsigned int row,
            unsigned int column, std::string_view title, bool active = true) {
    const unsigned int width = layout.width / 4u;
    const unsigned int x = column * width;
    const unsigned int y = layout.toolbar_y() + row * 33u;
    const unsigned int inner = (column == 3 ? layout.width - x : width) - 2u;
    if (!box(frame, x + 1u, y + 1u, inner, 30u, active)) return false;
    const auto text_width = static_cast<unsigned int>(title.size()) *
                            mm::fonts::kMono12.advance;
    const auto text_x = x + (width > text_width ? (width - text_width) / 2u : 2u);
    return label(frame, title, text_x, y + 8u, active);
}

bool render_list(Surface frame, const Layout& layout, const State& state) {
    const std::string_view theme_name = state.theme == Theme::Amber ? "AMBER >" : "GREEN >";
    const unsigned int theme_x = layout.width - 4u -
        static_cast<unsigned int>(theme_name.size()) * mm::fonts::kMono12.advance;
    if (!label(frame, "FILE COMMANDER", 4, 4) ||
        !label(frame, theme_name, theme_x, 4) ||
        !box(frame, 2, 19, layout.width - 4u, 1, true) ||
        !label(frame, filecommander::path(), 4, 24) ||
        !label(frame, state.message, 4, 43)) return false;
    if (!filecommander::ready()) {
        if (!label(frame, "Storage is unavailable", 6, 82) ||
            !label(frame, error_text(state.mount_error), 6, 104)) return false;
        if (state.mount_error == mm::fs::Status::Corrupt) {
            return button(frame, layout, 1, 0, "FORMAT");
        }
        return true;
    }
    if (filecommander::count() == 0 && !label(frame, "(empty folder)", 8, 74))
        return false;
    for (unsigned int i = 0; i < filecommander::count(); ++i) {
        const unsigned int y = 61u + i * row_height;
        const bool selected = state.selected == i;
        if (selected && !box(frame, 2, y - 2, layout.width - 4u,
                             row_height - 1u, true)) return false;
        if (!label(frame, filecommander::directory(i) ? "[D]" : "[F]",
                   5, y, selected) ||
            !label(frame, filecommander::name(i), 36, y, selected)) return false;
    }
    return button(frame, layout, 0, 0, "PREV", state.page != 0) &&
           button(frame, layout, 0, 1, "NEXT", filecommander::has_more()) &&
           button(frame, layout, 0, 2, "UP") &&
           button(frame, layout, 0, 3, "OPEN", filecommander::count() != 0) &&
           button(frame, layout, 1, 0, "NEW") &&
           button(frame, layout, 1, 1, "DIR") &&
           button(frame, layout, 1, 2, "RENAME", filecommander::count() != 0) &&
           button(frame, layout, 1, 3, "DELETE", filecommander::count() != 0);
}

bool render_view(Surface frame, const Layout& layout, const State& state) {
    if (!label(frame, filecommander::name(state.selected), 4, 5) ||
        !label(frame, "First 512 bytes", 4, 27) ||
        !label(frame, state.message, 4, 47)) return false;
    const unsigned int columns = (layout.width - 10u) / mm::fonts::kMono12.advance;
    const unsigned int bottom = layout.toolbar_y();
    unsigned int x = 5, y = 67, column = 0;
    for (std::size_t i = 0; i < state.preview_size && y + 17u < bottom; ++i) {
        const unsigned char ch = static_cast<unsigned char>(preview[i]);
        if (ch == '\r') continue;
        if (ch == '\n' || column == columns) {
            y += 18u;
            x = 5;
            column = 0;
            if (ch == '\n' || y + 17u >= bottom) continue;
        }
        const char printable = ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.';
        if (!label(frame, {&printable, 1}, x, y)) return false;
        x += mm::fonts::kMono12.advance;
        ++column;
    }
    return button(frame, layout, 0, 0, "BACK") &&
           button(frame, layout, 0, 1, "APPEND") &&
           button(frame, layout, 0, 2, "RENAME") &&
           button(frame, layout, 0, 3, "DELETE");
}

constexpr std::array<std::string_view, 4> key_rows{
    "QWERTYUIOP", "ASDFGHJKL.", "ZXCVBNM-_ ", "0123456789"
};

bool render_edit(Surface frame, const Layout& layout, const State& state) {
    const char* title = state.edit == Edit::NewFile ? "NEW FILE" :
                        state.edit == Edit::NewDir ? "NEW FOLDER" :
                        state.edit == Edit::Rename ? "RENAME" : "APPEND TEXT";
    if (!label(frame, title, 5, 5) ||
        !label(frame, filecommander::path(), 5, 28) ||
        !box(frame, 4, 52, layout.width - 8u, 20, true) ||
        !label(frame, {state.input.data(), state.input_size}, 7, 53, true))
        return false;
    const unsigned int key_width = layout.width / 10u;
    for (unsigned int row = 0; row < key_rows.size(); ++row) {
        for (unsigned int column = 0; column < 10; ++column) {
            const unsigned int x = column * key_width;
            const unsigned int y = layout.keyboard_y() + row * 29u;
            if (!box(frame, x + 1u, y + 1u, key_width - 2u, 27u, true) ||
                !label(frame, key_rows[row].substr(column, 1), x + (key_width - 7u) / 2u,
                       y + 6u, true)) return false;
        }
    }
    const unsigned int y = layout.height - 45u;
    constexpr std::array<std::string_view, 4> actions{"CANCEL", "SPACE", "DEL", "OK"};
    for (unsigned int i = 0; i < actions.size(); ++i) {
        const unsigned int x = i * layout.width / 4u;
        const unsigned int width = layout.width / 4u;
        if (!box(frame, x + 1u, y, width - 2u, 34u, true) ||
            !label(frame, actions[i], x + 3u, y + 9u, true)) return false;
    }
    return true;
}

bool render_confirm(Surface frame, const Layout& layout, const State& state) {
    const bool formatting = state.mode == Mode::FormatConfirm;
    if (!label(frame, formatting ? "FORMAT FLASH?" : "DELETE ITEM?", 6, 54) ||
        !label(frame, formatting ? "All files will be lost" :
               filecommander::name(state.selected), 6, 84) ||
        !label(frame, "Tap YES to confirm", 6, 110)) return false;
    return button(frame, layout, 1, 0, "NO") &&
           button(frame, layout, 1, 2, "YES");
}

bool draw(mm::display::Display& display, const Layout& layout, const State& state) {
    const unsigned int row_bytes = (layout.width + 7u) / 8u;
    const Surface frame{layout.width, layout.height, 1,
                        std::span<std::byte>{frame_bytes}.first(row_bytes * layout.height)};
    if (mm::gfx::fill(frame, mm::display::Color::White) != Status::Ok) return false;
    const bool rendered = state.mode == Mode::List ? render_list(frame, layout, state) :
                          state.mode == Mode::View ? render_view(frame, layout, state) :
                          state.mode == Mode::Edit ? render_edit(frame, layout, state) :
                          render_confirm(frame, layout, state);
    if (!rendered) return false;
    const auto geometry = display.geometry();
    const auto foreground = state.theme == Theme::Amber ?
        mm::gfx::rgb(255, 187, 54) : mm::gfx::rgb(83, 255, 107);
    const auto background = state.theme == Theme::Amber ?
        mm::gfx::rgb(8, 5, 2) : mm::gfx::rgb(2, 8, 4);
    const auto written = geometry.bits_per_pixel == 1 ?
        mm::gfx::write(display, frame, 0, 0) :
        mm::gfx::write(display, frame, 0, 0,
                       {foreground, background},
                       {}, std::span<std::byte>{scratch}.first(layout.width * 2u));
    return written == Status::Ok &&
           display.refresh(mm::display::Refresh::Full) == Status::Ok;
}

void reload(State& state) {
    if (!filecommander::ready()) return;
    auto status = filecommander::refresh(state.page * state.rows, state.rows);
    if (status == mm::fs::Status::Ok && filecommander::count() == 0 && state.page != 0) {
        --state.page;
        status = filecommander::refresh(state.page * state.rows, state.rows);
    }
    if (state.selected >= filecommander::count()) state.selected = 0;
    if (status != mm::fs::Status::Ok) state.message = error_text(status);
}

void begin_edit(State& state, Edit edit) {
    state.mode = Mode::Edit;
    state.edit = edit;
    state.input.fill(0);
    state.input_size = 0;
    if (edit == Edit::Rename) {
        const auto* original = filecommander::name(state.selected);
        const std::size_t length = std::strlen(original);
        if (length <= name_limit) {
            std::memcpy(state.input.data(), original, length);
            state.input_size = static_cast<unsigned int>(length);
        }
    }
}

void open_selected(State& state) {
    if (filecommander::count() == 0) return;
    if (filecommander::directory(state.selected)) {
        const auto status = filecommander::enter(state.selected);
        state.message = error_text(status);
        if (status == mm::fs::Status::Ok) {
            state.page = 0;
            state.selected = 0;
            reload(state);
        }
    } else {
        std::size_t count = 0;
        const auto status = filecommander::read(state.selected, preview, count);
        state.message = error_text(status);
        if (status == mm::fs::Status::Ok) {
            state.preview_size = count;
            state.mode = Mode::View;
        }
    }
}

void finish_edit(State& state) {
    if (state.input_size == 0) {
        state.mode = state.edit == Edit::Append ? Mode::View : Mode::List;
        return;
    }
    const std::string_view input{state.input.data(), state.input_size};
    mm::fs::Status status = mm::fs::Status::BadArgument;
    switch (state.edit) {
    case Edit::NewFile: status = filecommander::create(input, false); break;
    case Edit::NewDir: status = filecommander::create(input, true); break;
    case Edit::Rename: status = filecommander::rename(state.selected, input); break;
    case Edit::Append: status = filecommander::append(state.selected, input); break;
    }
    state.message = error_text(status);
    if (status == mm::fs::Status::Ok) {
        state.mode = Mode::List;
        reload(state);
    }
}

void action(State& state, const Layout& layout, unsigned int x, unsigned int y) {
    if (state.mode == Mode::List && y < 20u) {
        state.theme = state.theme == Theme::Amber ? Theme::Green : Theme::Amber;
        return;
    }
    if (state.mode == Mode::Edit) {
        if (y >= layout.height - 45u) {
            const unsigned int column = x * 4u / layout.width;
            if (column == 0) state.mode = state.edit == Edit::Append ? Mode::View : Mode::List;
            else if (column == 1 && state.input_size < name_limit)
                state.input[state.input_size++] = ' ';
            else if (column == 2 && state.input_size != 0) {
                --state.input_size;
                state.input[state.input_size] = 0;
            } else if (column == 3) finish_edit(state);
            return;
        }
        if (y >= layout.keyboard_y() && y < layout.keyboard_y() + 4u * 29u) {
            const unsigned int row = (y - layout.keyboard_y()) / 29u;
            const unsigned int column = x * 10u / layout.width;
            if (column < 10 && state.input_size < name_limit) {
                state.input[state.input_size++] = key_rows[row][column];
                state.input[state.input_size] = 0;
            }
        }
        return;
    }
    if (state.mode == Mode::DeleteConfirm || state.mode == Mode::FormatConfirm) {
        if (y >= layout.toolbar_y() + 33u) {
            const unsigned int column = x * 4u / layout.width;
            if (column == 0) state.mode = Mode::List;
            else if (column == 2) {
                const auto status = state.mode == Mode::FormatConfirm ?
                    filecommander::initialize(true) : filecommander::remove(state.selected);
                state.mount_error = status;
                state.message = error_text(status);
                state.mode = Mode::List;
                if (status == mm::fs::Status::Ok) reload(state);
            }
        }
        return;
    }
    if (state.mode == Mode::View) {
        if (y >= layout.toolbar_y() && y < layout.toolbar_y() + 33u) {
            const unsigned int column = x * 4u / layout.width;
            if (column == 0) state.mode = Mode::List;
            else if (column == 1) begin_edit(state, Edit::Append);
            else if (column == 2) begin_edit(state, Edit::Rename);
            else state.mode = Mode::DeleteConfirm;
        }
        return;
    }
    if (!filecommander::ready()) {
        if (state.mount_error == mm::fs::Status::Corrupt &&
            y >= layout.toolbar_y() + 33u && x < layout.width / 4u)
            state.mode = Mode::FormatConfirm;
        return;
    }
    if (y >= 59u && y < 59u + state.rows * row_height) {
        const unsigned int index = (y - 59u) / row_height;
        if (index < filecommander::count()) state.selected = index;
        return;
    }
    if (y < layout.toolbar_y()) return;
    const unsigned int row = (y - layout.toolbar_y()) / 33u;
    const unsigned int column = x * 4u / layout.width;
    if (row == 0) {
        if (column == 0 && state.page != 0) { --state.page; state.selected = 0; reload(state); }
        else if (column == 1 && filecommander::has_more()) {
            ++state.page; state.selected = 0; reload(state);
        } else if (column == 2) {
            const auto status = filecommander::up();
            state.message = error_text(status);
            if (status == mm::fs::Status::Ok) {
                state.page = 0; state.selected = 0; reload(state);
            }
        } else if (column == 3) open_selected(state);
    } else if (row == 1) {
        if (column == 0) begin_edit(state, Edit::NewFile);
        else if (column == 1) begin_edit(state, Edit::NewDir);
        else if (filecommander::count() != 0) {
            if (column == 2) begin_edit(state, Edit::Rename);
            else state.mode = Mode::DeleteConfirm;
        }
    }
}

struct Devices {
    mm::display::Display& display;
    mm::touch::Touch& touch;
    bool display_ready = false;
    bool touch_ready = false;
    ~Devices() {
        filecommander::shutdown();
        if (touch_ready) (void)touch.sleep();
        if (display_ready) (void)display.sleep();
    }
};
}

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
    if ((panel.bits_per_pixel != 1 && panel.bits_per_pixel != 16) ||
        panel.width < 220 || panel.width > max_width ||
        panel.height < 240 || panel.height > max_height ||
        sensor.width == 0 || sensor.height == 0) return 3;

    const Layout layout{panel.width, panel.height};
    State state{};
    state.rows = layout.rows();
    state.mount_error = filecommander::initialize(false);
    state.message = error_text(state.mount_error);
    if (filecommander::ready()) reload(state);
    if (!draw(display, layout, state)) return 4;

    bool was_down = false;
    std::array<mm::touch::Point, 1> points{};
    for (;;) {
        std::size_t count = 0;
        if (touch.read(points, count) != mm::touch::Status::Ok) return 5;
        const bool down = count != 0;
        if (down && !was_down) {
            const unsigned int x = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].x) * panel.width / sensor.width);
            const unsigned int y = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].y) * panel.height / sensor.height);
            if (x < layout.width && y < layout.height) {
                action(state, layout, x, y);
                if (!draw(display, layout, state)) return 6;
            }
        }
        was_down = down;
        if (mm::mcu::delay_ms(poll_ms) != mm::mcu::Status::Ok) return 7;
    }
}
