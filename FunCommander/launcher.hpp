#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import mm.fs;

namespace funcommander {

enum class StorageSource {
    LittleFs,   // Internal flash littlefs (/games)
    SdCard      // External SD card FAT (/sd)
};

enum class EntryKind {
    Parent,
    Directory,
    Chip8Game,
    DemoGame,
    OtherFile
};

struct Entry {
    std::array<char, mm::fs::max_name + 1> name{};
    std::array<char, mm::fs::max_path + 1> path{};
    EntryKind kind = EntryKind::OtherFile;
    std::uint64_t size = 0;
};

// Maximum entries shown per page.
constexpr unsigned int default_page_size = 7;
unsigned int page_size();
void set_page_size(unsigned int count);
// Maximum entries cached in directory list.
constexpr unsigned int max_cached_entries = 32;

// Storage lifecycle (LittleFS on /games and FAT on /sd).
mm::fs::Status initialize_storage();
void shutdown_storage();
bool is_littlefs_mounted();
bool is_sd_mounted();

// Path and navigation.
const char* current_directory();
StorageSource current_source();
const char* storage_source_name();
mm::fs::Status switch_source();  // Toggles between /games (LittleFS) and /sd (SD Card)
mm::fs::Status navigate_to(unsigned int entry_index);
mm::fs::Status navigate_up();

// Directory listing & pagination.
mm::fs::Status refresh_directory();
unsigned int total_entries();
unsigned int total_pages();
unsigned int current_page();
void set_page(unsigned int page);
bool next_page();
bool prev_page();
const Entry* get_page_entry(unsigned int index_on_page);
int find_entry_index(unsigned int index_on_page);

// File loading.
mm::fs::Status load_game_rom(const Entry& entry,
                             std::span<std::uint8_t> buffer,
                             std::size_t& bytes_loaded);

}  // namespace funcommander
