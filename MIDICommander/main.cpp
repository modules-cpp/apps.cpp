#include "bundled_music.hpp"
#include "player.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

import mm.display;
import mm.fonts;
import mm.fs;
import mm.fs.fat;
import mm.fs.local;
import mm.gfx;
import mm.mcu;
import mm.sdcard.socket;
import mm.touch;

namespace {

using mm::display::Status;
using mm::gfx::Rgb565;
using mm::gfx::Surface;

constexpr unsigned int maximum_width = 240;
constexpr unsigned int maximum_height = 320;
constexpr unsigned int page_size = 6;
constexpr unsigned int text_buffer_size = 2048;
std::array<std::byte, maximum_width * maximum_height * 2u> frame_bytes{};
std::array<std::byte, text_buffer_size> text_bits{};

constexpr Rgb565 background = mm::gfx::rgb(10, 18, 27);
constexpr Rgb565 panel = mm::gfx::rgb(23, 42, 57);
constexpr Rgb565 row = mm::gfx::rgb(28, 51, 67);
constexpr Rgb565 selected_row = mm::gfx::rgb(34, 92, 105);
constexpr Rgb565 accent = mm::gfx::rgb(240, 177, 78);
constexpr Rgb565 white = mm::gfx::rgb(234, 239, 231);
constexpr Rgb565 dim = mm::gfx::rgb(152, 175, 181);

struct Entry {
    std::array<char, mm::fs::max_name + 1> name{};
    std::array<char, mm::fs::max_path + 1> path{};
    bool directory = false;
};

[[nodiscard]] bool midi_name(std::string_view name) {
    auto suffix = [name](std::string_view ending) {
        if (name.size() < ending.size()) return false;
        for (std::size_t i = 0; i < ending.size(); ++i) {
            char a = name[name.size() - ending.size() + i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 'a' - 'A');
            if (a != ending[i]) return false;
        }
        return true;
    };
    return suffix(".mid") || suffix(".midi");
}

class Browser {
public:
    ~Browser() {
        if (sd_) (void)mm::fs::fat::unmount("/sd");
        if (local_) (void)mm::fs::local::unmount("/data");
    }

    [[nodiscard]] bool initialize() {
        mount_local();
        mount_sd();
        if (local_) {
            set_root(false);
        } else if (sd_) {
            set_root(true);
        } else {
            set_root(false);
            last_source_sd_ = true;
            last_source_status_ = sd_status_;
            return false;
        }
        if (refresh()) return true;
        if (!on_sd_ && sd_) {
            set_root(true);
            if (refresh()) return true;
        }
        last_source_sd_ = on_sd_;
        last_source_status_ = directory_status_;
        return false;
    }

    [[nodiscard]] bool switch_source() {
        const bool target_sd = !on_sd_;
        last_source_sd_ = target_sd;
        if (!on_sd_) {
            if (!sd_) mount_sd();
            if (!sd_) {
                last_source_status_ = sd_status_;
                return false;
            }
        } else {
            if (!local_) mount_local();
            if (!local_) {
                last_source_status_ = local_status_;
                return false;
            }
        }
        const auto old_path = path_;
        const auto old_offset = offset_;
        set_root(target_sd);
        if (refresh()) {
            last_source_status_ = mm::fs::Status::Ok;
            return true;
        }
        last_source_status_ = directory_status_;
        on_sd_ = !target_sd;
        path_ = old_path;
        offset_ = old_offset;
        (void)refresh();
        return false;
    }

    [[nodiscard]] const char* source_error() const {
        if (!last_source_sd_) return "LittleFS unavailable";
        switch (last_source_status_) {
            case mm::fs::Status::Corrupt: return "SD needs FAT16/32";
            case mm::fs::Status::TransportError: return "SD card I/O error";
            case mm::fs::Status::Timeout:
                return sd_card_ready_ ? "SD FAT mount timeout" : "SD card init timeout";
            case mm::fs::Status::Busy: return "SDIO bus busy";
            case mm::fs::Status::Unsupported: return "SD unsupported";
            case mm::fs::Status::NotFound: return "SD card not found";
            default: return "SD source unavailable";
        }
    }

