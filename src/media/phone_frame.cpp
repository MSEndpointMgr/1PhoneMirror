#include <openmirror/media/phone_frame.h>
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace openmirror::media {

// RGBA color helper (native byte order for pixel buffer)
static constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    return (r << 24) | (g << 16) | (b << 8) | a;
#else
    return r | (g << 8) | (b << 16) | (a << 24);
#endif
}

static constexpr uint32_t TRANSPARENT = rgba(0, 0, 0, 0);
static constexpr uint32_t BEZEL_COLOR = rgba(28, 28, 30, 255);       // Dark titanium
static constexpr uint32_t BEZEL_EDGE  = rgba(58, 58, 62, 255);       // Subtle lighter edge
static constexpr uint32_t ISLAND_COLOR = rgba(0, 0, 0, 255);         // Dynamic Island black
static constexpr uint32_t BUTTON_COLOR = rgba(44, 44, 48, 255);      // Side button color
static constexpr uint32_t SCREEN_BORDER = rgba(20, 20, 22, 255);     // Thin border around screen

PhoneFrame::PhoneFrame() = default;

PhoneFrame::~PhoneFrame() {
    if (texture_) SDL_DestroyTexture(texture_);
    delete[] frame_pixels_;
}

bool PhoneFrame::generate(SDL_Renderer* renderer, int screen_w, int screen_h) {
    if (texture_ && screen_w == screen_w_ && screen_h == screen_h_) {
        return true; // Already generated for this size
    }

    // Clean up previous
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
    delete[] frame_pixels_;
    frame_pixels_ = nullptr;

    screen_w_ = screen_w;
    screen_h_ = screen_h;

    // Calculate bezel sizes proportional to screen (matching iPhone 15/16 proportions)
    // iPhone 16 Pro: 6.3" display, ~2.5mm bezels ≈ 1.7% of width
    // We make bezels slightly thicker for visual clarity on screen
    float aspect = static_cast<float>(screen_h) / screen_w;
    bool is_portrait = aspect > 1.0f;
    // A tablet (iPad) has an aspect ratio close to 4:3 (1.33) — much less
    // elongated than a phone (~2.16). Use a different bezel style for those:
    // thin uniform bezels, small corner radius, NO dynamic island, no side
    // buttons. Threshold of 1.6 separates iPad-like (≤1.6) from phone-like.
    float long_over_short = is_portrait ? aspect : (1.0f / aspect);
    bool is_tablet = long_over_short < 1.6f;
    is_tablet_ = is_tablet;

    int short_side = is_portrait ? screen_w : screen_h;

    if (is_tablet) {
        // iPad: thin uniform bezels (~1.5% of short side), small corners (~3%)
        bezel_side_ = std::max(8, static_cast<int>(short_side * 0.025f));
        bezel_top_ = bezel_side_;
        bezel_bottom_ = bezel_side_;
        corner_radius_ = std::max(12, static_cast<int>(short_side * 0.045f));
    } else {
        // Bezel proportions relative to shorter screen dimension
        bezel_side_ = std::max(8, static_cast<int>(short_side * 0.028f));
        bezel_top_ = std::max(12, static_cast<int>(short_side * 0.042f));
        bezel_bottom_ = std::max(12, static_cast<int>(short_side * 0.042f));

        // Corner radius: iPhone has ~55pt corners on a 393pt wide screen ≈ 14%
        corner_radius_ = std::max(20, static_cast<int>(short_side * 0.11f));
    }

    frame_w_ = screen_w_ + bezel_side_ * 2;
    frame_h_ = screen_h_ + bezel_top_ + bezel_bottom_;

    // Allocate pixel buffer
    frame_pixels_ = new uint32_t[frame_w_ * frame_h_];
    memset(frame_pixels_, 0, frame_w_ * frame_h_ * sizeof(uint32_t)); // All transparent

    // 1. Draw the outer body (full rounded rect in bezel color)
    draw_rounded_rect(frame_pixels_, frame_w_, frame_h_,
                      0, 0, frame_w_, frame_h_, corner_radius_, BEZEL_COLOR);

    // 2. Draw a subtle edge highlight (slightly inset, 1px line effect)
    draw_rounded_rect_outline(frame_pixels_, frame_w_, frame_h_,
                              1, 1, frame_w_ - 2, frame_h_ - 2,
                              corner_radius_ - 1, 1, BEZEL_EDGE);

    // 3. Cut out the screen area (transparent) with its own corner radius
    int screen_corner = std::max(8, corner_radius_ - bezel_side_);
    draw_rounded_rect(frame_pixels_, frame_w_, frame_h_,
                      bezel_side_, bezel_top_, screen_w_, screen_h_,
                      screen_corner, TRANSPARENT);

    // 4. Thin dark border around the screen cutout
    draw_rounded_rect_outline(frame_pixels_, frame_w_, frame_h_,
                              bezel_side_ - 1, bezel_top_ - 1,
                              screen_w_ + 2, screen_h_ + 2,
                              screen_corner + 1, 1, SCREEN_BORDER);

    // 5. Dynamic Island — intentionally omitted. The mirrored video already
    //    occupies the full screen area; painting a pill on top would obscure
    //    real content (clock/widgets on iPad, status bar on iPhone).

    // 6. Side buttons (power button on right, volume on left)
    if (!is_tablet && is_portrait) {
        int btn_w = std::max(3, bezel_side_ / 4);
        // Power button (right side, upper-middle)
        int pwr_h = std::max(20, static_cast<int>(screen_h_ * 0.065f));
        int pwr_y = bezel_top_ + static_cast<int>(screen_h_ * 0.22f);
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  frame_w_ - btn_w, pwr_y, btn_w, pwr_h, BUTTON_COLOR);

        // Volume up (left side)
        int vol_h = std::max(14, static_cast<int>(screen_h_ * 0.045f));
        int vol_up_y = bezel_top_ + static_cast<int>(screen_h_ * 0.18f);
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  0, vol_up_y, btn_w, vol_h, BUTTON_COLOR);

        // Volume down (left side, below up)
        int vol_dn_y = vol_up_y + vol_h + std::max(4, vol_h / 3);
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  0, vol_dn_y, btn_w, vol_h, BUTTON_COLOR);
    }

    // Create SDL texture from pixels
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STATIC, frame_w_, frame_h_);
    if (!texture_) {
        std::cerr << "[PhoneFrame] Failed to create texture: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(texture_, nullptr, frame_pixels_, frame_w_ * sizeof(uint32_t));

    std::cout << "[PhoneFrame] Generated " << frame_w_ << "x" << frame_h_
              << " frame for " << screen_w_ << "x" << screen_h_ << " screen"
              << " (bezels: " << bezel_side_ << "/" << bezel_top_ << "/" << bezel_bottom_
              << ", corner: " << corner_radius_ << ")\n";
    return true;
}

