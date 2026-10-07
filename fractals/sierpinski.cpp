// Pawel Wodnicki (C) 2026
// 32bitmicro LLC (C) 2026
//
#include <cstddef>

import mm.display;
import mm.gfx;

namespace fractal {

using mm::display::Status;
using mm::gfx::Rgb565;
using mm::gfx::Surface;
using mm::gfx::rgb;
using mm::gfx::rgb565_black;

///
//   Renders a high-resolution Sierpinski triangle fractal onto a target 16bpp Surface.
//  
//  --> target The destination 16-bit graphics surface.
//  --> max_iterations Maximum recursive subdivision depth.
//  <-> 
//  <-- return mm::display::Status Returns Ok if rendered completely, otherwise an error status.
///
[[nodiscard]] Status draw_sierpinski(Surface target, unsigned int max_iterations = 8) {
    if (!target.valid() || target.bits_per_pixel != 16) {
        return Status::BadArgument; 
    }

    const double width_f = static_cast<double>(target.width);
    const double height_f = static_cast<double>(target.height);
    const std::size_t row_stride = target.row_bytes();

    // Map equilateral triangle centered within target bounds preserving 1:1 aspect ratio
    // For an equilateral triangle: base = height * (2 / sqrt(3))
    constexpr double inv_sqrt3_2 = 1.1547005383792515; // 2.0 / std::sqrt(3.0)
    
    // Fit triangle with a 5% margin
    double h_tri = 0.90 * height_f;
    double b_tri = h_tri * inv_sqrt3_2;
    if (b_tri > 0.90 * width_f) {
        b_tri = 0.90 * width_f;
        h_tri = b_tri * 0.8660254037844386; // b_tri * (std::sqrt(3.0) / 2.0)
    }

    const double a_x = width_f / 2.0;
    const double a_y = (height_f - h_tri) / 2.0;

    for (unsigned int y = 0; y < target.height; ++y) {
        const double y_f = static_cast<double>(y);
        const double s = (y_f - a_y) / h_tri;

        std::byte* row_ptr = target.pixels.data() + (y * row_stride);

        for (unsigned int x = 0; x < target.width; ++x) {
            const double x_f = static_cast<double>(x);
            const double d = 2.0 * (x_f - a_x) / b_tri;

            double u = (s - d) / 2.0;
            double v = (s + d) / 2.0;

            // Outside the outer bounding triangle
            if (u < 0.0 || v < 0.0 || (u + v) > 1.0) {
                auto* out_pixel = reinterpret_cast<unsigned short*>(row_ptr + (x * 2u));
                *out_pixel = rgb565_black.value;
                continue;
            }

            // Recursive subdivision iteration loop: check which scale removes the point
            unsigned int iter = 0;
            while (iter < max_iterations) {
                // Check if (u, v) falls into the central inverted triangle (hole) at this scale
                if (u <= 0.5 && v <= 0.5 && (u + v) > 0.5) {
                    break;
                }
                // Zoom by 2 into the containing corner sub-triangle
                if (u > 0.5) {
                    u = 2.0 * u - 1.0;
                    v = 2.0 * v;
                } else if (v > 0.5) {
                    u = 2.0 * u;
                    v = 2.0 * v - 1.0;
                } else {
                    u = 2.0 * u;
                    v = 2.0 * v;
                }
                ++iter;
            }

            Rgb565 pixel_color;
            if (iter == max_iterations) {
                // In the Sierpinski fractal body
                pixel_color = rgb(255, 255, 255); // Pure white edges
            } else {
                // Vibrant color gradient in the recursive hole chambers
                double t = static_cast<double>(iter + 1) / static_cast<double>(max_iterations + 2);
                unsigned char r = static_cast<unsigned char>(9.0 * (1.0 - t) * t * t * t * 255.0);
                unsigned char g = static_cast<unsigned char>(15.0 * (1.0 - t) * (1.0 - t) * t * t * 255.0);
                unsigned char b = static_cast<unsigned char>(8.5 * (1.0 - t) * (1.0 - t) * (1.0 - t) * t * 255.0);
                pixel_color = rgb(r, g, b);
            }

            auto* out_pixel = reinterpret_cast<unsigned short*>(row_ptr + (x * 2u));
            *out_pixel = pixel_color.value;
        }
    }

    return Status::Ok;
}

} // namespace fractal
