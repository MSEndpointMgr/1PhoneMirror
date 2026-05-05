#pragma once

#include <openmirror/media/decoder.h>
#include <openmirror/media/phone_frame.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace openmirror::media {

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(const std::string& title, int width, int height);
    void shutdown();
    void submit_frame(VideoFrame frame);
    void run();
    void stop();
    void request_reset() { reset_requested_.store(true); }
    bool is_running() const { return running_.load(); }
    SDL_Window* window() const { return window_; }
    void set_phone_frame_enabled(bool enabled) { phone_frame_enabled_ = enabled; }
    bool phone_frame_enabled() const { return phone_frame_enabled_; }

private:
    void render_frame();
    void draw_island();
    void draw_footer(int svx, int svy, int svw, int svh);
    void draw_info_panel();
    void take_screenshot();
    void open_screenshot_folder();
    void copy_to_clipboard(const uint8_t* rgba, int w, int h);
    void update_window_shape();
    void begin_window_drag();
    void load_icon_texture();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* sdl_renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* icon_texture_ = nullptr;
    int icon_tex_w_ = 0, icon_tex_h_ = 0;

    std::mutex frame_mutex_;
    VideoFrame pending_frame_;
    bool has_new_frame_ = false;
    bool ever_received_frame_ = false;

    std::vector<uint8_t> last_frame_data_;
    int last_frame_w_ = 0, last_frame_h_ = 0, last_frame_stride_ = 0;
    int tex_width_ = 0, tex_height_ = 0;

    PhoneFrame phone_frame_;
    bool phone_frame_enabled_ = true;
    bool screenshot_requested_ = false;
    bool window_shape_set_ = false;
    bool first_render_ = true;

    // Button hit rects
    struct BtnRect { int x = 0, y = 0, w = 0, h = 0; };
    BtnRect close_btn_, screenshot_btn_, folder_btn_, icon_btn_, info_btn_, menu_btn_;
    int hover_btn_ = -1; // -1=none, 0=close, 1=screenshot, 2=folder, 3=icon, 4=info, 5=menu

    // Island visibility & animation
    bool island_visible_ = true;
    float island_anim_ = 1.0f; // 0=hidden, 1=fully visible
    std::chrono::steady_clock::time_point island_anim_start_;
    bool island_animating_ = false;

    // Resize
    bool resizing_ = false;
    int resize_start_gx_ = 0, resize_start_gy_ = 0;
    int resize_start_w_ = 0, resize_start_h_ = 0;
    BtnRect resize_grip_;

    // Button flash
    bool btn_flash_ = false;
    std::chrono::steady_clock::time_point btn_flash_start_;

    // Toast
    bool toast_active_ = false;
    std::chrono::steady_clock::time_point toast_start_;
    std::string toast_text_;
    SDL_Texture* toast_tex_ = nullptr;
    int toast_tex_w_ = 0, toast_tex_h_ = 0;
    std::string toast_tex_str_;

    // Tooltip
    SDL_Texture* tooltip_tex_ = nullptr;
    int tooltip_tex_w_ = 0, tooltip_tex_h_ = 0;
    int last_tooltip_btn_ = -1;

    // Logo texture (loaded from PNG)
    SDL_Texture* logo_texture_ = nullptr;
    int logo_tex_w_ = 0, logo_tex_h_ = 0;
    void load_logo_texture();

    // Waiting screen
    SDL_Texture* waiting_tex_ = nullptr;
    int waiting_tex_w_ = 0, waiting_tex_h_ = 0;

    // Cast instructions
    SDL_Texture* ios_instr_tex_ = nullptr;
    int ios_instr_w_ = 0, ios_instr_h_ = 0;
    SDL_Texture* android_instr_tex_ = nullptr;
    int android_instr_w_ = 0, android_instr_h_ = 0;
    SDL_Texture* ios_icon_tex_ = nullptr;
    int ios_icon_sz_ = 0;
    SDL_Texture* android_icon_tex_ = nullptr;
    int android_icon_sz_ = 0;

    // Footer (waiting screen)
    struct FooterSeg {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        std::string url;
        std::string tooltip;
    };
    std::vector<FooterSeg> footer_line1_, footer_line2_;
    struct FooterHit { int x = 0, y = 0, w = 0, h = 0; std::string url; std::string tooltip; };
    std::vector<FooterHit> footer_hits_;
    SDL_Texture* footer_tooltip_tex_ = nullptr;
    int footer_tooltip_w_ = 0, footer_tooltip_h_ = 0;
    std::string footer_tooltip_str_;

    // Info panel
    bool info_panel_visible_ = false;
    bool info_panel_animating_ = false;
    float info_panel_anim_ = 0.0f; // 0=hidden, 1=fully visible
    std::chrono::steady_clock::time_point info_panel_anim_start_;
    BtnRect info_panel_rect_;
    struct InfoLine {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::vector<InfoLine> info_lines_;

    // Version panel
    bool version_panel_visible_ = false;
    bool version_panel_animating_ = false;
    float version_panel_anim_ = 0.0f; // 0=hidden, 1=fully visible
    std::chrono::steady_clock::time_point version_panel_anim_start_;
    BtnRect version_panel_rect_;
    std::vector<InfoLine> version_lines_;
    int version_scroll_offset_ = 0;
    void draw_version_panel();

    // Log panel (slides out to the right of phone frame)
    bool log_panel_visible_ = false;
    bool log_panel_animating_ = false;
    float log_panel_anim_ = 0.0f; // 0=hidden, 1=fully visible
    std::chrono::steady_clock::time_point log_panel_anim_start_;
    int log_scroll_offset_ = 0;
    uint64_t log_last_version_ = 0;
    BtnRect log_btn_; // right bezel star button
    bool log_scrollbar_dragging_ = false;
    int log_sb_track_y_ = 0, log_sb_track_h_ = 0;
    int log_sb_thumb_h_ = 0;
    int log_sb_max_scroll_ = 0;
    void draw_log_panel();

    // Frame position
    int frame_dst_x_ = 0, frame_dst_y_ = 0;
    int frame_dst_w_ = 0, frame_dst_h_ = 0;

    std::string screenshot_dir_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reset_requested_{false};
};

} // namespace openmirror::media
