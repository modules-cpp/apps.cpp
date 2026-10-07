#include <cstddef>
#include <cstdint>

namespace kalkulator {
namespace {

constexpr unsigned int maximum_digits = 8;
constexpr double maximum_result = 99'999'999.5;
char entry[16] = "0";
std::size_t length = 1;
unsigned int digits = 1;
double left = 0.0;
char pending_key = 0;
bool new_entry = false;
bool failed = false;

void reset_entry() {
    entry[0] = '0';
    entry[1] = 0;
    length = 1;
    digits = 1;
}

void error() {
    entry[0] = 'E';
    entry[1] = 'r';
    entry[2] = 'r';
    entry[3] = 'o';
    entry[4] = 'r';
    entry[5] = 0;
    length = 5;
    pending_key = 0;
    new_entry = true;
    failed = true;
}

bool has_dot() {
    for (std::size_t i = 0; i < length; ++i)
        if (entry[i] == '.') return true;
    return false;
}

double parse() {
    double result = 0.0;
    double place = 0.1;
    bool fractional = false;
    for (std::size_t i = 0; i < length; ++i) {
        if (entry[i] == '-') continue;
        if (entry[i] == '.') { fractional = true; continue; }
        const unsigned int digit = static_cast<unsigned int>(entry[i] - '0');
        if (fractional) {
            result += static_cast<double>(digit) * place;
            place *= 0.1;
        } else {
            result = result * 10.0 + static_cast<double>(digit);
        }
    }
    return entry[0] == '-' ? -result : result;
}

bool format(double value) {
    const bool negative = value < 0.0;
    const double magnitude = negative ? -value : value;
    if (!(magnitude <= maximum_result)) return false;

    unsigned int whole_digits = 1;
    double boundary = 10.0;
    while (magnitude >= boundary && whole_digits < maximum_digits + 1) {
        ++whole_digits;
        boundary *= 10.0;
    }
    if (whole_digits > maximum_digits) return false;

    const unsigned int decimals = maximum_digits - whole_digits;
    std::uint64_t scale = 1;
    for (unsigned int i = 0; i < decimals; ++i) scale *= 10;
    const auto units = static_cast<std::uint64_t>(
        magnitude * static_cast<double>(scale) + 0.5);
    std::uint64_t whole = units / scale;
    std::uint64_t fraction = units % scale;
    if (whole > 99'999'999) return false;

    length = 0;
    if (negative && units != 0) entry[length++] = '-';
    char reversed[maximum_digits + 1];
    unsigned int count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole != 0);
    while (count != 0) entry[length++] = reversed[--count];

    if (fraction != 0) {
        entry[length++] = '.';
        char tail[maximum_digits];
        for (unsigned int i = decimals; i != 0; --i) {
            tail[i - 1] = static_cast<char>('0' + fraction % 10);
            fraction /= 10;
        }
        unsigned int used = decimals;
        while (used != 0 && tail[used - 1] == '0') --used;
        for (unsigned int i = 0; i < used; ++i) entry[length++] = tail[i];
    }
    entry[length] = 0;
    digits = 0;
    for (std::size_t i = 0; i < length; ++i)
        if (entry[i] >= '0' && entry[i] <= '9') ++digits;
    return true;
}

bool calculate(double right, double& result) {
    switch (pending_key) {
        case '+': result = left + right; return true;
        case '-': result = left - right; return true;
        case '*': result = left * right; return true;
        case '/':
            if (right == 0.0) return false;
            result = left / right;
            return true;
        default: return false;
    }
}

void append_digit(char key) {
    if (new_entry) {
        reset_entry();
        new_entry = false;
    }
    if (length == 1 && entry[0] == '0') {
        if (key != '0') entry[0] = key;
        return;
    }
    if (digits == maximum_digits || length + 1 >= sizeof entry) return;
    entry[length++] = key;
    entry[length] = 0;
    ++digits;
}

void append_dot() {
    if (new_entry) {
        reset_entry();
        new_entry = false;
    }
    if (has_dot() || length + 1 >= sizeof entry) return;
    entry[length++] = '.';
    entry[length] = 0;
}

void backspace() {
    if (new_entry) new_entry = false;
    if (length <= 1 || (length == 2 && entry[0] == '-')) {
        reset_entry();
        return;
    }
    const char removed = entry[--length];
    entry[length] = 0;
    if (removed >= '0' && removed <= '9') --digits;
}

void change_sign() {
    if (new_entry) new_entry = false;
    if (length == 1 && entry[0] == '0') return;
    if (entry[0] == '-') {
        for (std::size_t i = 0; i < length; ++i) entry[i] = entry[i + 1];
        --length;
    } else if (length + 1 < sizeof entry) {
        for (std::size_t i = length + 1; i != 0; --i) entry[i] = entry[i - 1];
        entry[0] = '-';
        ++length;
    }
}

}  // namespace

void clear() {
    reset_entry();
    left = 0.0;
    pending_key = 0;
    new_entry = false;
    failed = false;
}

const char* display() { return entry; }
char operation() { return pending_key; }

void press(char key) {
    if (key == 'C') { clear(); return; }
    if (failed) {
        if ((key >= '0' && key <= '9') || key == '.') clear();
        else return;
    }
    if (key >= '0' && key <= '9') { append_digit(key); return; }
    if (key == '.') { append_dot(); return; }
    if (key == 'D') { backspace(); return; }
    if (key == 'S') { change_sign(); return; }

    if (key == '=') {
        if (pending_key == 0 || new_entry) return;
        double result = 0.0;
        if (!calculate(parse(), result) || !format(result)) { error(); return; }
        pending_key = 0;
        new_entry = true;
        return;
    }
    if (key != '+' && key != '-' && key != '*' && key != '/') return;
    if (pending_key != 0 && !new_entry) {
        double result = 0.0;
        if (!calculate(parse(), result) || !format(result)) { error(); return; }
        left = result;
    } else {
        left = parse();
    }
    pending_key = key;
    new_entry = true;
}

}  // namespace kalkulator
