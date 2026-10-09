#include "launcher.hpp"
#include "bundled_games.hpp"
#include "chip8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

import mm.fs;
import mm.fs.local;
import mm.fs.fat;
import mm.sdcard.socket;

namespace funcommander {
namespace {

std::array<char, mm::fs::max_path + 1> current_path{ '/', 'g', 'a', 'm', 'e', 's', '\0' };
std::array<Entry, max_cached_entries> entries{};
unsigned int entry_count = 0;
unsigned int page_index = 0;
unsigned int g_page_size = default_page_size;
bool littlefs_mounted = false;
bool sd_mounted = false;

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > mm::fs::max_name ||
        name == "." || name == "..") return false;
    for (char ch : name) {
        if (ch == '/' || ch == '\\' || ch == '\0' ||
            static_cast<unsigned char>(ch) < 32) return false;
    }
    return true;
}

bool is_chip8_filename(std::string_view name) {
    if (name.size() < 4) return false;
    auto has_suffix = [](std::string_view str, std::string_view suffix) {
        if (str.size() < suffix.size()) return false;
        auto end = str.substr(str.size() - suffix.size());
        for (std::size_t i = 0; i < suffix.size(); ++i) {
            char a = end[i];
            char b = suffix[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
            if (a != b) return false;
        }
        return true;
    };
    return has_suffix(name, ".ch8") || has_suffix(name, ".c8") || has_suffix(name, ".rom");
}

bool join_path(std::string_view parent, std::string_view name,
               std::array<char, mm::fs::max_path + 1>& out) {
    if (!valid_name(name)) return false;
    const std::size_t p_len = parent.size();
    const std::size_t n_len = name.size();
    const bool needs_slash = (p_len > 0 && parent[p_len - 1] != '/');
    const std::size_t total = p_len + (needs_slash ? 1 : 0) + n_len;
    if (total > mm::fs::max_path) return false;

    std::memcpy(out.data(), parent.data(), p_len);
    std::size_t cursor = p_len;
    if (needs_slash) {
        out[cursor++] = '/';
    }
    std::memcpy(out.data() + cursor, name.data(), n_len);
    out[total] = '\0';
    return true;
}

void add_entry(std::string_view name, std::string_view path, EntryKind kind, std::uint64_t size) {
    if (entry_count >= max_cached_entries) return;
    auto& e = entries[entry_count++];
    const std::size_t n_len = name.size() < mm::fs::max_name ? name.size() : mm::fs::max_name;
    std::memcpy(e.name.data(), name.data(), n_len);
    e.name[n_len] = '\0';

    const std::size_t p_len = path.size() < mm::fs::max_path ? path.size() : mm::fs::max_path;
    std::memcpy(e.path.data(), path.data(), p_len);
    e.path[p_len] = '\0';

    e.kind = kind;
    e.size = size;
}

void seed_bundled_games_if_needed() {
    // Seed bundled games from games/ folder into LittleFS /games
    for (const auto& rom : bundled_roms) {
        std::array<char, mm::fs::max_path + 1> path{};
        const std::string_view prefix = "/games/";
        if (prefix.size() + rom.filename.size() > mm::fs::max_path) continue;
        std::memcpy(path.data(), prefix.data(), prefix.size());
        std::memcpy(path.data() + prefix.size(), rom.filename.data(), rom.filename.size());
        path[prefix.size() + rom.filename.size()] = '\0';

        mm::fs::File file;
        auto status = mm::fs::open(path.data(), mm::fs::Access::Write,
                                   mm::fs::Disposition::CreateNew, file);
        if (status == mm::fs::Status::Ok) {
            std::size_t written = 0;
            (void)file.write(std::as_bytes(rom.data), written);
            (void)file.close();
        }
    }
}

}  // namespace

mm::fs::Status initialize_storage() {
    // 1. Mount internal littlefs storage at /games (formatting if blank, exactly like FileCommander)
    if (!littlefs_mounted) {
        const auto status = mm::fs::local::mount("/games", {false, true});
        if (status == mm::fs::Status::Ok || status == mm::fs::Status::Exists) {
            littlefs_mounted = true;
            seed_bundled_games_if_needed();
        }
    }

    // 2. Try mounting external SD card FAT storage at /sd
    if (!sd_mounted) {
        auto& card = mm::sdcard::socket::card();
        const auto status = mm::fs::fat::mount("/sd", card, { .read_only = true });
        if (status == mm::fs::Status::Ok || status == mm::fs::Status::Exists) {
            sd_mounted = true;
        }
    }

    // Default to /games
    current_path = { '/', 'g', 'a', 'm', 'e', 's', '\0' };
    return refresh_directory();
}

