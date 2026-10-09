// Clockal USB time-setting protocol. No board SDK headers are needed here.
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

import mm.rtc;
import mm.stdio;

namespace {
constexpr std::string_view hello = "TDSET 1 HELLO";
constexpr std::string_view command = "TDSET 1 ";
constexpr std::string_view ready = "TDSET 1 READY\n";
constexpr std::string_view ok = "TDSET 1 OK\n";
constexpr std::string_view bad_time = "TDSET 1 BAD TIME\n";
constexpr std::string_view failed = "TDSET 1 ERROR\n";

struct Receiver {
    std::array<char, 48> line{};
    std::size_t size = 0;
    bool overflow = false;
};

void reply(mm::stdio::Console& console, std::string_view message) {
    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(message.data()), message.size()};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        if (console.write(bytes.subspan(offset), written) != mm::stdio::Status::Ok ||
            written == 0 || written > bytes.size() - offset) return;
        offset += written;
    }
    (void)console.flush();
}

bool digits(std::string_view input, unsigned int& value) {
    if (input.empty()) return false;
    value = 0;
    for (const char ch : input) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10u + static_cast<unsigned int>(ch - '0');
    }
    return true;
}

bool leap_year(unsigned int year) {
    return year % 4u == 0 && (year % 100u != 0 || year % 400u == 0);
}

bool parse_time(std::string_view input, mm::rtc::DateTime& time) {
    if (input.size() != 20 || input[4] != '-' || input[7] != '-' ||
        input[10] != 'T' || input[13] != ':' || input[16] != ':' ||
        input[19] != 'Z' ||
        !digits(input.substr(0, 4), time.year) ||
        !digits(input.substr(5, 2), time.month) ||
        !digits(input.substr(8, 2), time.day) ||
        !digits(input.substr(11, 2), time.hour) ||
        !digits(input.substr(14, 2), time.minute) ||
        !digits(input.substr(17, 2), time.second)) return false;

    if (time.year < 2000 || time.year > 2099 || time.month < 1 || time.month > 12 ||
        time.hour > 23 || time.minute > 59 || time.second > 59) return false;
    constexpr std::array<unsigned int, 12> month_days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned int last_day = month_days[time.month - 1u];
    if (time.month == 2 && leap_year(time.year)) ++last_day;
    if (time.day < 1 || time.day > last_day) return false;

    // Both the hardware RTC and software clock use Sunday=0.
    constexpr std::array<unsigned int, 12> offsets{
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    const unsigned int year = time.year - (time.month < 3u ? 1u : 0u);
    time.weekday = (year + year / 4u - year / 100u + year / 400u +
                    offsets[time.month - 1u] + time.day) % 7u;
    return true;
}

bool process(mm::rtc::Clock& clock, mm::stdio::Console& console,
             std::string_view line) {
    if (line == hello) {
        reply(console, ready);
        return false;
    }
    if (!line.starts_with(command)) return false;
    mm::rtc::DateTime time{};
    if (!parse_time(line.substr(command.size()), time)) {
        reply(console, bad_time);
        return false;
    }
    if (clock.initialize() != mm::rtc::Status::Ok ||
        clock.write(time) != mm::rtc::Status::Ok) {
        reply(console, failed);
        return false;
    }
    reply(console, ok);
    return true;
}
}  // namespace

bool tdset_poll(mm::rtc::Clock& clock, mm::stdio::Console& console) {
    static Receiver receiver{};
    std::array<std::byte, 32> bytes{};
    std::size_t count = 0;
    if (console.read(bytes, count) != mm::stdio::Status::Ok || count > bytes.size())
        return false;
    bool updated = false;
    for (std::size_t index = 0; index < count; ++index) {
        const char ch = static_cast<char>(bytes[index]);
        if (ch == '\n') {
            if (!receiver.overflow) {
                updated = process(clock, console,
                                  {receiver.line.data(), receiver.size}) || updated;
            }
            receiver.size = 0;
            receiver.overflow = false;
        } else if (ch != '\r' && !receiver.overflow) {
            if (receiver.size < receiver.line.size())
                receiver.line[receiver.size++] = ch;
            else
                receiver.overflow = true;
        }
    }
    return updated;
}
