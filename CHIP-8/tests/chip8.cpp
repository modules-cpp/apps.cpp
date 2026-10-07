#include <array>
#include <cassert>
#include <cstdint>
#include <span>

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

int main() {
    {
        constexpr std::array<std::uint8_t, 8> arithmetic{{
            0x60, 0xFE, 0x61, 0x03, 0x80, 0x14, 0x70, 0x01
        }};
        assert(chip8::load(arithmetic));
        for (int i = 0; i < 4; ++i) assert(chip8::step());
        assert(chip8::reg(0) == 2);
        assert(chip8::reg(15) == 1);
    }
    {
        constexpr std::array<std::uint8_t, 13> sprite{{
            0x60,0x00, 0x61,0x00, 0xA2,0x0C,
            0xD0,0x11, 0xD0,0x11, 0x12,0x00, 0x80
        }};
        assert(chip8::load(sprite));
        for (int i = 0; i < 4; ++i) assert(chip8::step());
        assert(chip8::pixel(0, 0));
        assert(chip8::step());
        assert(!chip8::pixel(0, 0));
        assert(chip8::reg(15) == 1);
    }
    {
        constexpr std::array<std::uint8_t, 8> timer{{
            0x60,0x02, 0xF0,0x15, 0xF1,0x07, 0xF2,0x0A
        }};
        assert(chip8::load(timer));
        assert(chip8::step() && chip8::step());
        assert(chip8::delay_timer() == 2);
        chip8::tick();
        assert(chip8::step() && chip8::reg(1) == 1);
        assert(chip8::step());
        assert(chip8::program_counter() == 0x206);
        chip8::key(6, true);
        assert(chip8::step());
        assert(chip8::reg(2) == 6);
    }
    {
        constexpr std::array<std::uint8_t, 4> bad_store{{
            0xAF,0xFF, 0xF1,0x55
        }};
        assert(chip8::load(bad_store));
        assert(chip8::step());
        assert(!chip8::step());
    }
    {
        assert(chip8::load(chip8::game_rom()));
        bool drew = false;
        chip8::key(4, true);
        for (int i = 0; i < 4000; ++i) {
            assert(chip8::step());
            if (chip8::dirty()) {
                drew = true;
                chip8::acknowledge_frame();
            }
            if (i % 12 == 0) chip8::tick();
        }
        assert(drew);
        assert(chip8::reg(0) == 0);
        chip8::key(4, false);
        chip8::key(6, true);
        for (int i = 0; i < 4000; ++i) {
            assert(chip8::step());
            if (i % 12 == 0) chip8::tick();
        }
        assert(chip8::reg(0) > 0);
    }
}
