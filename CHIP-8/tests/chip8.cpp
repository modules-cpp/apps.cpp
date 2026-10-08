#include <array>
#include <cstdint>
#include <span>

import mm.test;

namespace chip8 {
bool load(std::span<const std::uint8_t>);
std::span<const std::uint8_t> game_rom();
void key(unsigned int, bool);
bool pixel(unsigned int, unsigned int);
bool dirty();
void acknowledge_frame();
void tick();
bool step();
std::uint8_t delay_timer();
std::uint8_t reg(unsigned int);
std::uint16_t program_counter();
}

namespace {
using mm::test::expect;

void arithmetic() {
    constexpr std::array<std::uint8_t, 8> arithmetic{{
        0x60, 0xFE, 0x61, 0x03, 0x80, 0x14, 0x70, 0x01
    }};
    expect(chip8::load(arithmetic), "chip8::load(arithmetic)");
    for (int i = 0; i < 4; ++i) expect(chip8::step(), "chip8::step()");
    expect(chip8::reg(0) == 2, "chip8::reg(0) == 2");
    expect(chip8::reg(15) == 1, "chip8::reg(15) == 1");
}

void sprites() {
    constexpr std::array<std::uint8_t, 13> sprite{{
        0x60,0x00, 0x61,0x00, 0xA2,0x0C,
        0xD0,0x11, 0xD0,0x11, 0x12,0x00, 0x80
    }};
    expect(chip8::load(sprite), "chip8::load(sprite)");
    for (int i = 0; i < 4; ++i) expect(chip8::step(), "chip8::step()");
    expect(chip8::pixel(0, 0), "chip8::pixel(0, 0)");
    expect(chip8::step(), "chip8::step()");
    expect(!chip8::pixel(0, 0), "!chip8::pixel(0, 0)");
    expect(chip8::reg(15) == 1, "chip8::reg(15) == 1");
}

void timers_and_keys() {
    constexpr std::array<std::uint8_t, 8> timer{{
        0x60,0x02, 0xF0,0x15, 0xF1,0x07, 0xF2,0x0A
    }};
    expect(chip8::load(timer), "chip8::load(timer)");
    expect(chip8::step() && chip8::step(), "chip8::step() && chip8::step()");
    expect(chip8::delay_timer() == 2, "chip8::delay_timer() == 2");
    chip8::tick();
    expect(chip8::step() && chip8::reg(1) == 1, "chip8::step() && chip8::reg(1) == 1");
    expect(chip8::step(), "chip8::step()");
    expect(chip8::program_counter() == 0x206, "chip8::program_counter() == 0x206");
    chip8::key(6, true);
    expect(chip8::step(), "chip8::step()");
    expect(chip8::reg(2) == 6, "chip8::reg(2) == 6");
}

void memory_bounds() {
    constexpr std::array<std::uint8_t, 4> bad_store{{
        0xAF,0xFF, 0xF1,0x55
    }};
    expect(chip8::load(bad_store), "chip8::load(bad_store)");
    expect(chip8::step(), "chip8::step()");
    expect(!chip8::step(), "!chip8::step()");
}

void bundled_game() {
    expect(chip8::load(chip8::game_rom()), "chip8::load(chip8::game_rom())");
    bool drew = false;
    chip8::key(4, true);
    for (int i = 0; i < 4000; ++i) {
        expect(chip8::step(), "chip8::step()");
        if (chip8::dirty()) {
            drew = true;
            chip8::acknowledge_frame();
        }
        if (i % 12 == 0) chip8::tick();
    }
    expect(drew, "drew");
    expect(chip8::reg(0) == 0, "chip8::reg(0) == 0");
    chip8::key(4, false);
    chip8::key(6, true);
    for (int i = 0; i < 4000; ++i) {
        expect(chip8::step(), "chip8::step()");
        if (i % 12 == 0) chip8::tick();
    }
    expect(chip8::reg(0) > 0, "chip8::reg(0) > 0");
}

const mm::test::case_ cases[] = {
    {"arithmetic and carry", &arithmetic},
    {"sprite XOR and collision", &sprites},
    {"timers and key wait", &timers_and_keys},
    {"memory bounds", &memory_bounds},
    {"bundled game", &bundled_game},
};
const mm::test::registrar reg{"chip8", cases};
}