    [[nodiscard]] bool refresh() {
        shown_ = 0;
        more_ = false;
        selected_ = -1;
        mm::fs::Directory directory;
        directory_status_ = mm::fs::open_directory(path_.data(), directory);
        if (directory_status_ != mm::fs::Status::Ok)
            return false;
        std::array<char, mm::fs::max_name + 1> name{};
        unsigned int position = 0;
        for (;;) {
            std::size_t length = 0;
            mm::fs::Stat stat{};
            bool done = false;
            const auto result = directory.next(name, length, stat, done);
            if (result != mm::fs::Status::Ok) {
                directory_status_ = result;
                return false;
            }
            if (done) break;
            if (length == 0 || name[0] == '.') continue;
            if (stat.kind != mm::fs::Kind::Directory &&
                (stat.kind != mm::fs::Kind::File ||
                 !midi_name(std::string_view{name.data(), length}))) continue;
            if (position++ < offset_) continue;
            if (shown_ == page_size) { more_ = true; break; }
            auto& entry = entries_[shown_];
            if (!join(std::string_view{name.data(), length}, entry.path)) continue;
            std::memcpy(entry.name.data(), name.data(), length);
            entry.name[length] = 0;
            entry.directory = stat.kind == mm::fs::Kind::Directory;
            ++shown_;
        }
        directory_status_ = directory.close();
        return directory_status_ == mm::fs::Status::Ok;
    }

    [[nodiscard]] bool next_page() {
        if (!more_) return false;
        offset_ += page_size;
        return refresh();
    }
    [[nodiscard]] bool previous_page() {
        if (offset_ == 0) return false;
        offset_ = offset_ >= page_size ? offset_ - page_size : 0;
        return refresh();
    }
    [[nodiscard]] bool up() {
        if (std::strcmp(path_.data(), on_sd_ ? "/sd" : "/data") == 0)
            return false;
        char* slash = std::strrchr(path_.data(), '/');
        if (slash == nullptr) return false;
        *slash = 0;
        offset_ = 0;
        return refresh();
    }
    [[nodiscard]] bool open_selected() {
        const auto* entry = selected();
        if (entry == nullptr || !entry->directory) return false;
        path_ = entry->path;
        offset_ = 0;
        return refresh();
    }
    void select(unsigned int row_index) {
        if (row_index < shown_) selected_ = static_cast<int>(row_index);
    }
    [[nodiscard]] const Entry* selected() const {
        return selected_ >= 0 && static_cast<unsigned int>(selected_) < shown_ ?
               &entries_[static_cast<unsigned int>(selected_)] : nullptr;
    }
    [[nodiscard]] const Entry* entry(unsigned int i) const {
        return i < shown_ ? &entries_[i] : nullptr;
    }
    [[nodiscard]] int selected_index() const { return selected_; }
    [[nodiscard]] const char* path() const { return path_.data(); }
    [[nodiscard]] bool on_sd() const { return on_sd_; }

private:
    void mount_local() {
        if (local_) return;
        const auto local_status = mm::fs::local::mount("/data", {false, true});
        local_status_ = local_status;
        if (local_status == mm::fs::Status::Ok || local_status == mm::fs::Status::Exists) {
            local_ = true;
            seed_bundled_music_if_needed();
        }
    }
    void seed_bundled_music_if_needed() {
        for (const auto& song : midicommander::bundled_songs) {
            std::array<char, mm::fs::max_path + 1> full_path{};
            const std::string_view prefix = "/data/";
            if (prefix.size() + song.filename.size() > mm::fs::max_path) continue;
            std::memcpy(full_path.data(), prefix.data(), prefix.size());
            std::memcpy(full_path.data() + prefix.size(), song.filename.data(), song.filename.size());
            full_path[prefix.size() + song.filename.size()] = '\0';

            mm::fs::File file;
            const auto status = mm::fs::open(full_path.data(), mm::fs::Access::Write,
                                             mm::fs::Disposition::CreateNew, file);
            if (status == mm::fs::Status::Ok) {
                std::size_t written = 0;
                (void)file.write(std::as_bytes(song.data), written);
                (void)file.close();
            }
        }
    }
    void mount_sd() {
        if (sd_) return;
        auto& card = mm::sdcard::socket::card();
        mm::fs::BlockGeometry geometry{};
        sd_status_ = card.geometry(geometry);
        sd_card_ready_ = sd_status_ == mm::fs::Status::Ok;
        if (!sd_card_ready_) return;
        const auto result = mm::fs::fat::mount("/sd", card, {.read_only = true});
        sd_status_ = result;
        sd_ = result == mm::fs::Status::Ok;
    }
    void set_root(bool sd) {
        on_sd_ = sd;
        path_.fill(0);
        std::memcpy(path_.data(), sd ? "/sd" : "/data", sd ? 4 : 6);
        offset_ = 0;
    }
    [[nodiscard]] bool join(std::string_view name,
                            std::array<char, mm::fs::max_path + 1>& out) const {
        const auto length = std::strlen(path_.data());
        if (length + 1 + name.size() > mm::fs::max_path) return false;
        std::memcpy(out.data(), path_.data(), length);
        out[length] = '/';
        std::memcpy(out.data() + length + 1, name.data(), name.size());
        out[length + 1 + name.size()] = 0;
        return true;
    }

