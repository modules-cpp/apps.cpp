#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace chip8 {
namespace {

constexpr std::uint16_t program_start = 0x200;
constexpr std::uint16_t font_start = 0x050;
constexpr std::array<std::uint8_t, 80> font{{
    0xF0,0x90,0x90,0x90,0xF0, 0x20,0x60,0x20,0x20,0x70,
    0xF0,0x10,0xF0,0x80,0xF0, 0xF0,0x10,0xF0,0x10,0xF0,
    0x90,0x90,0xF0,0x10,0x10, 0xF0,0x80,0xF0,0x10,0xF0,
    0xF0,0x80,0xF0,0x90,0xF0, 0xF0,0x10,0x20,0x40,0x40,
    0xF0,0x90,0xF0,0x90,0xF0, 0xF0,0x90,0xF0,0x10,0xF0,
    0xF0,0x90,0xF0,0x90,0x90, 0xE0,0x90,0xE0,0x90,0xE0,
    0xF0,0x80,0x80,0x80,0xF0, 0xE0,0x90,0x90,0x90,0xE0,
    0xF0,0x80,0xF0,0x80,0xF0, 0xF0,0x80,0xF0,0x80,0x80,
}};

struct Machine {
    std::array<std::uint8_t, 4096> memory{};
    std::array<std::uint8_t, 16> registers{};
    std::array<std::uint16_t, 16> stack{};
    std::array<std::uint8_t, 64 * 32> screen{};
    std::array<bool, 16> keys{};
    std::uint16_t index = 0;
    std::uint16_t pc = program_start;
    unsigned int stack_size = 0;
    std::uint8_t delay = 0;
    std::uint8_t sound = 0;
    std::uint32_t random = 0xC0FFEE12u;
    bool changed = true;
};

Machine machine;

bool address(std::uint16_t candidate) {
    if (candidate >= machine.memory.size()) return false;
    machine.pc = candidate;
    return true;
}

bool range(unsigned int first, unsigned int count) {
    return first < machine.memory.size() &&
           count <= machine.memory.size() - first;
}

void draw(std::uint8_t x, std::uint8_t y, unsigned int rows) {
    machine.registers[15] = 0;
    for (unsigned int row = 0; row < rows; ++row) {
        const auto sprite = machine.memory[machine.index + row];
        for (unsigned int column = 0; column < 8; ++column) {
            if ((sprite & (0x80u >> column)) == 0) continue;
            const unsigned int px = (static_cast<unsigned int>(x) + column) % 64u;
            const unsigned int py = (static_cast<unsigned int>(y) + row) % 32u;
            auto& pixel = machine.screen[py * 64u + px];
            if (pixel != 0) machine.registers[15] = 1;
            pixel ^= 1u;
        }
    }
    machine.changed = true;
}

}  // namespace

bool load(std::span<const std::uint8_t> rom) {
    if (rom.empty() || rom.size() > machine.memory.size() - program_start)
        return false;
    machine = Machine{};
    for (std::size_t i = 0; i < font.size(); ++i)
        machine.memory[font_start + i] = font[i];
    for (std::size_t i = 0; i < rom.size(); ++i)
        machine.memory[program_start + i] = rom[i];
    return true;
}

void key(unsigned int number, bool down) {
    if (number < machine.keys.size()) machine.keys[number] = down;
}

bool pixel(unsigned int x, unsigned int y) {
    return x < 64 && y < 32 && machine.screen[y * 64u + x] != 0;
}

bool dirty() { return machine.changed; }
void acknowledge_frame() { machine.changed = false; }
std::uint8_t delay_timer() { return machine.delay; }
std::uint8_t sound_timer() { return machine.sound; }
std::uint8_t reg(unsigned int number) {
    return number < machine.registers.size() ? machine.registers[number] : 0;
}
std::uint16_t program_counter() { return machine.pc; }

void tick() {
    if (machine.delay != 0) --machine.delay;
    if (machine.sound != 0) --machine.sound;
}

