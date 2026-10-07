// Pawel Wodnicki (C) 2026
// 32bitmicro LLC (C) 2026
//
#include <complex>
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
//   Renders a high-resolution Mandelbrot fractal onto a target 16bpp Surface.
//  
//  --> target The destination 16-bit graphics surface.
//  --> max_iterations Maximum escape-time velocity ceiling (higher means sharper details).
//  <-> 
//  <-- return mm::display::Status Returns Success if rendered completely, otherwise an error status.
///
[[nodiscard]] Status draw_mandelbrot(Surface target, unsigned int max_iterations = 250) {
    if (!target.valid() || target.bits_per_pixel != 16) {
        return Status::BadArgument; 
    }

    const double width_f = static_cast<double>(target.width);
    const double height_f = static_cast<double>(target.height);
    const std::size_t row_stride = target.row_bytes();

    // Map fractal cleanly inside the display framing boundaries preserving 1:1 aspect ratio
    const double aspect = width_f / height_f;
    double x_min = -2.0;
    double x_max = 0.5;
    double y_min = -1.25;
    double y_max = 1.25;

    if (aspect > 1.0) {
        // Landscape: preserve vertical range [-1.25, 1.25], expand horizontal range around center c_real = -0.75
        const double half_w = 1.25 * aspect;
        x_min = -0.75 - half_w;
        x_max = -0.75 + half_w;
    } else if (aspect < 1.0) {
        // Portrait: preserve horizontal range [-2.0, 0.5], expand vertical range around center c_imag = 0.0
        const double half_h = 1.25 / aspect;
        y_min = -half_h;
        y_max = half_h;
    }

    for (unsigned int y = 0; y < target.height; ++y) {
        // Calculate the imaginary coordinate component for the row
        double c_imag = y_min + (static_cast<double>(y) / (height_f - 1.0)) * (y_max - y_min);
        
        // Find safe pointer bounds to write into row memory directly
        std::byte* row_ptr = target.pixels.data() + (y * row_stride);

        for (unsigned int x = 0; x < target.width; ++x) {
            double c_real = x_min + (static_cast<double>(x) / (width_f - 1.0)) * (x_max - x_min);

            std::complex<double> c(c_real, c_imag);
            std::complex<double> z = 0.0;
            unsigned int iter = 0;

            // Escape time iteration loop (norm represents squared magnitude)
            while (std::norm(z) <= 4.0 && iter < max_iterations) {
                z = z * z + c;
                ++iter;
            }

            // Decide color representation
            Rgb565 pixel_color;
            if (iter == max_iterations) {
                pixel_color = rgb565_black; // Core interior body of set
            } else {
                // Generate a vibrant smooth dynamic color spectrum gradient
                double t = static_cast<double>(iter) / static_cast<double>(max_iterations);
                
                unsigned char r = static_cast<unsigned char>(9.0 * (1.0 - t) * t * t * t * 255.0);
                unsigned char g = static_cast<unsigned char>(15.0 * (1.0 - t) * (1.0 - t) * t * t * 255.0);
                unsigned char b = static_cast<unsigned char>(8.5 * (1.0 - t) * (1.0 - t) * (1.0 - t) * t * 255.0);
                
                pixel_color = rgb(r, g, b);
            }

            // Directly pack the 16-bit color into the raw byte span row memory
            auto* out_pixel = reinterpret_cast<unsigned short*>(row_ptr + (x * 2u));
            *out_pixel = pixel_color.value;
        }
    }

    return Status::Ok;
}

} // namespace fractal
