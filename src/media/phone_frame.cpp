#include <opm/media/phone_frame.h>
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace opm::media {

// RGBA color helper (native byte order for pixel buffer)
static constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    return (r << 24) | (g << 16) | (b << 8) | a;
#else
    return r | (g << 8) | (b << 16) | (a << 24);
#endif
}

static constexpr uint32_t TRANSPARENT = rgba(0, 0, 0, 0);
static constexpr uint32_t ISLAND_COLOR = rgba(0, 0, 0, 255);         // Dynamic Island black

// Derive a slightly lighter shade for highlight edges / side buttons.
static inline uint8_t lighten(uint8_t c, int delta) {
    int v = (int)c + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

PhoneFrame::PhoneFrame() = default;

PhoneFrame::~PhoneFrame() {
    if (texture_) SDL_DestroyTexture(texture_);
    delete[] frame_pixels_;
}

void PhoneFrame::set_bezel_color(uint8_t r, uint8_t g, uint8_t b) {
    if (r == bezel_r_ && g == bezel_g_ && b == bezel_b_) return;
    bezel_r_ = r; bezel_g_ = g; bezel_b_ = b;
    // Invalidate cached texture so the next generate() rebuilds it.
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
    delete[] frame_pixels_; frame_pixels_ = nullptr;
    // Force regeneration on next call by clearing the size cache.
    screen_w_ = 0; screen_h_ = 0;
}

bool PhoneFrame::generate(SDL_Renderer* renderer, int screen_w, int screen_h) {
    if (texture_ && screen_w == screen_w_ && screen_h == screen_h_) {
        return true; // Already generated for this size
    }

    // Derived palette (user-configurable bezel colour).
    const uint32_t BEZEL_COLOR    = rgba(bezel_r_, bezel_g_, bezel_b_, 255);
    const uint32_t BEZEL_EDGE     = rgba(lighten(bezel_r_, 30),
                                          lighten(bezel_g_, 30),
                                          lighten(bezel_b_, 32), 255);
    // Side-button shade — must remain distinguishable from the bezel but
    // subtle enough not to be confused with the app's interactive UI
    // chrome (menu chips, drawer rails). A small lift on dark bezels and
    // a small darken on light ones reads as "subtle metallic accent"
    // rather than "tappable button".
    int bezel_luma = (bezel_r_ * 30 + bezel_g_ * 59 + bezel_b_ * 11) / 100;
    int btn_delta  = (bezel_luma < 128) ? 28 : -28;
    const uint32_t BUTTON_COLOR   = rgba(lighten(bezel_r_, btn_delta),
                                          lighten(bezel_g_, btn_delta),
                                          lighten(bezel_b_, btn_delta), 255);
    const uint32_t SCREEN_BORDER  = rgba(lighten(bezel_r_, -8),
                                          lighten(bezel_g_, -8),
                                          lighten(bezel_b_, -8), 255);

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

    // 6. Side buttons (power button on right, action+volume on left)
    //    Heights are 50% longer than the early prototype and the cluster
    //    sits further down the chassis to match a modern iPhone (15/16):
    //      * Power: ~28% of screen height, starts ~30% from the top.
    //      * Action: small button above the volume rockers (~3% tall).
    //      * Volume up/down: each ~7% tall, slightly below action.
    if (!is_tablet && is_portrait) {
        int btn_w = std::max(3, bezel_side_ / 4);

        // Action button (left side, above volume) — small.
        int act_h    = std::max(8, static_cast<int>(screen_h_ * 0.030f));
        int act_y    = bezel_top_ + static_cast<int>(screen_h_ * 0.17f);
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  0, act_y, btn_w, act_h, BUTTON_COLOR);

        // Volume up (left side) — 50% longer, moved further down.
        int vol_h    = std::max(21, static_cast<int>(screen_h_ * 0.0675f));
        int vol_up_y = act_y + act_h + std::max(6, vol_h / 3);
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  0, vol_up_y, btn_w, vol_h, BUTTON_COLOR);

        // Volume down (left side, below up).
        int vol_dn_y = vol_up_y + vol_h + std::max(4, vol_h / 3);
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  0, vol_dn_y, btn_w, vol_h, BUTTON_COLOR);

        // Power button (right side) — 50% longer; bottom aligned with
        // the bottom of the lower volume button to match real iPhone
        // proportions where the power and volume-down ends sit at the
        // same height on the chassis.
        int pwr_h = std::max(30, static_cast<int>(screen_h_ * 0.0975f));
        int pwr_y = (vol_dn_y + vol_h) - pwr_h;
        draw_rect(frame_pixels_, frame_w_, frame_h_,
                  frame_w_ - btn_w, pwr_y, btn_w, pwr_h, BUTTON_COLOR);
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