void PhoneFrame::render(SDL_Renderer* renderer, int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!texture_) return;
    SDL_Rect dst = { dst_x, dst_y, dst_w, dst_h };
    SDL_RenderCopy(renderer, texture_, nullptr, &dst);
}

uint8_t* PhoneFrame::composite_screenshot(SDL_Renderer* /*renderer*/,
                                           const uint8_t* video_rgba, int vid_w, int vid_h,
                                           int vid_stride,
                                           int* out_w, int* out_h) {
    if (!frame_pixels_) return nullptr;

    *out_w = frame_w_;
    *out_h = frame_h_;

    // Output buffer: RGBA
    int out_stride = frame_w_ * 4;
    uint8_t* out = new uint8_t[out_stride * frame_h_];
    memset(out, 0, out_stride * frame_h_); // Transparent background

    // Paste video into the screen area, scaled to fit
    float scale_x = static_cast<float>(screen_w_) / vid_w;
    float scale_y = static_cast<float>(screen_h_) / vid_h;
    float scale = std::min(scale_x, scale_y);
    int scaled_w = static_cast<int>(vid_w * scale);
    int scaled_h = static_cast<int>(vid_h * scale);
    int offset_x = bezel_side_ + (screen_w_ - scaled_w) / 2;
    int offset_y = bezel_top_ + (screen_h_ - scaled_h) / 2;

    // Simple bilinear-ish scaling of video into output
    for (int y = 0; y < scaled_h; y++) {
        int src_y = y * vid_h / scaled_h;
        if (src_y >= vid_h) src_y = vid_h - 1;
        for (int x = 0; x < scaled_w; x++) {
            int src_x = x * vid_w / scaled_w;
            if (src_x >= vid_w) src_x = vid_w - 1;

            int dst_px = offset_x + x;
            int dst_py = offset_y + y;
            if (dst_px < 0 || dst_px >= frame_w_ || dst_py < 0 || dst_py >= frame_h_)
                continue;

            const uint8_t* src = video_rgba + src_y * vid_stride + src_x * 4;
            uint8_t* dst = out + dst_py * out_stride + dst_px * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255; // Opaque video
        }
    }

    // Apply phone frame on top (alpha compositing)
    for (int y = 0; y < frame_h_; y++) {
        for (int x = 0; x < frame_w_; x++) {
            uint32_t fp = frame_pixels_[y * frame_w_ + x];
            // Extract RGBA from frame pixel
            uint8_t fr, fg, fb, fa;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
            fr = (fp >> 24) & 0xFF;
            fg = (fp >> 16) & 0xFF;
            fb = (fp >> 8) & 0xFF;
            fa = fp & 0xFF;
#else
            fr = fp & 0xFF;
            fg = (fp >> 8) & 0xFF;
            fb = (fp >> 16) & 0xFF;
            fa = (fp >> 24) & 0xFF;
#endif
            if (fa == 0) continue; // Transparent frame pixel — keep what's below

            uint8_t* dst = out + y * out_stride + x * 4;

            if (fa == 255) {
                // Opaque frame pixel: replace
                dst[0] = fr;
                dst[1] = fg;
                dst[2] = fb;
                dst[3] = 255;
            } else {
                // Alpha blend
                float a = fa / 255.0f;
                float inv_a = 1.0f - a;
                dst[0] = static_cast<uint8_t>(fr * a + dst[0] * inv_a);
                dst[1] = static_cast<uint8_t>(fg * a + dst[1] * inv_a);
                dst[2] = static_cast<uint8_t>(fb * a + dst[2] * inv_a);
                dst[3] = static_cast<uint8_t>(std::min(255.0f, fa + dst[3] * inv_a));
            }
        }
    }

    // Now mask outside corners to transparent:
    // Any pixel outside the outer rounded rect should be transparent.
    // We can detect this by checking if the original frame pixel was transparent
    // AND no video was placed there.
    // Actually, the frame_pixels_ already defines the shape: where frame is transparent
    // AND it's outside the screen area, the output should also be transparent.
    // The screen area is already filled with video. The bezels are filled with bezel color.
    // The only issue is the outer corners which should remain transparent.
    // Since we started with memset(0), and only wrote video inside the screen rect
    // and frame pixels where fa > 0, the outer corners should already be transparent.

    return out;
}