    std::array<Entry, page_size> entries_{};
    std::array<char, mm::fs::max_path + 1> path_{};
    unsigned int shown_ = 0;
    unsigned int offset_ = 0;
    int selected_ = -1;
    bool more_ = false;
    bool local_ = false;
    bool sd_ = false;
    bool sd_card_ready_ = false;
    bool on_sd_ = false;
    bool last_source_sd_ = true;
    mm::fs::Status local_status_ = mm::fs::Status::NotFound;
    mm::fs::Status sd_status_ = mm::fs::Status::NotFound;
    mm::fs::Status directory_status_ = mm::fs::Status::NotFound;
    mm::fs::Status last_source_status_ = mm::fs::Status::NotFound;
};

[[nodiscard]] bool text(Surface frame, std::string_view value, unsigned int x,
                        unsigned int y, Rgb565 color) {
    if (x >= frame.width || y >= frame.height) return true;
    std::array<char8_t, 64> characters{};
    unsigned int count = 0;
    while (count < value.size() && count < characters.size() &&
           (count + 1) * mm::fonts::kMono12.advance <= frame.width - x) {
        const auto raw = static_cast<unsigned char>(value[count]);
        characters[count] = static_cast<char8_t>(raw >= 32 && raw < 127 ? raw : '?');
        ++count;
    }
    if (count == 0) return true;
    auto width = count * mm::fonts::kMono12.advance;
    auto row_bytes = (width + 7u) / 8u;
    auto size = row_bytes * mm::fonts::kMono12.height;
    while (size > text_bits.size() && count > 1) {
        --count;
        width = count * mm::fonts::kMono12.advance;
        row_bytes = (width + 7u) / 8u;
        size = row_bytes * mm::fonts::kMono12.height;
    }
    if (size > text_bits.size()) return true;
    const Surface mask{width, mm::fonts::kMono12.height, 1,
                       std::span<std::byte>{text_bits}.first(size)};
    if (mm::gfx::fill(mask, mm::display::Color::Black) != Status::Ok ||
        mm::fonts::render(characters.data(), count, mm::fonts::kMono12,
                          mm::display::Color::White, 0, 0, mask) != Status::Ok)
        return false;
    for (unsigned int r = 0; r < mask.height; ++r) {
        if (y + r >= frame.height) break;
        for (unsigned int c = 0; c < mask.width; ++c) {
            if (x + c >= frame.width) break;
            const auto bits = mask.pixels[r * row_bytes + c / 8u];
            if ((bits & static_cast<std::byte>(0x80u >> (c % 8u))) != std::byte{0})
                (void)mm::gfx::pixel(frame, static_cast<int>(x + c),
                                     static_cast<int>(y + r), color);
        }
    }
    return true;
}

[[nodiscard]] bool text(Surface frame, const char* value, unsigned int x,
                        unsigned int y, Rgb565 color) {
    if (value == nullptr) return true;
    return text(frame, std::string_view{value}, x, y, color);
}

void append_uint(std::array<char, 64>& buf, std::size_t& len, std::uint64_t val) {
    std::array<char, 24> tmp{};
    std::size_t tlen = 0;
    do {
        tmp[tlen++] = static_cast<char>('0' + (val % 10u));
        val /= 10u;
    } while (val != 0 && tlen < tmp.size());
    while (tlen > 0 && len + 1 < buf.size()) {
        buf[len++] = tmp[--tlen];
    }
    buf[len] = '\0';
}

void append_str(std::array<char, 64>& buf, std::size_t& len, std::string_view str) {
    for (char c : str) {
        if (len + 1 >= buf.size()) break;
        buf[len++] = c;
    }
    buf[len] = '\0';
}

void format_duration(std::array<char, 64>& buf, std::size_t& len, unsigned int total_sec) {
    const unsigned int mins = total_sec / 60;
    const unsigned int secs = total_sec % 60;
    append_uint(buf, len, mins);
    if (len + 1 < buf.size()) buf[len++] = ':';
    if (secs < 10 && len + 1 < buf.size()) buf[len++] = '0';
    append_uint(buf, len, secs);
}

[[nodiscard]] bool draw(mm::display::Display& display, unsigned int width,
                        unsigned int height, const Browser& browser,
                        const char* status, bool playing, const char* playing_name,
                        bool showing_info, const char* info_name, const char* info_path) {
    const Surface frame{width, height, 16,
                        std::span<std::byte>{frame_bytes}.first(
                            static_cast<std::size_t>(width) * height * 2u)};
    if (mm::gfx::fill(frame, background) != Status::Ok) return false;

    if (showing_info) {
        (void)mm::gfx::fill_rectangle(frame, 0, 0, width, 35, panel);
        if (!text(frame, "MIDI FILE INFO", 7, 10, accent)) return false;

        (void)mm::gfx::fill_rectangle(frame, 5, 40, width - 10, height - 82, panel);
        const auto& info = midicommander::current_info();

        std::array<char, 64> line{};
        unsigned int y = 46;
        constexpr unsigned int step = 19;

        // 1. File Name
        std::size_t len = 0;
        append_str(line, len, "File: ");
        append_str(line, len, info_name != nullptr && info_name[0] != '\0' ? info_name : "(unknown)");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 2. Title
        len = 0;
        append_str(line, len, "Title: ");
        if (info.title[0] != '\0') {
            append_str(line, len, info.title.data());
            if (!text(frame, line.data(), 10, y, accent)) return false;
        } else {
            append_str(line, len, "(none)");
            if (!text(frame, line.data(), 10, y, dim)) return false;
        }
        y += step;

        // 3. File Size
        len = 0;
        append_str(line, len, "Size: ");
        append_uint(line, len, info.file_size);
        append_str(line, len, " bytes");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 4. SMF Format
        len = 0;
        append_str(line, len, "Format: SMF ");
        append_uint(line, len, info.format);
        append_str(line, len, info.format == 0 ? " (single)" : " (multi)");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 5. Tracks
        len = 0;
        append_str(line, len, "Tracks: ");
        append_uint(line, len, info.tracks);
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 6. PPQ Division
        len = 0;
        append_str(line, len, "Division: ");
        append_uint(line, len, info.division);
        append_str(line, len, " PPQ");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 7. Tempo
        len = 0;
        append_str(line, len, "Tempo: ");
        append_uint(line, len, info.bpm);
        append_str(line, len, " BPM (");
        append_uint(line, len, info.tempo_us);
        append_str(line, len, " us)");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 8. Duration
        len = 0;
        append_str(line, len, "Duration: ");
        format_duration(line, len, info.duration_s);
        append_str(line, len, " (");
        append_uint(line, len, info.duration_s);
        append_str(line, len, "s)");
        if (!text(frame, line.data(), 10, y, accent)) return false;
        y += step;

        // 9. Notes
        len = 0;
        append_str(line, len, "Notes: ");
        append_uint(line, len, info.note_count);
        append_str(line, len, " events");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 10. Volume
        len = 0;
        append_str(line, len, "Volume: ");
        append_uint(line, len, midicommander::volume());
        append_str(line, len, "%");
        if (!text(frame, line.data(), 10, y, white)) return false;
        y += step;

        // 11. Status / State
        len = 0;
        append_str(line, len, "State: ");
        append_str(line, len, playing ? "Playing" : "Ready to play");
        if (!text(frame, line.data(), 10, y, playing ? accent : dim)) return false;
        y += step;

        // 12. Full Path
        len = 0;
        append_str(line, len, "Path: ");
        append_str(line, len, info_path != nullptr ? info_path : "");
        if (!text(frame, line.data(), 10, y, dim)) return false;

        // Bottom action buttons:
        // Left: PLAY / STOP (selected_row)
        // Right: BACK (panel)
        const auto btn_y = height - 34;
        const auto half = width / 2;
        (void)mm::gfx::fill_rectangle(frame, 5, btn_y, half - 8, 27, selected_row);
        if (!text(frame, playing ? "STOP" : "PLAY", playing ? half / 2 - 14 : half / 2 - 14, btn_y + 8, white)) return false;

        (void)mm::gfx::fill_rectangle(frame, half + 3, btn_y, half - 8, 27, panel);
        if (!text(frame, "BACK", half + half / 2 - 14, btn_y + 8, white)) return false;

        return mm::gfx::write(display, frame, 0, 0) == Status::Ok &&
               display.refresh(mm::display::Refresh::Full) == Status::Ok;
    }

    (void)mm::gfx::fill_rectangle(frame, 0, 0, width, 35, panel);
    if (!text(frame, "MIDI COMMANDER", 7, 10, accent)) return false;

    if (playing) {
        // Song Info Card: y = 42 to 196
        (void)mm::gfx::fill_rectangle(frame, 8, 42, width - 16, 154, panel);
        if (!text(frame, "NOW PLAYING", 16, 50, accent) ||
            !text(frame, playing_name, 16, 72, white)) return false;

        const auto& info = midicommander::current_info();
        std::array<char, 64> line{};
        unsigned int py = 94;
        constexpr unsigned int pstep = 19;

        if (info.title[0] != '\0') {
            std::size_t len = 0;
            append_str(line, len, "Title: ");
            append_str(line, len, info.title.data());
            if (!text(frame, line.data(), 16, py, accent)) return false;
            py += pstep;
        }

        std::size_t len = 0;
        append_str(line, len, "SMF ");
        append_uint(line, len, info.format);
        append_str(line, len, " | ");
        append_uint(line, len, info.tracks);
        append_str(line, len, " trk | ");
        append_uint(line, len, info.bpm);
        append_str(line, len, " BPM");
        if (!text(frame, line.data(), 16, py, dim)) return false;
        py += pstep;

        len = 0;
        append_str(line, len, "Length: ");
        format_duration(line, len, info.duration_s);
        append_str(line, len, " | ");
        append_uint(line, len, info.note_count);
        append_str(line, len, " notes");
        if (!text(frame, line.data(), 16, py, white)) return false;
        py += pstep;

        len = 0;
        append_str(line, len, "PPQ: ");
        append_uint(line, len, info.division);
        append_str(line, len, " | 8-Voice Piano");
        if (!text(frame, line.data(), 16, py, dim)) return false;

        // Volume Control Panel: y = 202 to 246 (height 44)
        (void)mm::gfx::fill_rectangle(frame, 8, 202, width - 16, 44, panel);

        // VOL- button (x = 12 to 58)
        (void)mm::gfx::fill_rectangle(frame, 12, 206, 46, 36, selected_row);
        if (!text(frame, "VOL-", 21, 217, white)) return false;

        // VOL+ button (x = width - 58 to width - 12)
        (void)mm::gfx::fill_rectangle(frame, width - 58, 206, 46, 36, selected_row);
        if (!text(frame, "VOL+", width - 49, 217, white)) return false;

        // Center Volume text and gauge (x = 62 to width - 62 = 178)
        const auto vol = midicommander::volume();
        std::array<char, 64> vol_buf{};
        std::size_t vlen = 0;
        append_str(vol_buf, vlen, "VOL: ");
        append_uint(vol_buf, vlen, vol);
        append_str(vol_buf, vlen, "%");
        if (!text(frame, vol_buf.data(), 89, 210, accent)) return false;

        // Gauge bar: x = 66, y = 230, width = 108, height = 9
        (void)mm::gfx::fill_rectangle(frame, 66, 230, 108, 9, background);
        if (vol > 0) {
            const unsigned int bar_w = (108u * vol) / 100u;
            (void)mm::gfx::fill_rectangle(frame, 66, 230, bar_w > 108u ? 108u : bar_w, 9, accent);
        }

        // Bottom buttons: STOP (width 142) and INFO (width 74)
        const auto stop_btn_y = height - 46;
        (void)mm::gfx::fill_rectangle(frame, 8, stop_btn_y, width - 96, 38, selected_row);
        if (!text(frame, "STOP", (width - 96) / 2 - 14, stop_btn_y + 12, white)) return false;

        (void)mm::gfx::fill_rectangle(frame, width - 82, stop_btn_y, 74, 38, panel);
        if (!text(frame, "INFO", width - 82 + 23, stop_btn_y + 12, white)) return false;
    } else {
        (void)mm::gfx::fill_rectangle(frame, width - 71, 38, 66, 29, panel);
        if (!text(frame, browser.on_sd() ? "TO LFS" : "TO SD", width - 68, 47,
                  white)) return false;
        if (!text(frame, browser.path(), 6, 45, dim)) return false;

        for (unsigned int i = 0; i < page_size; ++i) {
            const auto* entry = browser.entry(i);
            const unsigned int y = 73 + i * 25;
            (void)mm::gfx::fill_rectangle(frame, 5, y, width - 10, 23,
                browser.selected_index() == static_cast<int>(i) ? selected_row : row);
            if (entry != nullptr) {
                if (!text(frame, entry->directory ? "[D]" : "[M]", 8, y + 5,
                          entry->directory ? accent : dim) ||
                    !text(frame, entry->name.data(), 40, y + 5, white)) return false;
            }
        }
        const auto controls_y = height - 66;
        const auto third = width / 3;
        for (unsigned int i = 0; i < 3; ++i)
            (void)mm::gfx::fill_rectangle(frame, 5 + i * third, controls_y,
                                          third - 7, 28, panel);
        if (!text(frame, "PREV", 11, controls_y + 8, white) ||
            !text(frame, "NEXT", third + 10, controls_y + 8, white) ||
            !text(frame, "UP", 2 * third + 12, controls_y + 8, white))
            return false;

        const auto bottom_y = height - 34;
        (void)mm::gfx::fill_rectangle(frame, 5, bottom_y, width - 86, 27, selected_row);
        if (!text(frame, "OPEN / PLAY", (width - 86) / 2 - 37, bottom_y + 8, white))
            return false;

        (void)mm::gfx::fill_rectangle(frame, width - 76, bottom_y, 71, 27, panel);
        if (!text(frame, "INFO", width - 76 + 21, bottom_y + 8, white))
            return false;
    }
    if (!text(frame, status, 6, height - (playing ? 68 : 82), accent)) return false;
    return mm::gfx::write(display, frame, 0, 0) == Status::Ok &&
           display.refresh(mm::display::Refresh::Full) == Status::Ok;
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
    const auto panel_geometry = display.geometry();
    const auto touch_geometry = touch.geometry();
    if (panel_geometry.bits_per_pixel != 16 || panel_geometry.width < 220 ||
        panel_geometry.width > maximum_width || panel_geometry.height < 280 ||
        panel_geometry.height > maximum_height || touch_geometry.width == 0 ||
        touch_geometry.height == 0) return 3;
    Browser browser;
    const bool storage_ok = browser.initialize();
    const bool audio_ok = midicommander::initialize();

    bool showing_info = false;
    std::array<char, mm::fs::max_name + 1> info_name{};
    std::array<char, mm::fs::max_path + 1> info_path{};

    const char* status = "Select a MIDI file";
    if (!storage_ok) {
        status = browser.source_error();
    } else if (!audio_ok) {
        status = midicommander::error();
    }
    const char* playing_name = "";
    (void)draw(display, panel_geometry.width, panel_geometry.height, browser,
               status, false, playing_name,
               showing_info, info_name.data(), info_path.data());

    bool was_touched = false;
    unsigned long last_touch_ms = 0;
    unsigned long last_poll_ms = 0;
    unsigned int last_x = 0;
    unsigned int last_y = 0;
    std::array<mm::touch::Point, 1> points{};
    for (;;) {
        if (midicommander::active() && !midicommander::service()) {
            midicommander::stop();
            status = midicommander::error();
            playing_name = "";
            (void)draw(display, panel_geometry.width, panel_geometry.height,
                       browser, status, false, playing_name,
                       showing_info, info_name.data(), info_path.data());
        }
        if (!midicommander::active() && playing_name[0] != 0) {
            playing_name = "";
            status = midicommander::error();
            (void)draw(display, panel_geometry.width, panel_geometry.height,
                       browser, status, false, playing_name,
                       showing_info, info_name.data(), info_path.data());
        }

        unsigned long now_ms = 0;
        if (mm::mcu::ticks_ms(now_ms) != mm::mcu::Status::Ok) {
            (void)mm::mcu::delay_ms(1);
            continue;
        }
        if (midicommander::active() && now_ms - last_poll_ms < 12u) {
            (void)mm::mcu::delay_ms(1);
            continue;
        }
        last_poll_ms = now_ms;
        std::size_t count = 0;
        if (touch.read(points, count) != mm::touch::Status::Ok) {
            count = 0;
        }
        bool touched = count != 0;
        if (touched) {
            last_x = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].x) * panel_geometry.width /
                touch_geometry.width);
            last_y = static_cast<unsigned int>(
                static_cast<std::uint64_t>(points[0].y) * panel_geometry.height /
                touch_geometry.height);
            last_touch_ms = now_ms;
        } else if (was_touched && now_ms - last_touch_ms < 35u) {
            touched = true;
        }
        if (touched && !was_touched) {
            if (showing_info) {
                if (last_y >= panel_geometry.height - 34) {
                    const auto half = panel_geometry.width / 2;
                    if (last_x < half) {
                        // Left button: STOP (if active) or PLAY (if inactive)
                        if (midicommander::active()) {
                            midicommander::stop();
                            playing_name = "";
                            status = "Stopped";
                            showing_info = false;
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, false, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                        } else if (info_name[0] != '\0') {
                            playing_name = info_name.data();
                            status = "Piano playback";
                            showing_info = false;
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, true, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                            if (!midicommander::start()) {
                                playing_name = "";
                                status = midicommander::error();
                                (void)draw(display, panel_geometry.width, panel_geometry.height,
                                           browser, status, false, playing_name,
                                           showing_info, info_name.data(), info_path.data());
                            }
                        }
                    } else {
                        // Right button: BACK
                        showing_info = false;
                        (void)draw(display, panel_geometry.width, panel_geometry.height,
                                   browser, status, midicommander::active(), playing_name,
                                   showing_info, info_name.data(), info_path.data());
                    }
                }
            } else if (midicommander::active()) {
                if (last_y >= panel_geometry.height - 48) {
                    if (last_x < panel_geometry.width - 82) {
                        midicommander::stop();
                        playing_name = "";
                        status = "Stopped";
                        (void)draw(display, panel_geometry.width, panel_geometry.height,
                                   browser, status, false, playing_name,
                                   showing_info, info_name.data(), info_path.data());
                    } else {
                        showing_info = true;
                        (void)draw(display, panel_geometry.width, panel_geometry.height,
                                   browser, status, true, playing_name,
                                   showing_info, info_name.data(), info_path.data());
                    }
                } else if (last_y >= 202 && last_y < 250) {
                    unsigned int current = midicommander::volume();
                    if (last_x < 60) {
                        if (current >= 10) current -= 10;
                        else current = 0;
                    } else if (last_x >= panel_geometry.width - 60) {
                        if (current <= 90) current += 10;
                        else current = 100;
                    } else if (last_x >= 66 && last_x < 174) {
                        int pct = static_cast<int>((last_x - 66) * 100 / 108);
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        current = static_cast<unsigned int>(((pct + 5) / 10) * 10);
                    }
                    midicommander::set_volume(current);
                    (void)midicommander::service();
                    (void)draw(display, panel_geometry.width, panel_geometry.height,
                               browser, status, true, playing_name,
                               showing_info, info_name.data(), info_path.data());
                }
            } else if (last_y >= 38 && last_y < 67 &&
                       last_x >= panel_geometry.width - 71) {
                status = browser.switch_source() ? "Source changed" :
                                                    browser.source_error();
                (void)draw(display, panel_geometry.width, panel_geometry.height,
                           browser, status, false, playing_name,
                           showing_info, info_name.data(), info_path.data());
            } else if (last_y >= 73 && last_y < 73 + page_size * 25) {
                const auto tapped_row = (last_y - 73) / 25;
                if (browser.selected_index() == static_cast<int>(tapped_row)) {
                    // Double tap: open folder or play file
                    const auto* entry = browser.selected();
                    if (entry != nullptr && entry->directory) {
                        if (browser.open_selected()) status = "Folder opened";
                        else status = "Cannot open folder";
                        (void)draw(display, panel_geometry.width, panel_geometry.height,
                                   browser, status, false, playing_name,
                                   showing_info, info_name.data(), info_path.data());
                    } else if (entry != nullptr) {
                        if (midicommander::load(entry->path.data())) {
                            info_name = entry->name;
                            info_path = entry->path;
                            playing_name = entry->name.data();
                            status = "Piano playback";
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, true, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                            if (!midicommander::start()) {
                                playing_name = "";
                                status = midicommander::error();
                                (void)draw(display, panel_geometry.width,
                                           panel_geometry.height, browser, status,
                                           false, playing_name,
                                           showing_info, info_name.data(), info_path.data());
                            }
                        } else {
                            status = midicommander::error();
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, false, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                        }
                    }
                } else {
                    browser.select(tapped_row);
                    (void)draw(display, panel_geometry.width, panel_geometry.height,
                               browser, status, false, playing_name,
                               showing_info, info_name.data(), info_path.data());
                }
            } else if (last_y >= panel_geometry.height - 66 &&
                       last_y < panel_geometry.height - 38) {
                const auto third = panel_geometry.width / 3;
                bool changed = false;
                if (last_x < third) changed = browser.previous_page();
                else if (last_x < 2 * third) changed = browser.next_page();
                else changed = browser.up();
                if (changed) {
                    (void)draw(display, panel_geometry.width,
                               panel_geometry.height, browser, status,
                               false, playing_name,
                               showing_info, info_name.data(), info_path.data());
                }
            } else if (last_y >= panel_geometry.height - 34) {
                if (last_x < panel_geometry.width - 76) {
                    // OPEN / PLAY
                    const auto* entry = browser.selected();
                    if (entry != nullptr && entry->directory) {
                        if (browser.open_selected()) status = "Folder opened";
                        else status = "Cannot open folder";
                        (void)draw(display, panel_geometry.width, panel_geometry.height,
                                   browser, status, false, playing_name,
                                   showing_info, info_name.data(), info_path.data());
                    } else if (entry != nullptr) {
                        if (midicommander::load(entry->path.data())) {
                            info_name = entry->name;
                            info_path = entry->path;
                            playing_name = entry->name.data();
                            status = "Piano playback";
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, true, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                            if (!midicommander::start()) {
                                playing_name = "";
                                status = midicommander::error();
                                (void)draw(display, panel_geometry.width,
                                           panel_geometry.height, browser, status,
                                           false, playing_name,
                                           showing_info, info_name.data(), info_path.data());
                            }
                        } else {
                            status = midicommander::error();
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, false, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                        }
                    }
                } else {
                    // INFO button
                    const auto* entry = browser.selected();
                    if (entry != nullptr) {
                        if (entry->directory) {
                            status = "Select a MIDI file";
                            (void)draw(display, panel_geometry.width, panel_geometry.height,
                                       browser, status, false, playing_name,
                                       showing_info, info_name.data(), info_path.data());
                        } else {
                            if (midicommander::load(entry->path.data())) {
                                info_name = entry->name;
                                info_path = entry->path;
                                showing_info = true;
                                status = "Ready to play";
                                (void)draw(display, panel_geometry.width, panel_geometry.height,
                                           browser, status, false, playing_name,
                                           showing_info, info_name.data(), info_path.data());
                            } else {
                                status = midicommander::error();
                                (void)draw(display, panel_geometry.width, panel_geometry.height,
                                           browser, status, false, playing_name,
                                           showing_info, info_name.data(), info_path.data());
                            }
                        }
                    } else {
                        status = "Select a file first";
                        (void)draw(display, panel_geometry.width, panel_geometry.height,
                                   browser, status, false, playing_name,
                                   showing_info, info_name.data(), info_path.data());
                    }
                }
            }
        }
        was_touched = touched;
        (void)mm::mcu::delay_ms(midicommander::active() ? 1 : 20);
    }
}
