#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace chip8 {

constexpr std::size_t max_rom_size = 4096 - 0x200;  // 3584 bytes

// Initializes/loads a CHIP-8 ROM into memory at 0x200 and resets registers/PC/stack.
bool load(std::span<const std::uint8_t> rom);

// Sets key state (0x0 to 0xF) to pressed (true) or released (false).
void key(unsigned int number, bool down);

// Query pixel at (x, y) on 64x32 screen.
bool pixel(unsigned int x, unsigned int y);

// Has screen changed since last acknowledge_frame?
bool dirty();
void acknowledge_frame();

// Timers.
std::uint8_t delay_timer();
std::uint8_t sound_timer();

// Registers / PC inspection.
std::uint8_t reg(unsigned int number);
std::uint16_t program_counter();

// Cycle stepping.
void tick();  // 60 Hz timer tick
bool step();  // Execute one opcode

}  // namespace chip8