void shutdown_storage() {
    if (littlefs_mounted) {
        (void)mm::fs::local::unmount("/games");
        littlefs_mounted = false;
    }
    if (sd_mounted) {
        (void)mm::fs::fat::unmount("/sd");
        sd_mounted = false;
    }
}

bool is_littlefs_mounted() {
    return littlefs_mounted;
}

bool is_sd_mounted() {
    return sd_mounted;
}

StorageSource current_source() {
    if (std::strncmp(current_path.data(), "/sd", 3) == 0) {
        return StorageSource::SdCard;
    }
    return StorageSource::LittleFs;
}

const char* storage_source_name() {
    return current_source() == StorageSource::LittleFs ? "LittleFS Flash" : "SD Card FAT";
}

mm::fs::Status switch_source() {
    if (current_source() == StorageSource::LittleFs) {
        // Try switching to /sd
        if (!sd_mounted) {
            auto& card = mm::sdcard::socket::card();
            const auto status = mm::fs::fat::mount("/sd", card, { .read_only = true });
            if (status == mm::fs::Status::Ok || status == mm::fs::Status::Exists) {
                sd_mounted = true;
            } else {
                return status;
            }
        }
        current_path = { '/', 's', 'd', '\0' };
    } else {
        // Switch back to /games
        current_path = { '/', 'g', 'a', 'm', 'e', 's', '\0' };
    }
    return refresh_directory();
}

const char* current_directory() {
    return current_path.data();
}

mm::fs::Status refresh_directory() {
    entry_count = 0;
    page_index = 0;

    const bool at_games_root = (std::strcmp(current_path.data(), "/games") == 0);
    const bool at_sd_root = (std::strcmp(current_path.data(), "/sd") == 0);
    const bool at_root = (at_games_root || at_sd_root);

    if (!at_root) {
        add_entry(".. (Parent Directory)", "", EntryKind::Parent, 0);
    }

    mm::fs::Directory dir;
    auto status = mm::fs::open_directory(current_path.data(), dir);
    if (status != mm::fs::Status::Ok) {
        if (entry_count == 0) {
            add_entry("★ CATCH THE DOT (DEMO)", "", EntryKind::DemoGame, rom_catch_the_dot_ch8.size());
        }
        return status;
    }

    std::array<char, mm::fs::max_name + 1> filename{};
    std::size_t length = 0;
    mm::fs::Stat stat{};
    bool done = false;

    // Scan directories first
    while (true) {
        status = dir.next(filename, length, stat, done);
        if (status != mm::fs::Status::Ok || done) break;
        if (length == 0 || filename[0] == '.') continue;
        if (stat.kind != mm::fs::Kind::Directory) continue;

        std::array<char, mm::fs::max_path + 1> child_path{};
        if (join_path(current_path.data(), std::string_view{filename.data(), length}, child_path)) {
            add_entry(std::string_view{filename.data(), length},
                      std::string_view{child_path.data(), std::strlen(child_path.data())},
                      EntryKind::Directory, 0);
        }
        if (entry_count >= max_cached_entries) break;
    }
    (void)dir.close();

    // Reopen directory to scan files
    if (entry_count < max_cached_entries) {
        status = mm::fs::open_directory(current_path.data(), dir);
        if (status == mm::fs::Status::Ok) {
            done = false;
            while (true) {
                status = dir.next(filename, length, stat, done);
                if (status != mm::fs::Status::Ok || done) break;
                if (length == 0 || filename[0] == '.') continue;
                if (stat.kind == mm::fs::Kind::Directory) continue;

                std::string_view name_sv{filename.data(), length};
                EntryKind kind = is_chip8_filename(name_sv) ? EntryKind::Chip8Game : EntryKind::OtherFile;

                std::array<char, mm::fs::max_path + 1> child_path{};
                if (join_path(current_path.data(), name_sv, child_path)) {
                    add_entry(name_sv,
                              std::string_view{child_path.data(), std::strlen(child_path.data())},
                              kind, stat.size);
                }
                if (entry_count >= max_cached_entries) break;
            }
            (void)dir.close();
        }
    }

    if (entry_count == 0) {
        add_entry("★ CATCH THE DOT (DEMO)", "", EntryKind::DemoGame, rom_catch_the_dot_ch8.size());
    }

    return mm::fs::Status::Ok;
}