bool step() {
    if (!range(machine.pc, 2)) return false;
    const auto opcode = static_cast<std::uint16_t>(
        (static_cast<unsigned int>(machine.memory[machine.pc]) << 8u) |
        machine.memory[machine.pc + 1u]);
    machine.pc += 2;
    const unsigned int x = (opcode >> 8u) & 0x0Fu;
    const unsigned int y = (opcode >> 4u) & 0x0Fu;
    const unsigned int n = opcode & 0x0Fu;
    const std::uint8_t byte = static_cast<std::uint8_t>(opcode);
    const std::uint16_t target = opcode & 0x0FFFu;
    auto& vx = machine.registers[x];
    const auto vy = machine.registers[y];

    switch (opcode & 0xF000u) {
        case 0x0000u:
            if (opcode == 0x00E0u) {
                machine.screen.fill(0);
                machine.changed = true;
                return true;
            }
            if (opcode == 0x00EEu) {
                if (machine.stack_size == 0) return false;
                return address(machine.stack[--machine.stack_size]);
            }
            return false;
        case 0x1000u: return address(target);
        case 0x2000u:
            if (machine.stack_size == machine.stack.size() ||
                target >= machine.memory.size()) return false;
            machine.stack[machine.stack_size++] = machine.pc;
            return address(target);
        case 0x3000u: if (vx == byte) machine.pc += 2; return true;
        case 0x4000u: if (vx != byte) machine.pc += 2; return true;
        case 0x5000u:
            if (n != 0) return false;
            if (vx == vy) machine.pc += 2;
            return true;
        case 0x6000u: vx = byte; return true;
        case 0x7000u: vx = static_cast<std::uint8_t>(vx + byte); return true;
        case 0x8000u: {
            const auto original = vx;
            switch (n) {
                case 0x0: vx = vy; return true;
                case 0x1: vx |= vy; return true;
                case 0x2: vx &= vy; return true;
                case 0x3: vx ^= vy; return true;
                case 0x4: {
                    const unsigned int sum = original + vy;
                    vx = static_cast<std::uint8_t>(sum);
                    machine.registers[15] = sum > 255u;
                    return true;
                }
                case 0x5:
                    vx = static_cast<std::uint8_t>(original - vy);
                    machine.registers[15] = original >= vy;
                    return true;
                case 0x6:
                    vx = original >> 1u;
                    machine.registers[15] = original & 1u;
                    return true;
                case 0x7:
                    vx = static_cast<std::uint8_t>(vy - original);
                    machine.registers[15] = vy >= original;
                    return true;
                case 0xE:
                    vx = static_cast<std::uint8_t>(original << 1u);
                    machine.registers[15] = (original >> 7u) & 1u;
                    return true;
                default: return false;
            }
        }
        case 0x9000u:
            if (n != 0) return false;
            if (vx != vy) machine.pc += 2;
            return true;
        case 0xA000u: machine.index = target; return true;
        case 0xB000u: return address(static_cast<std::uint16_t>(
            target + machine.registers[0]));
        case 0xC000u:
            machine.random ^= machine.random << 13u;
            machine.random ^= machine.random >> 17u;
            machine.random ^= machine.random << 5u;
            vx = static_cast<std::uint8_t>(machine.random) & byte;
            return true;
        case 0xD000u:
            if (!range(machine.index, n)) return false;
            draw(vx, vy, n);
            return true;
        case 0xE000u:
            if (vx >= machine.keys.size()) return false;
            if (byte == 0x9Eu) {
                if (machine.keys[vx]) machine.pc += 2;
                return true;
            }
            if (byte == 0xA1u) {
                if (!machine.keys[vx]) machine.pc += 2;
                return true;
            }
            return false;
        case 0xF000u:
            switch (byte) {
                case 0x07: vx = machine.delay; return true;
                case 0x0A:
                    for (unsigned int pressed = 0; pressed < 16; ++pressed)
                        if (machine.keys[pressed]) {
                            vx = static_cast<std::uint8_t>(pressed);
                            return true;
                        }
                    machine.pc -= 2;
                    return true;
                case 0x15: machine.delay = vx; return true;
                case 0x18: machine.sound = vx; return true;
                case 0x1E: machine.index = static_cast<std::uint16_t>(
                    machine.index + vx); return true;
                case 0x29: machine.index = static_cast<std::uint16_t>(
                    font_start + (vx & 0x0Fu) * 5u); return true;
                case 0x33:
                    if (!range(machine.index, 3)) return false;
                    machine.memory[machine.index] = vx / 100u;
                    machine.memory[machine.index + 1u] = (vx / 10u) % 10u;
                    machine.memory[machine.index + 2u] = vx % 10u;
                    return true;
                case 0x55:
                    if (!range(machine.index, x + 1u)) return false;
                    for (unsigned int i = 0; i <= x; ++i)
                        machine.memory[machine.index + i] = machine.registers[i];
                    return true;
                case 0x65:
                    if (!range(machine.index, x + 1u)) return false;
                    for (unsigned int i = 0; i <= x; ++i)
                        machine.registers[i] = machine.memory[machine.index + i];
                    return true;
                default: return false;
            }
        default: return false;
    }
}

}  // namespace chip8
