#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import mm.mcu;
import mm.stdio;

namespace {

constexpr std::size_t channel_count = 16;
constexpr unsigned int first_gpio = 0;
constexpr std::uint64_t frame_period_ns = 20'000'000;
constexpr unsigned int minimum_pulse_us = 1000;
constexpr unsigned int maximum_pulse_us = 2000;
constexpr unsigned int center_pulse_us = 1500;

struct Channel {
    unsigned int output = 0;
    unsigned int pulse_us = center_pulse_us;
    bool available = false;
    bool enabled = false;
};

std::array<Channel, channel_count> channels{};

bool write_all(mm::stdio::Console& console, std::string_view text) {
    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()), text.size()};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        if (console.write(bytes.subspan(offset), written) != mm::stdio::Status::Ok ||
            written == 0 || written > bytes.size() - offset) return false;
        offset += written;
    }
    return console.flush() == mm::stdio::Status::Ok;
}

void number_text(unsigned int value, char* output, std::size_t capacity,
                 std::size_t& size) {
    const auto result = std::to_chars(output, output + capacity, value);
    size = result.ec == std::errc{} ? static_cast<std::size_t>(result.ptr - output) : 0;
}

bool write_number(mm::stdio::Console& console, unsigned int value) {
    std::array<char, 16> buffer{};
    std::size_t size = 0;
    number_text(value, buffer.data(), buffer.size(), size);
    return size != 0 && write_all(console, {buffer.data(), size});
}

bool parse_unsigned(std::string_view text, unsigned int& value) {
    if (text.empty()) return false;
    unsigned int parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const unsigned int digit = static_cast<unsigned int>(ch - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

struct Words {
    std::array<std::string_view, 3> value{};
    std::size_t count = 0;
    bool too_many = false;
};

Words split_words(std::string_view line) {
    Words words{};
    std::size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() &&
               (line[position] == ' ' || line[position] == '\t')) ++position;
        if (position == line.size()) break;
        const auto start = position;
        while (position < line.size() && line[position] != ' ' &&
               line[position] != '\t') ++position;
        if (words.count == words.value.size()) {
            words.too_many = true;
            return words;
        }
        words.value[words.count++] = line.substr(start, position - start);
    }
    return words;
}

bool channel_index(std::string_view text, std::size_t& index) {
    unsigned int parsed = 0;
    if (!parse_unsigned(text, parsed) || parsed >= channel_count) return false;
    index = parsed;
    return true;
}

bool pulse_value(std::string_view text, unsigned int& pulse) {
    return parse_unsigned(text, pulse) && pulse >= minimum_pulse_us &&
           pulse <= maximum_pulse_us;
}

bool set_channel(std::size_t index, unsigned int pulse_us) {
    auto& channel = channels[index];
    if (!channel.available) return false;
    if (mm::mcu::pwm_configure(channel.output, frame_period_ns) != mm::mcu::Status::Ok)
        return false;
    if (mm::mcu::pwm_write(channel.output,
            static_cast<std::uint64_t>(pulse_us) * 1000u) != mm::mcu::Status::Ok)
        return false;
    channel.pulse_us = pulse_us;
    channel.enabled = true;
    return true;
}

bool disable_channel(std::size_t index) {
    auto& channel = channels[index];
    if (!channel.available || !channel.enabled) return true;
    if (mm::mcu::pwm_release(channel.output) != mm::mcu::Status::Ok) return false;
    channel.enabled = false;
    return true;
}

bool show_channel(mm::stdio::Console& console, std::size_t index) {
    const auto& channel = channels[index];
    if (!write_all(console, "CH ") || !write_number(console, static_cast<unsigned int>(index)) ||
        !write_all(console, " GP") || !write_number(console, first_gpio + static_cast<unsigned int>(index)))
        return false;
    if (!channel.available) return write_all(console, " unavailable\r\n");
    if (!write_all(console, channel.enabled ? " ": " off ")) return false;
    if (channel.enabled && (!write_number(console, channel.pulse_us) ||
                            !write_all(console, " us"))) return false;
    return write_all(console, "\r\n");
}

bool help(mm::stdio::Console& console) {
    return write_all(console,
        "ServoCommander - 16-channel RC servo console\r\n"
        "  help                  show commands\r\n"
        "  list                  show all channels and commanded pulses\r\n"
        "  get <ch>              read back commanded pulse width\r\n"
        "  set <ch> <us>         set pulse (channel 0-15, 1000-2000 us)\r\n"
        "  all <us>              set every channel to the pulse width\r\n"
        "  off <ch|all>          stop PWM output\r\n"
        "  quit                  leave outputs unchanged and exit\r\n"
        "Readback is the last command sent; ordinary RC servos provide no position feedback.\r\n");
}