mm::fs::Status navigate_to(unsigned int entry_index) {
    if (entry_index >= entry_count) return mm::fs::Status::BadArgument;
    const auto& entry = entries[entry_index];
    if (entry.kind == EntryKind::Parent) {
        return navigate_up();
    }
    if (entry.kind != EntryKind::Directory) {
        return mm::fs::Status::NotDirectory;
    }

    std::size_t len = std::strlen(entry.path.data());
    if (len > mm::fs::max_path) return mm::fs::Status::NameTooLong;
    std::memcpy(current_path.data(), entry.path.data(), len + 1);
    return refresh_directory();
}

mm::fs::Status navigate_up() {
    const bool at_games_root = (std::strcmp(current_path.data(), "/games") == 0);
    const bool at_sd_root = (std::strcmp(current_path.data(), "/sd") == 0);
    if (at_games_root || at_sd_root) return mm::fs::Status::BadArgument;

    char* last_slash = std::strrchr(current_path.data(), '/');
    if (last_slash == nullptr || last_slash <= current_path.data()) {
        current_path = { '/', 'g', 'a', 'm', 'e', 's', '\0' };
    } else {
        *last_slash = '\0';
    }
    return refresh_directory();
}

unsigned int page_size() {
    return g_page_size;
}

void set_page_size(unsigned int count) {
    g_page_size = count > 0 ? count : 1;
}

unsigned int total_entries() {
    return entry_count;
}

unsigned int total_pages() {
    if (entry_count == 0) return 1;
    return (entry_count + g_page_size - 1) / g_page_size;
}

unsigned int current_page() {
    return page_index;
}

void set_page(unsigned int page) {
    if (page < total_pages()) page_index = page;
}

bool next_page() {
    if (page_index + 1 < total_pages()) {
        ++page_index;
        return true;
    }
    return false;
}

bool prev_page() {
    if (page_index > 0) {
        --page_index;
        return true;
    }
    return false;
}

const Entry* get_page_entry(unsigned int index_on_page) {
    const unsigned int idx = page_index * g_page_size + index_on_page;
    if (idx < entry_count) {
        return &entries[idx];
    }
    return nullptr;
}

int find_entry_index(unsigned int index_on_page) {
    const unsigned int idx = page_index * g_page_size + index_on_page;
    return idx < entry_count ? static_cast<int>(idx) : -1;
}

mm::fs::Status load_game_rom(const Entry& entry,
                             std::span<std::uint8_t> buffer,
                             std::size_t& bytes_loaded) {
    bytes_loaded = 0;
    if (entry.kind == EntryKind::DemoGame) {
        if (rom_catch_the_dot_ch8.size() > buffer.size()) return mm::fs::Status::NoSpace;
        for (std::size_t i = 0; i < rom_catch_the_dot_ch8.size(); ++i) {
            buffer[i] = rom_catch_the_dot_ch8[i];
        }
        bytes_loaded = rom_catch_the_dot_ch8.size();
        return mm::fs::Status::Ok;
    }

    if (entry.path[0] == '\0') return mm::fs::Status::NotFound;

    mm::fs::File file;
    auto status = mm::fs::open(entry.path.data(), mm::fs::Access::Read,
                               mm::fs::Disposition::OpenExisting, file);
    if (status != mm::fs::Status::Ok) return status;

    status = file.read(std::as_writable_bytes(buffer), bytes_loaded);
    const auto close_status = file.close();
    if (status != mm::fs::Status::Ok) return status;
    if (close_status != mm::fs::Status::Ok) return close_status;
    if (bytes_loaded == 0) return mm::fs::Status::Corrupt;

    return mm::fs::Status::Ok;
}

}  // namespace funcommander