namespace {

// Signed distance to a rounded rectangle. <0 inside, >0 outside, ~0 on the
// edge. Used for both the outer frame fill and the screen cutout so the
// two curves share the same geometry — guarantees the screen border traces
// the bezel exactly.
inline float rrect_sdf(float px, float py,
                      float rx, float ry, float rw, float rh, float r) {
    float hx = rw * 0.5f;
    float hy = rh * 0.5f;
    float cx = rx + hx;
    float cy = ry + hy;
    float qx = std::abs(px - cx) - (hx - r);
    float qy = std::abs(py - cy) - (hy - r);
    float ox = std::max(qx, 0.0f);
    float oy = std::max(qy, 0.0f);
    float outside = std::sqrt(ox * ox + oy * oy);
    float inside  = std::min(std::max(qx, qy), 0.0f);
    return outside + inside - r;
}

inline void blend_pixel(uint32_t* dst_ptr, uint32_t src, float coverage) {
    if (coverage <= 0.0f) return;
    if (coverage > 1.0f) coverage = 1.0f;

    uint8_t sa = (src >> 24) & 0xFF;
    if (sa == 0) {
        // Erase mode: multiply existing alpha by (1 - coverage). Keeps RGB
        // intact so the cut-out edge fades smoothly into the surrounding
        // bezel colour.
        uint32_t d = *dst_ptr;
        uint8_t da = (d >> 24) & 0xFF;
        uint8_t new_a = static_cast<uint8_t>(da * (1.0f - coverage));
        *dst_ptr = (d & 0x00FFFFFFu) | (static_cast<uint32_t>(new_a) << 24);
        return;
    }

    float a = coverage * (sa / 255.0f);
    float ia = 1.0f - a;

    uint32_t d = *dst_ptr;
    uint8_t dr = d & 0xFF;
    uint8_t dg = (d >> 8) & 0xFF;
    uint8_t db = (d >> 16) & 0xFF;
    uint8_t da = (d >> 24) & 0xFF;

    uint8_t sr = src & 0xFF;
    uint8_t sg = (src >> 8) & 0xFF;
    uint8_t sb = (src >> 16) & 0xFF;

    uint8_t orr = static_cast<uint8_t>(sr * a + dr * ia);
    uint8_t og  = static_cast<uint8_t>(sg * a + dg * ia);
    uint8_t ob  = static_cast<uint8_t>(sb * a + db * ia);
    float new_a_f = a * 255.0f + da * ia;
    if (new_a_f > 255.0f) new_a_f = 255.0f;
    uint8_t oa = static_cast<uint8_t>(new_a_f);

    *dst_ptr = orr | (static_cast<uint32_t>(og) << 8)
                   | (static_cast<uint32_t>(ob) << 16)
                   | (static_cast<uint32_t>(oa) << 24);
}

} // namespace

void PhoneFrame::draw_rounded_rect(uint32_t* pixels, int img_w, int img_h,
                                    int x, int y, int w, int h, int radius, uint32_t color) {
    // SDF-based draw with 0.5-px anti-aliased edges. Works in both fill
    // mode (opaque colour) and erase mode (alpha=0 → screen cutout).
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);
    float fr = static_cast<float>(radius);

    // Tight scan: only iterate the rect bbox (no point scanning the whole
    // frame). Clip to image bounds.
    int y0 = std::max(0, y);
    int y1 = std::min(img_h, y + h);
    int x0 = std::max(0, x);
    int x1 = std::min(img_w, x + w);

    for (int py = y0; py < y1; ++py) {
        float spy = py + 0.5f;
        uint32_t* row = pixels + py * img_w;
        for (int px = x0; px < x1; ++px) {
            float spx = px + 0.5f;
            float sd = rrect_sdf(spx, spy, fx, fy, fw, fh, fr);
            // coverage = 1 deep inside, 0 outside, ~linear in [-0.5, +0.5]
            float cov = 0.5f - sd;
            if (cov <= 0.0f) continue;
            blend_pixel(row + px, color, cov);
        }
    }
}

void PhoneFrame::draw_rounded_rect_outline(uint32_t* pixels, int img_w, int img_h,
                                            int x, int y, int w, int h, int radius,
                                            int thickness, uint32_t color) {
    // Outline = ring between the outer rounded rect and an inner rect
    // shrunk by `thickness` on every side. Computing both SDFs gives us
    // a clean anti-aliased stroke that follows the corner curvature.
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);
    float fr_out = static_cast<float>(radius);

    float t  = static_cast<float>(thickness);
    float ix = fx + t;
    float iy = fy + t;
    float iw = std::max(0.0f, fw - 2.0f * t);
    float ih = std::max(0.0f, fh - 2.0f * t);
    float fr_in = std::max(0.0f, fr_out - t);

    int y0 = std::max(0, y);
    int y1 = std::min(img_h, y + h);
    int x0 = std::max(0, x);
    int x1 = std::min(img_w, x + w);

    for (int py = y0; py < y1; ++py) {
        float spy = py + 0.5f;
        uint32_t* row = pixels + py * img_w;
        for (int px = x0; px < x1; ++px) {
            float spx = px + 0.5f;
            float sd_out = rrect_sdf(spx, spy, fx, fy, fw, fh, fr_out);
            float sd_in  = rrect_sdf(spx, spy, ix, iy, iw, ih, fr_in);
            float cov_out = std::clamp(0.5f - sd_out, 0.0f, 1.0f);
            float cov_in  = std::clamp(0.5f - sd_in,  0.0f, 1.0f);
            float ring = cov_out - cov_in;
            if (ring <= 0.0f) continue;
            blend_pixel(row + px, color, ring);
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

} // namespace opm::media
