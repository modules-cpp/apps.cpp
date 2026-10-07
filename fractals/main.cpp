// Pawel Wodnicki (C) 2026
// 32bitmicro LLC (C) 2026
//
#include <array>
#include <cstddef>
#include <span>

import mm.display;
import mm.gfx;
import mm.mcu;

namespace fractal {
[[nodiscard]] mm::display::Status draw_mandelbrot(mm::gfx::Surface target,
                                                  unsigned int max_iterations = 250);
[[nodiscard]] mm::display::Status draw_sierpinski(mm::gfx::Surface target,
                                                  unsigned int max_iterations = 250);
}

namespace {

using mm::display::Color;
using mm::display::Status;
using mm::gfx::Rgb565;
using mm::gfx::Surface;

// Frame budget: the maximum dimensions supported for the panel surface.
constexpr unsigned int maximum_width = 480;
constexpr unsigned int maximum_height = 320;
constexpr std::size_t maximum_surface_bytes = maximum_width * maximum_height * 2u;
std::array<std::byte, maximum_surface_bytes> surface_bytes;

constexpr unsigned int still_hold_ms = 4'000;

// One orientation / turn: renders the Mandelbrot fractal across the entire surface.
// Returns the step of main's exit codes that failed, or zero.
[[nodiscard]] int show(mm::display::Display& display,
                       mm::display::Geometry geometry, unsigned int turns) {
    if (geometry.bits_per_pixel != 16) return 2;
    if (geometry.width == 0 || geometry.height == 0) return 2;
    if (geometry.width > maximum_width || geometry.height > maximum_height) return 3;

    const std::size_t required_bytes =
        static_cast<std::size_t>(geometry.width) * geometry.height * 2u;
    const Surface surface{geometry.width, geometry.height, 16,
                          std::span<std::byte>{surface_bytes.data(), required_bytes}};

    // Progression of detail across orientations
    constexpr unsigned int iterations[] = {32, 64, 128, 250};
    const unsigned int max_iter = iterations[turns % 4u];

    // Mandelbrot
    if (fractal::draw_mandelbrot(surface, max_iter) != Status::Ok) return 5;
    if (mm::gfx::write(display, surface, 0, 0) != Status::Ok) return 6;
    if (display.refresh(mm::display::Refresh::Full) != Status::Ok) return 7;
    if (mm::mcu::delay_ms(still_hold_ms) != mm::mcu::Status::Ok) return 8;

    // Sierpinski
    if (fractal::draw_sierpinski(surface, max_iter) != Status::Ok) return 5;
    if (mm::gfx::write(display, surface, 0, 0) != Status::Ok) return 6;
    if (display.refresh(mm::display::Refresh::Full) != Status::Ok) return 7;
    if (mm::mcu::delay_ms(still_hold_ms) != mm::mcu::Status::Ok) return 8;

    return 0;
}

}

int main() {
    auto& display = mm::display::selected_display();
    if (display.initialize() != Status::Ok) return 1;
    const auto geometry = display.geometry();
    if (geometry.width == 0 || geometry.height == 0 ||
        (geometry.bits_per_pixel != 1 && geometry.bits_per_pixel != 16))
        return 2;
    if (geometry.width > maximum_width || geometry.height > maximum_height)
        return 3;

    for (unsigned int turns = 0; turns < 4; ++turns)
        if (const int step = show(display, geometry, turns); step != 0)
            return step;
    if (display.sleep() != Status::Ok) return 9;
    return 0;
}
