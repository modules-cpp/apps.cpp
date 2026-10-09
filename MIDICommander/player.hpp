#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace midicommander {

struct MidiInfo {
    std::array<char, 64> title{};
    unsigned int format = 0;          // SMF format (0 or 1)
    unsigned int tracks = 0;          // Number of tracks
    unsigned int division = 0;        // Ticks per quarter note (PPQ)
    std::uint32_t tempo_us = 500'000; // Microseconds per quarter note
    unsigned int bpm = 120;           // Beats per minute
    unsigned int duration_s = 0;      // Total duration in seconds
    unsigned int note_count = 0;      // Total note-on events
    std::size_t file_size = 0;        // File size in bytes
    bool valid = false;
};

const char* error();
bool active();
bool initialize();
bool load(std::string_view path);
bool start();
void stop();
bool service();
const MidiInfo& current_info();
void set_volume(unsigned int percent);
unsigned int volume();

} // namespace midicommander
