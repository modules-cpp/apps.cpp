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
import mm.sdcard.socket;
import mm.touch;

namespace filecommander {
mm::fs::Status initialize();
mm::fs::Status erase_and_format();
mm::fs::Status switch_volume();
bool sd_selected();
void shutdown();
bool ready();
const char* path();
unsigned int count();
bool has_more();
const char* name(unsigned int);
bool directory(unsigned int);
std::uint64_t size(unsigned int);
unsigned int depth(unsigned int);
mm::fs::Status refresh(unsigned int, unsigned int, bool);
mm::fs::Status enter(unsigned int);
mm::fs::Status up();
mm::fs::Status create(std::string_view, bool);
mm::fs::Status rename(unsigned int, std::string_view);
mm::fs::Status remove(unsigned int);
mm::fs::Status read(unsigned int, std::uint64_t, std::span<std::byte>, std::size_t&);
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

enum class Mode { List, View, Info, Edit, DeleteConfirm, FormatConfirm };
enum class Edit { NewFile, NewDir, Rename, Append };
enum class Theme { Amber, Green };
enum class ViewFormat { Ascii, Hex };

struct State {
    Mode mode = Mode::List;
    Edit edit = Edit::NewFile;
    Theme theme = Theme::Amber;
    ViewFormat view_format = ViewFormat::Ascii;
    bool tree = false;
    unsigned int page = 0;
    unsigned int selected = 0;
    unsigned int rows = 0;
    std::array<char, name_limit + 1> input{};
    unsigned int input_size = 0;
    const char* message = "";
    std::size_t preview_size = 0;
    std::array<std::uint64_t, 64> view_offsets{};
    unsigned int view_page = 0;
    unsigned int view_pages = 1;
    unsigned int info_page = 0;
    mm::fs::Space volume_space{};
    mm::fs::Status volume_status = mm::fs::Status::NotFound;
    mm::mcu::FlashRegionGeometry flash_geometry{};
    mm::mcu::Status flash_status = mm::mcu::Status::Unsupported;
    mm::fs::BlockGeometry sd_geometry{};
    mm::fs::Status sd_status = mm::fs::Status::Unsupported;
    mm::fs::Status mount_error = mm::fs::Status::Ok;
};

bool littlefs_volume(const State& state) {
    return !filecommander::sd_selected() &&
           state.volume_status == mm::fs::Status::Ok &&
           state.flash_status == mm::mcu::Status::Ok &&
           state.flash_geometry.erase_size != 0 &&
           state.volume_space.total == state.flash_geometry.size;
}

void refresh_info(State& state) {
    state.volume_space = {};
    state.flash_geometry = {};
    state.sd_geometry = {};
    state.flash_status = mm::mcu::Status::Unsupported;
    state.sd_status = mm::fs::Status::Unsupported;
    state.volume_status = filecommander::ready() ?
        mm::fs::space(filecommander::sd_selected() ? "/sd" : "/data",
                      state.volume_space) : state.mount_error;
    if (filecommander::sd_selected()) {
        if (filecommander::ready())
            state.sd_status = mm::sdcard::socket::card().geometry(state.sd_geometry);
        return;
    }
#if defined(__linux__)
    // A Linux flash geometry query can create an unrelated mapped image file.
    state.flash_status = mm::mcu::Status::Unsupported;
#else
    state.flash_status = mm::mcu::flash_region_geometry(state.flash_geometry);
#endif
}

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
    const std::string_view volume_name = filecommander::sd_selected() ? "SD >" : "LFS >";
    const std::string_view tree_name = state.tree ? "TREE >" : "LIST >";
    const unsigned int theme_x = layout.width - 4u -
        static_cast<unsigned int>(theme_name.size()) * mm::fonts::kMono12.advance;
    const unsigned int volume_x = theme_x -
        static_cast<unsigned int>(volume_name.size() + 1u) * mm::fonts::kMono12.advance;
    const unsigned int tree_x = layout.width - 4u -
        static_cast<unsigned int>(tree_name.size()) * mm::fonts::kMono12.advance;
    const unsigned int info_x = layout.width - 4u - 6u * mm::fonts::kMono12.advance;
    if (!label(frame, "FILE COMMANDER", 4, 4) ||
        !label(frame, volume_name, volume_x, 4) ||
        !label(frame, theme_name, theme_x, 4) ||
        !box(frame, 2, 19, layout.width - 4u, 1, true) ||
        !label(frame, filecommander::path(), 4, 24) ||
        !box(frame, tree_x - 3u, 23, layout.width - tree_x + 1u, 16, false) ||
        !label(frame, tree_name, tree_x, 24) ||
        !label(frame, state.message, 4, 43) ||
        !box(frame, info_x - 3u, 42, layout.width - info_x + 1u, 16, false) ||
        !label(frame, "INFO >", info_x, 43)) return false;
    if (!filecommander::ready()) {
        if (!label(frame, "Storage is unavailable", 6, 82) ||
            !label(frame, error_text(state.mount_error), 6, 104)) return false;
        if (!filecommander::sd_selected() &&
            state.mount_error == mm::fs::Status::Corrupt) {
            return button(frame, layout, 1, 0, "ERASE");
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
        const unsigned int depth = state.tree ? filecommander::depth(i) : 0u;
        const unsigned int indent = depth * 14u;
        if (depth != 0u && !label(frame, "|-", 5u + indent - 14u, y, selected))
            return false;
        if (!label(frame, filecommander::directory(i) ? "[D]" : "[F]",
                   5u + indent, y, selected) ||
            !label(frame, filecommander::name(i), 36u + indent, y, selected)) return false;
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

unsigned int view_lines(const Layout& layout) {
    unsigned int lines = 0;
    for (unsigned int y = 67; y + 17u < layout.toolbar_y(); y += 18u) ++lines;
    return lines;
}

std::size_t visible_ascii_bytes(const Layout& layout, const State& state) {
    const unsigned int columns = (layout.width - 10u) / mm::fonts::kMono12.advance;
    const unsigned int lines = view_lines(layout);
    unsigned int line = 0, column = 0;
    for (std::size_t i = 0; i < state.preview_size; ++i) {
        const auto ch = static_cast<unsigned char>(preview[i]);
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (++line == lines) return i + 1u;
            column = 0;
            continue;
        }
        if (column == columns) {
            if (++line == lines) return i;
            column = 0;
        }
        ++column;
    }
    return state.preview_size;
}

unsigned int hex_width(const Layout& layout) { return layout.width < 320u ? 4u : 8u; }

std::size_t visible_bytes(const Layout& layout, const State& state) {
    if (state.view_format == ViewFormat::Ascii) return visible_ascii_bytes(layout, state);
    const std::size_t capacity = view_lines(layout) * hex_width(layout);
    return state.preview_size < capacity ? state.preview_size : capacity;
}

char hex_digit(unsigned int value) {
    return "0123456789ABCDEF"[value & 15u];
}

bool render_view(Surface frame, const Layout& layout, const State& state) {
    if (!label(frame, filecommander::name(state.selected), 4, 5) ||
        !label(frame, state.view_format == ViewFormat::Ascii ? "ASCII TEXT" : "HEX DUMP", 4, 27) ||
        !label(frame, state.message, 4, 47)) return false;
    const auto start = state.view_offsets[state.view_page];
    if (state.view_format == ViewFormat::Ascii) {
        const unsigned int columns = (layout.width - 10u) / mm::fonts::kMono12.advance;
        unsigned int x = 5, y = 67, column = 0;
        const auto count = visible_ascii_bytes(layout, state);
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char ch = static_cast<unsigned char>(preview[i]);
            if (ch == '\r') continue;
            if (ch == '\n') { y += 18u; x = 5; column = 0; continue; }
            if (column == columns) { y += 18u; x = 5; column = 0; }
            const char printable = ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.';
            if (!label(frame, {&printable, 1}, x, y)) return false;
            x += mm::fonts::kMono12.advance;
            ++column;
        }
    } else {
        const unsigned int width = hex_width(layout);
        const auto count = visible_bytes(layout, state);
        for (std::size_t i = 0; i < count; i += width) {
            std::array<char, 48> line{};
            const auto offset = start + i;
            for (unsigned int digit = 0; digit < 8; ++digit)
                line[digit] = hex_digit(static_cast<unsigned int>(offset >> ((7u - digit) * 4u)));
            line[8] = ':'; line[9] = ' ';
            unsigned int cursor = 10;
            for (unsigned int j = 0; j < width; ++j) {
                if (i + j < count) {
                    const auto byte = static_cast<unsigned char>(preview[i + j]);
                    line[cursor++] = hex_digit(byte >> 4u);
                    line[cursor++] = hex_digit(byte);
                } else { line[cursor++] = ' '; line[cursor++] = ' '; }
                line[cursor++] = ' ';
            }
            line[cursor++] = ' ';
            for (unsigned int j = 0; j < width && i + j < count; ++j) {
                const auto byte = static_cast<unsigned char>(preview[i + j]);
                line[cursor++] = byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.';
            }
            if (!label(frame, {line.data(), cursor}, 5, 67u +
                       static_cast<unsigned int>(i / width) * 18u)) return false;
        }
    }
    return button(frame, layout, 0, 0, "BACK") &&
           button(frame, layout, 0, 1, "PREV", state.view_page != 0) &&
           button(frame, layout, 0, 2, "NEXT", start + visible_bytes(layout, state) <
                  filecommander::size(state.selected)) &&
           button(frame, layout, 0, 3, state.view_format == ViewFormat::Ascii ? "HEX" : "ASCII") &&
           button(frame, layout, 1, 0, "APPEND") &&
           button(frame, layout, 1, 1, "RENAME") &&
           button(frame, layout, 1, 2, "DELETE");
}

bool info_text(Surface frame, unsigned int row, std::string_view value) {
    return label(frame, value, 5, 26u + row * 15u);
}

bool info_number(Surface frame, unsigned int row, std::string_view title,
                 std::uint64_t value, std::string_view suffix = {}) {
    std::array<char, 48> line{};
    std::size_t length = 0;
    for (char ch : title) line[length++] = ch;
    const std::size_t start = length;
    do {
        line[length++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    for (std::size_t left = start, right = length - 1u; left < right; ++left, --right) {
        const char temporary = line[left];
        line[left] = line[right];
        line[right] = temporary;
    }
    for (char ch : suffix) line[length++] = ch;
    return info_text(frame, row, {line.data(), length});
}

bool render_info(Surface frame, const Layout& layout, const State& state) {
    const bool sd = filecommander::sd_selected();
    const bool littlefs = littlefs_volume(state);
    const auto& geometry = state.flash_geometry;
    const auto& space = state.volume_space;
    const auto board = mm::mcu::board().name;
    if (!label(frame, state.info_page == 0 ? "VOLUME INFO 1/3" :
                      state.info_page == 1 ? "STORAGE GEOMETRY 2/3" :
                                             "DETAILS 3/3", 5, 4) ||
        !box(frame, 3, 19, layout.width - 6u, 1, true)) return false;
    if (state.info_page == 0) {
        if (!info_text(frame, 0, sd ? "Mount: /sd" : "Mount: /data") ||
            !info_text(frame, 1, board.empty() ? "Board: unspecified" : board) ||
            !info_text(frame, 2, filecommander::ready() ? "State: mounted" :
                       error_text(state.mount_error)) ||
            !info_text(frame, 3, sd ? "Backend: FAT" : littlefs ? "Backend: LittleFS" :
                       "Backend: local volume") ||
            !info_text(frame, 4, "Access: read/write")) return false;
        if (state.volume_status == mm::fs::Status::Ok) {
            const std::uint64_t used = space.total >= space.free ?
                space.total - space.free : 0;
            if (!info_number(frame, 5, "Capacity: ", space.total, " B") ||
                !info_number(frame, 6, littlefs ? "Free est: " : "Available: ",
                             space.free, " B") ||
                !info_number(frame, 7, littlefs ? "Allocated: " : "Disk used: ",
                             used, " B") ||
                !info_text(frame, 8, sd ? "FAT space from card" :
                           littlefs ? "Used = allocated blocks" :
                           "Host disk, not folder")) return false;
        } else if (!info_text(frame, 5, "Space: unavailable") ||
                   !info_text(frame, 6, error_text(state.volume_status))) return false;
    } else if (state.info_page == 1) {
        if (sd) {
            if (state.sd_status == mm::fs::Status::Ok) {
                if (!info_number(frame, 0, "Sectors: ", state.sd_geometry.count) ||
                    !info_number(frame, 1, "Sector size: ", state.sd_geometry.size, " B") ||
                    !info_number(frame, 2, "Card bytes: ",
                                 state.sd_geometry.count * state.sd_geometry.size)) return false;
            } else if (!info_text(frame, 0, "SD geometry unavailable") ||
                       !info_text(frame, 1, error_text(state.sd_status))) return false;
        } else if (!littlefs) {
            if (!info_text(frame, 0, "No LittleFS geometry") ||
                !info_text(frame, 1, "for this local volume") ||
                !info_text(frame, 3, "Flash region absent or") ||
                !info_text(frame, 4, "not this volume")) return false;
        } else if (!info_number(frame, 0, "Flash region: ", geometry.size, " B") ||
                   !info_number(frame, 1, "Read unit: ", geometry.read_size, " B") ||
                   !info_number(frame, 2, "Program unit: ", geometry.program_size, " B") ||
                   !info_number(frame, 3, "Erase/block: ", geometry.erase_size, " B") ||
                   !info_number(frame, 4, "Block count: ",
                                geometry.size / geometry.erase_size) ||
                   !info_number(frame, 5, "Cache: ", geometry.program_size, " B") ||
                   !info_text(frame, 6, "Lookahead: 32 B") ||
                   !info_text(frame, 7, "Block cycles: 500") ||
                   !info_number(frame, 8, "Max name: ", mm::fs::max_name, " B")) return false;
    } else {
        if (!info_text(frame, 0, sd ? "SD card FAT volume" :
                                     littlefs ? "Pico LittleFS provider" :
                                     "Local volume provider") ||
            !info_text(frame, 1, sd ? "Card must be FAT formatted" :
                                     littlefs ? "Free = block estimate" :
                                     "Space = host disk") ||
            !info_text(frame, 2, "File bytes may differ") ||
            !info_text(frame, 3, "Wear counts: unavailable") ||
            !info_text(frame, 4, "On-disk rev: unavailable") ||
            !info_number(frame, 5, "Max path: ", mm::fs::max_path, " B") ||
            !info_text(frame, 6, sd ? "No SD format action" :
                                     "Erase/format: confirm") ||
            !info_text(frame, 7, "REFRESH rereads space")) return false;
    }
    return button(frame, layout, 0, 0, "BACK") &&
           button(frame, layout, 0, 1, "PREV", state.info_page != 0) &&
           button(frame, layout, 0, 2, "NEXT", state.info_page != 2) &&
           button(frame, layout, 0, 3, "REFRESH");
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
    if (!label(frame, formatting ? "ERASE + FORMAT?" : "DELETE ITEM?", 6, 54) ||
        !label(frame, formatting ? "All flash files lost" :
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
                          state.mode == Mode::Info ? render_info(frame, layout, state) :
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
    auto status = filecommander::refresh(state.page * state.rows, state.rows, state.tree);
    if (status == mm::fs::Status::Ok && filecommander::count() == 0 && state.page != 0) {
        --state.page;
        status = filecommander::refresh(state.page * state.rows, state.rows, state.tree);
    }
    if (state.selected >= filecommander::count()) state.selected = 0;
    if (status != mm::fs::Status::Ok) state.message = error_text(status);
}

mm::fs::Status load_view(State& state) {
    state.preview_size = 0;
    const auto status = filecommander::read(state.selected,
        state.view_offsets[state.view_page], preview, state.preview_size);
    state.message = error_text(status);
    return status;
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
        state.view_page = 0;
        state.view_pages = 1;
        state.view_offsets[0] = 0;
        state.view_format = ViewFormat::Ascii;
        if (load_view(state) == mm::fs::Status::Ok) state.mode = Mode::View;
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
        const unsigned int theme_x = layout.width - 4u - 7u * mm::fonts::kMono12.advance;
        const unsigned int volume_x = theme_x - 6u * mm::fonts::kMono12.advance;
        if (x >= theme_x) {
            state.theme = state.theme == Theme::Amber ? Theme::Green : Theme::Amber;
        } else if (x >= volume_x) {
            state.mount_error = filecommander::switch_volume();
            state.message = error_text(state.mount_error);
            state.page = 0;
            state.selected = 0;
            if (filecommander::ready()) reload(state);
        }
        return;
    }
    if (state.mode == Mode::List && y < 42u) {
        state.tree = !state.tree;
        state.page = 0;
        state.selected = 0;
        reload(state);
        return;
    }
    if (state.mode == Mode::List && y < 59u) {
        state.info_page = 0;
        refresh_info(state);
        state.mode = Mode::Info;
        return;
    }
    if (state.mode == Mode::Info) {
        if (y >= layout.toolbar_y() && y < layout.toolbar_y() + 33u) {
            const unsigned int column = x * 4u / layout.width;
            if (column == 0) state.mode = Mode::List;
            else if (column == 1 && state.info_page != 0) --state.info_page;
            else if (column == 2 && state.info_page != 2) ++state.info_page;
            else if (column == 3) refresh_info(state);
        }
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
                    filecommander::erase_and_format() : filecommander::remove(state.selected);
                state.mount_error = status;
                state.message = error_text(status);
                state.mode = Mode::List;
                if (status == mm::fs::Status::Ok) reload(state);
            }
        }
        return;
    }
    if (state.mode == Mode::View) {
        if (y >= layout.toolbar_y()) {
            const unsigned int row = (y - layout.toolbar_y()) / 33u;
            const unsigned int column = x * 4u / layout.width;
            if (row == 0) {
                if (column == 0) state.mode = Mode::List;
                else if (column == 1 && state.view_page != 0) {
                    --state.view_page;
                    load_view(state);
                } else if (column == 2) {
                    const auto next = state.view_offsets[state.view_page] + visible_bytes(layout, state);
                    if (next < filecommander::size(state.selected)) {
                        if (state.view_page + 1u < state.view_pages) ++state.view_page;
                        else {
                            if (state.view_pages == state.view_offsets.size()) {
                                for (unsigned int i = 1; i < state.view_pages; ++i)
                                    state.view_offsets[i - 1u] = state.view_offsets[i];
                                --state.view_pages;
                                --state.view_page;
                            }
                            state.view_offsets[state.view_pages++] = next;
                            ++state.view_page;
                        }
                        load_view(state);
                    }
                } else if (column == 3) {
                    state.view_format = state.view_format == ViewFormat::Ascii ?
                        ViewFormat::Hex : ViewFormat::Ascii;
                    state.view_page = 0;
                    state.view_pages = 1;
                    state.view_offsets[0] = 0;
                    load_view(state);
                }
            } else if (row == 1) {
                if (column == 0) begin_edit(state, Edit::Append);
                else if (column == 1) begin_edit(state, Edit::Rename);
                else if (column == 2) state.mode = Mode::DeleteConfirm;
            }
        }
        return;
    }
    if (!filecommander::ready()) {
        if (!filecommander::sd_selected() &&
            state.mount_error == mm::fs::Status::Corrupt &&
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
    state.mount_error = filecommander::initialize();
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
