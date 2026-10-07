#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace chip8 {
namespace {

// "Catch the Dot" ROM. CHIP-8 key 4 moves left; key 6 moves right.
// The score is one hexadecimal digit in the upper left. 
// Missing a dot resets it.
struct Rom {
    std::array<std::uint8_t, 128> bytes{};
    std::size_t used = 0;

    constexpr std::size_t emit(std::uint16_t instruction) {
        const auto at = used;
        bytes[used++] = static_cast<std::uint8_t>(instruction >> 8u);
        bytes[used++] = static_cast<std::uint8_t>(instruction);
        return at;
    }

    constexpr void patch(std::size_t at, std::uint16_t instruction) {
        bytes[at] = static_cast<std::uint8_t>(instruction >> 8u);
        bytes[at + 1u] = static_cast<std::uint8_t>(instruction);
    }

    [[nodiscard]] static constexpr std::uint16_t address(std::size_t at) {
        return static_cast<std::uint16_t>(0x200u + at);
    }
};

constexpr Rom make_game() {
    Rom rom;
    rom.emit(0x601E);  // V0: paddle x = 30
    rom.emit(0xC13F);  // V1: random dot x
    rom.emit(0x6200);  // V2: dot y
    rom.emit(0x6300);  // V3: score
    rom.emit(0x6404);  // V4: left key
    rom.emit(0x6506);  // V5: right key
    rom.emit(0x6701);  // V7: score x
    rom.emit(0x6801);  // V8: score y
    rom.emit(0x691F);  // V9: bottom row
    rom.emit(0x6A0F);  // VA: score mask

    const auto frame = rom.used;
    rom.emit(0x00E0);  // clear
    rom.emit(0xF329);  // built-in font for score
    rom.emit(0xD785);  // draw score
    const auto paddle_index = rom.emit(0xA000);
    rom.emit(0xD091);  // draw paddle
    const auto dot_index = rom.emit(0xA000);
    rom.emit(0xD121);  // draw dot; VF reports overlap with paddle
    rom.emit(0x321F);  // if dot is not at bottom, jump to timer
    const auto jump_timer = rom.emit(0x1000);
    rom.emit(0x3F01);  // if caught, skip jump to miss
    const auto jump_miss = rom.emit(0x1000);
    rom.emit(0x7301);  // score++
    rom.emit(0x83A2);  // keep score in 0..F
    const auto jump_reset = rom.emit(0x1000);

    const auto miss = rom.used;
    rom.emit(0x6300);  // miss: score = 0
    const auto reset = rom.used;
    rom.emit(0xC13F);  // next random dot
    rom.emit(0x6200);  // dot y = 0

    const auto timer = rom.used;
    rom.emit(0x6603);  // wait three 60 Hz timer ticks
    rom.emit(0xF615);
    const auto wait = rom.used;
    rom.emit(0xF607);
    rom.emit(0x3600);
    rom.emit(0x1000 | Rom::address(wait));

    rom.emit(0xE49E);  // key 4 held?
    const auto jump_right = rom.emit(0x1000);
    rom.emit(0x3000);  // stop at x = 0
    rom.emit(0x70FF);  // paddle left
    const auto right = rom.used;
    rom.emit(0xE59E);  // key 6 held?
    const auto jump_advance = rom.emit(0x1000);
    rom.emit(0x303D);  // stop at x = 61
    rom.emit(0x7001);  // paddle right
    const auto advance = rom.used;
    rom.emit(0x7201);  // dot falls one row
    rom.emit(0x1000 | Rom::address(frame));

    const auto paddle = rom.used;
    rom.bytes[rom.used++] = 0xE0;  // three lit pixels
    const auto dot = rom.used;
    rom.bytes[rom.used++] = 0x80;  // one lit pixel

    rom.patch(paddle_index, 0xA000 | Rom::address(paddle));
    rom.patch(dot_index, 0xA000 | Rom::address(dot));
    rom.patch(jump_timer, 0x1000 | Rom::address(timer));
    rom.patch(jump_miss, 0x1000 | Rom::address(miss));
    rom.patch(jump_reset, 0x1000 | Rom::address(reset));
    rom.patch(jump_right, 0x1000 | Rom::address(right));
    rom.patch(jump_advance, 0x1000 | Rom::address(advance));
    return rom;
}

constexpr auto game = make_game();

}  // namespace

std::span<const std::uint8_t> game_rom() {
    return {game.bytes.data(), game.used};
}

}  // namespace chip8