bool process(mm::stdio::Console& console, std::string_view line, bool& quit) {
    const auto words = split_words(line);
    if (words.count == 0) return true;
    if (words.too_many) return write_all(console, "ERR too many arguments\r\n");
    const auto command = words.value[0];
    if ((command == "help" || command == "?") && words.count == 1)
        return help(console);
    if (command == "quit" && words.count == 1) {
        quit = true;
        return write_all(console, "Bye. Servo outputs remain at their last settings.\r\n");
    }
    if (command == "list" && words.count == 1) {
        for (std::size_t i = 0; i < channel_count; ++i)
            if (!show_channel(console, i)) return false;
        return true;
    }
    if (command == "get" && words.count == 2) {
        std::size_t index = 0;
        if (!channel_index(words.value[1], index))
            return write_all(console, "ERR channel must be 0-15\r\n");
        const auto& channel = channels[index];
        if (!channel.available) return write_all(console, "ERR channel unavailable\r\n");
        if (!channel.enabled) return write_all(console, "CH ") &&
            write_number(console, static_cast<unsigned int>(index)) &&
            write_all(console, " OFF\r\n");
        return write_all(console, "CH ") &&
            write_number(console, static_cast<unsigned int>(index)) &&
            write_all(console, " ") && write_number(console, channel.pulse_us) &&
            write_all(console, " us (commanded)\r\n");
    }
    if (command == "set" && words.count == 3) {
        std::size_t index = 0;
        unsigned int pulse = 0;
        if (!channel_index(words.value[1], index))
            return write_all(console, "ERR channel must be 0-15\r\n");
        if (!pulse_value(words.value[2], pulse))
            return write_all(console, "ERR pulse must be 1000-2000 microseconds\r\n");
        if (!set_channel(index, pulse)) return write_all(console, "ERR PWM update failed\r\n");
        return write_all(console, "OK CH ") &&
            write_number(console, static_cast<unsigned int>(index)) &&
            write_all(console, " ") && write_number(console, pulse) &&
            write_all(console, " us\r\n");
    }
    if (command == "all" && words.count == 2) {
        unsigned int pulse = 0;
        if (!pulse_value(words.value[1], pulse))
            return write_all(console, "ERR pulse must be 1000-2000 microseconds\r\n");
        for (std::size_t index = 0; index < channel_count; ++index) {
            if (!set_channel(index, pulse)) {
                return write_all(console, "ERR PWM update failed at channel ") &&
                    write_number(console, static_cast<unsigned int>(index)) &&
                    write_all(console, "\r\n");
            }
        }
        return write_all(console, "OK all channels ") && write_number(console, pulse) &&
               write_all(console, " us\r\n");
    }
    if (command == "off" && words.count == 2) {
        if (words.value[1] == "all") {
            for (std::size_t index = 0; index < channel_count; ++index)
                if (!disable_channel(index)) return write_all(console, "ERR PWM release failed\r\n");
            return write_all(console, "OK all channels off\r\n");
        }
        std::size_t index = 0;
        if (!channel_index(words.value[1], index))
            return write_all(console, "ERR channel must be 0-15 or all\r\n");
        if (!disable_channel(index)) return write_all(console, "ERR PWM release failed\r\n");
        return write_all(console, "OK CH ") &&
            write_number(console, static_cast<unsigned int>(index)) &&
            write_all(console, " off\r\n");
    }
    return write_all(console, "ERR unknown command or wrong arguments; type help\r\n");
}

}  // namespace

int main() {
    auto& console = mm::stdio::selected_console();
    if (console.initialize() != mm::stdio::Status::Ok) return 1;

    for (std::size_t index = 0; index < channel_count; ++index) {
        auto& channel = channels[index];
        if (mm::mcu::pwm_output_for_gpio(first_gpio + static_cast<unsigned int>(index),
                                         channel.output) == mm::mcu::Status::Ok) {
            channel.available = true;
            if (!set_channel(index, center_pulse_us)) channel.available = false;
        }
    }

    if (!write_all(console, "\r\nServoCommander ready. Type help.\r\n> ")) return 1;
    std::array<char, 96> line{};
    std::size_t length = 0;
    bool overflow = false;
    bool quit = false;
    std::array<std::byte, 32> input{};

    while (!quit) {
        std::size_t count = 0;
        if (console.read(input, count) != mm::stdio::Status::Ok || count > input.size())
            return 1;
        if (count == 0) {
            if (mm::mcu::delay_ms(2) != mm::mcu::Status::Ok) return 1;
            continue;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const char ch = static_cast<char>(input[i]);
            if (ch == '\r' || ch == '\n') {
                if (!write_all(console, "\r\n")) return 1;
                if (overflow) {
                    if (!write_all(console, "ERR line too long\r\n")) return 1;
                } else if (!process(console, {line.data(), length}, quit)) return 1;
                length = 0;
                overflow = false;
                if (!quit && !write_all(console, "> ")) return 1;
            } else if (ch == '\b' || ch == 0x7f) {
                if (length != 0 && !overflow) {
                    --length;
                    if (!write_all(console, "\b \b")) return 1;
                }
            } else if (ch >= 0x20 && ch <= 0x7e) {
                if (length < line.size()) line[length++] = ch;
                else overflow = true;
                if (!write_all(console, {&ch, 1})) return 1;
            }
        }
    }
    return 0;
}
