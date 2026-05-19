#pragma once

#include <cstdint>

struct SDL_Renderer;
struct SDL_Texture;

namespace opm::media {

// Generates and manages an iPhone-style frame overlay.
// The frame is a dark bezel with rounded corners, Dynamic Island,
// and side buttons. The screen area is transparent.
class PhoneFrame {
public:
    PhoneFrame();
    ~PhoneFrame();

    // Generate the frame texture for a given screen content size.
    // The frame adds bezels around the screen area.
    // Returns false if generation fails.
    bool generate(SDL_Renderer* renderer, int screen_w, int screen_h);

    // Render the frame overlay on top of the current scene.
    // dst_x/dst_y is where the frame's top-left corner goes in window coords.
    void render(SDL_Renderer* renderer, int dst_x, int dst_y, int dst_w, int dst_h);

    // Get the total frame dimensions (screen + bezels)
    int frame_width() const { return frame_w_; }
    int frame_height() const { return frame_h_; }

    // Get the screen area rect within the frame
    int screen_x() const { return bezel_side_; }
    int screen_y() const { return bezel_top_; }
    int screen_width() const { return screen_w_; }
    int screen_height() const { return screen_h_; }

    // Get bezel sizes (in frame-local pixels)
    int bezel_left() const { return bezel_side_; }
    int bezel_right() const { return bezel_side_; }
    int bezel_top_size() const { return bezel_top_; }
    int bezel_bottom_size() const { return bezel_bottom_; }
    int corner_radius() const { return corner_radius_; }

    // User-configurable bezel colour. Button + edge colours are derived
    // automatically. Calling this invalidates the cached texture so the
    // next generate() call re-rasterises with the new palette.
    void set_bezel_color(uint8_t r, uint8_t g, uint8_t b);
    uint8_t bezel_r() const { return bezel_r_; }
    uint8_t bezel_g() const { return bezel_g_; }
    uint8_t bezel_b() const { return bezel_b_; }

    // True if last generate() detected a tablet aspect ratio (used by UI to
    // keep overlays from scaling up on iPad-shaped frames).
    bool is_tablet() const { return is_tablet_; }

    // Composite screenshot: video frame pixels + phone frame with alpha.
    // Returns RGBA pixel buffer (caller must free with delete[]).
    // out_w/out_h receive the image dimensions.
    uint8_t* composite_screenshot(SDL_Renderer* renderer,
                                  const uint8_t* video_rgba, int vid_w, int vid_h, int vid_stride,
                                  int* out_w, int* out_h);

    bool is_generated() const { return texture_ != nullptr; }

private:
    void draw_rounded_rect(uint32_t* pixels, int img_w, int img_h,
                           int x, int y, int w, int h, int radius, uint32_t color);
    void draw_pill(uint32_t* pixels, int img_w, int img_h,
                   int cx, int cy, int pill_w, int pill_h, uint32_t color);
    void draw_rect(uint32_t* pixels, int img_w, int img_h,
                   int x, int y, int w, int h, uint32_t color);
    void draw_rounded_rect_outline(uint32_t* pixels, int img_w, int img_h,
                                   int x, int y, int w, int h, int radius,
                                   int thickness, uint32_t color);

    SDL_Texture* texture_ = nullptr;
    uint32_t* frame_pixels_ = nullptr; // Kept for screenshot compositing

    // User-configurable bezel colour (defaults to dark titanium).
    uint8_t bezel_r_ = 28, bezel_g_ = 28, bezel_b_ = 30;

    int screen_w_ = 0;
    int screen_h_ = 0;
    int frame_w_ = 0;
    int frame_h_ = 0;
    int bezel_side_ = 0;
    int bezel_top_ = 0;
    int bezel_bottom_ = 0;
    int corner_radius_ = 0;
    bool is_tablet_ = false;
};

} // namespace opm::media