// --- Drawing primitives ---

void PhoneFrame::draw_rounded_rect(uint32_t* pixels, int img_w, int img_h,
                                    int x, int y, int w, int h, int radius, uint32_t color) {
    int r2 = radius * radius;
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= img_h) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= img_w) continue;

            // Check if pixel is inside the rounded rectangle
            int lx = px - x;       // Local x within rect
            int ly = py - y;       // Local y within rect
            bool inside = true;

            // Check corners
            if (lx < radius && ly < radius) {
                // Top-left corner
                int dx = radius - lx;
                int dy = radius - ly;
                if (dx * dx + dy * dy > r2) inside = false;
            } else if (lx >= w - radius && ly < radius) {
                // Top-right corner
                int dx = lx - (w - radius - 1);
                int dy = radius - ly;
                if (dx * dx + dy * dy > r2) inside = false;
            } else if (lx < radius && ly >= h - radius) {
                // Bottom-left corner
                int dx = radius - lx;
                int dy = ly - (h - radius - 1);
                if (dx * dx + dy * dy > r2) inside = false;
            } else if (lx >= w - radius && ly >= h - radius) {
                // Bottom-right corner
                int dx = lx - (w - radius - 1);
                int dy = ly - (h - radius - 1);
                if (dx * dx + dy * dy > r2) inside = false;
            }

            if (inside) {
                pixels[py * img_w + px] = color;
            }
        }
    }
}

void PhoneFrame::draw_rounded_rect_outline(uint32_t* pixels, int img_w, int img_h,
                                            int x, int y, int w, int h, int radius,
                                            int thickness, uint32_t color) {
    int outer_r2 = radius * radius;
    int inner_r = std::max(0, radius - thickness);
    int inner_r2 = inner_r * inner_r;

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= img_h) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= img_w) continue;

            int lx = px - x;
            int ly = py - y;

            // Must be on the edge (within thickness of the border)
            bool on_outer_edge = (lx < thickness || lx >= w - thickness ||
                                  ly < thickness || ly >= h - thickness);
            if (!on_outer_edge) continue;

            // Must be inside the outer rounded rect
            bool inside_outer = true;
            auto check_corner = [&](int cx, int cy, int r2_check) -> bool {
                int dx = lx - cx;
                int dy = ly - cy;
                return dx * dx + dy * dy <= r2_check;
            };

            if (lx < radius && ly < radius) {
                inside_outer = check_corner(radius, radius, outer_r2);
            } else if (lx >= w - radius && ly < radius) {
                inside_outer = check_corner(w - radius - 1, radius, outer_r2);
            } else if (lx < radius && ly >= h - radius) {
                inside_outer = check_corner(radius, h - radius - 1, outer_r2);
            } else if (lx >= w - radius && ly >= h - radius) {
                inside_outer = check_corner(w - radius - 1, h - radius - 1, outer_r2);
            }

            if (inside_outer) {
                pixels[py * img_w + px] = color;
            }
        }
    }
}

void PhoneFrame::draw_pill(uint32_t* pixels, int img_w, int img_h,
                            int cx, int cy, int pill_w, int pill_h, uint32_t color) {
    // Pill = rectangle with semicircular ends
    int radius = std::min(pill_w, pill_h) / 2;
    int half_w = pill_w / 2;
    int half_h = pill_h / 2;

    for (int py = cy - half_h; py <= cy + half_h; py++) {
        if (py < 0 || py >= img_h) continue;
        for (int px = cx - half_w; px <= cx + half_w; px++) {
            if (px < 0 || px >= img_w) continue;

            int dx = px - cx;
            int dy = py - cy;

            bool inside = false;
            if (pill_w >= pill_h) {
                // Horizontal pill
                int straight = half_w - radius;
                if (std::abs(dx) <= straight && std::abs(dy) <= radius) {
                    inside = true;
                } else if (dx > straight) {
                    int ex = dx - straight;
                    inside = (ex * ex + dy * dy <= radius * radius);
                } else if (dx < -straight) {
                    int ex = dx + straight;
                    inside = (ex * ex + dy * dy <= radius * radius);
                }
            } else {
                // Vertical pill
                int straight = half_h - radius;
                if (std::abs(dy) <= straight && std::abs(dx) <= radius) {
                    inside = true;
                } else if (dy > straight) {
                    int ey = dy - straight;
                    inside = (dx * dx + ey * ey <= radius * radius);
                } else if (dy < -straight) {
                    int ey = dy + straight;
                    inside = (dx * dx + ey * ey <= radius * radius);
                }
            }

            if (inside) {
                pixels[py * img_w + px] = color;
            }
        }
    }
}

void PhoneFrame::draw_rect(uint32_t* pixels, int img_w, int img_h,
                            int x, int y, int w, int h, uint32_t color) {
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= img_h) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= img_w) continue;
            pixels[py * img_w + px] = color;
        }
    }
}

} // namespace openmirror::media
