#pragma once

#include <opm/media/decoder.h>
#include <opm/media/phone_frame.h>
#include <opm/media/recorder.h>
#include <opm/settings.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

namespace opm::media {

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

    // Kicks off a background GitHub-releases poll. When `show_when_up_to_date`
    // is true (manual check from the Settings panel) the renderer surfaces a
    // toast even when no newer version exists. On launch the App calls this
    // with false so a missing/blocked network is invisible to the user.
    void check_for_update_async(bool show_when_up_to_date);

    // PIN overlay (used by AirPlay PIN pairing). Pass an empty string to clear.
    void set_pin_code(const std::string& pin);

    // ---- Multi-source picker ----
    struct SourceEntry {
        std::string id;
        std::string name;
        bool active = false;
        bool streaming = false;
        bool paused = false;
    };
    using GetSourcesFn = std::function<std::vector<SourceEntry>()>;
    using SetActiveFn = std::function<void(const std::string& id)>;
    using DisconnectFn = std::function<void(const std::string& id)>;
    void set_source_provider(GetSourcesFn get, SetActiveFn set,
                             DisconnectFn disc = {}) {
        get_sources_fn_ = std::move(get);
        set_active_source_fn_ = std::move(set);
        disconnect_source_fn_ = std::move(disc);
    }

    // Triggered by the user pressing 'A' on the idle screen — App pops up a
    // pair/connect dialog and starts the Android receiver.
    using AddAndroidFn = std::function<void()>;
    void set_add_android_callback(AddAndroidFn cb) { add_android_fn_ = std::move(cb); }

    // Connect dialog driven by the renderer (so the styling matches the app).
    // Returns success status string when the worker finishes; the renderer
    // only calls this on the main thread, so the App's implementation must
    // either return quickly or hand off to a worker (current impl does).
    // Connect dialog driven by the renderer (so the styling matches the app).
    // Returns success status string when the worker finishes; the renderer
    // only calls this on the main thread, so the App's implementation must
    // either return quickly or hand off to a worker (current impl does).
    // `connect_port` is optional — when non-empty it bypasses adb's mDNS
    // discovery (which is unreliable on networks that block multicast)
    // and connects directly to <ip>:<connect_port>.
    using AndroidConnectFn = std::function<std::string(
        const std::string& ip, const std::string& pair_port,
        const std::string& pin, const std::string& connect_port)>;
    using AndroidDisconnectFn = std::function<void()>;
    void set_android_handlers(AndroidConnectFn connect, AndroidDisconnectFn disc) {
        android_connect_fn_ = std::move(connect);
        android_disconnect_fn_ = std::move(disc);
    }

    // Background discovery of Android Wireless-debug endpoints via the
    // adb daemon's own mDNS browser. Returns one entry per device that is
    // currently visible on the LAN — each may carry a connect port,
    // a pair port (only while the phone's "Pair device with pairing
    // code" popup is open), or both. The renderer polls this from a
    // worker thread while the connect panel is visible and renders the
    // results as clickable rows that pre-fill the form.
    struct DiscoveredAndroidDevice {
        std::string label;        // e.g. "adb-XXXXXX-YYYY"
        std::string ip;           // "192.168.10.73"
        std::string connect_port; // "46029" — empty if only pairing
        std::string pair_port;    // "42379" — empty if only connect
    };
    using AndroidDiscoverFn = std::function<std::vector<DiscoveredAndroidDevice>()>;
    void set_android_discover_callback(AndroidDiscoverFn fn) {
        android_discover_fn_ = std::move(fn);
    }

    void show_android_panel(); // open from outside if you want

private:
    void render_frame();
    void draw_island();
    void draw_footer(int svx, int svy, int svw, int svh);
    void draw_info_panel();
    // Returns a "phone-equivalent" reference width for sizing overlay UI
    // (panels, popup menus, tooltips, swatches, footer/help dialogs) so
    // they stay compact regardless of source dimensions. When mirroring a
    // tablet/desktop the screen viewport (svw) is much wider than a phone,
    // and dividing fonts/buttons by `svw / N` bloats every overlay. This
    // helper collapses the source to a phone-equivalent width and applies
    // an absolute cap.
    int ui_ref_width() const;
    void take_screenshot();
    void open_screenshot_folder();
    // Recording lifecycle helpers (called only from the renderer thread).
    void start_recording();
    void stop_recording();
    std::string make_recording_path() const;
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
    // Recording state. record_toggle_requested_ is set by the hotkey/click
    // handler and consumed in render_frame() so encoder init runs on the
    // renderer thread (same place as screenshot capture).
    Recorder recorder_;
    bool record_toggle_requested_ = false;
    // Optional countdown before recording starts (right-click menu "Start
    // in 5s"). When > 0, render_frame() shows a countdown HUD and starts
    // recording when it reaches 0.
    int  record_countdown_ms_ = 0;
    std::chrono::steady_clock::time_point record_countdown_start_;
    // Optional fixed-length recording (5/10/15 s). 0 = open-ended.
    int  pending_record_duration_sec_ = 0;
    bool window_shape_set_ = false;
    // Set when the user picks a different source from the bezel dots so
    // the next texture-resize event regenerates the phone frame and
    // resizes the window to match the new source's aspect (otherwise the
    // canvas-change branch keeps the old frame and letterboxes).
    bool source_just_switched_ = false;
    int  window_shape_last_lp_w_ = -1; // pixel width of drawer region when last applied
    int  window_shape_last_frame_w_ = -1;
    int  window_shape_last_frame_x_ = -1;
    int  window_shape_last_frame_y_ = -1;
    bool first_render_ = true;

    // Button hit rects
    struct BtnRect { int x = 0, y = 0, w = 0, h = 0; };
    BtnRect close_btn_, screenshot_btn_, folder_btn_, icon_btn_, info_btn_, menu_btn_, settings_btn_;
    // Mini screenshot button drawn in the top bezel when the island menu
    // is hidden, so users still have a visible affordance for Ctrl+S.
    BtnRect bezel_screenshot_btn_;
    // Mini close (X) button drawn in the top bezel when the island menu is
    // hidden — same stroke aesthetic as the menu/log chevrons.
    BtnRect bezel_close_btn_;
    // Bezel record button — sits to the right of the screenshot button in
    // the menu strip and (when collapsed) in the top bezel. Same hit-rect
    // pattern as bezel_screenshot_btn_.
    BtnRect record_btn_;
    BtnRect bezel_record_btn_;
    int hover_btn_ = -1; // -1=none, 0=close, 1=screenshot, 2=folder, 3=icon, 4=info, 5=menu, 6=settings

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

    // Source picker (small numbered dots in bottom bezel)
    GetSourcesFn get_sources_fn_;
    SetActiveFn set_active_source_fn_;
    DisconnectFn disconnect_source_fn_;
    AddAndroidFn add_android_fn_;
    std::vector<std::pair<std::string, BtnRect>> source_btns_;    // Right-click popup menu over any bezel button.
    // target is a string id: "menu", "log", or "src:<source-id>".
    bool bezel_menu_visible_ = false;
    std::string bezel_menu_target_;
    int bezel_menu_anchor_x_ = 0;
    int bezel_menu_anchor_y_ = 0;
    // (action_id, hit-rect) pairs computed during draw, consumed by clicks.
    std::vector<std::pair<std::string, BtnRect>> bezel_menu_items_;
    // Slide-in animation, matching info/version panels (200 ms ease-out).
    float bezel_menu_anim_ = 0.0f;
    bool bezel_menu_animating_ = false;
    std::chrono::steady_clock::time_point bezel_menu_anim_start_;

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
    // Per-toast lifetime in ms. Defaults to 1500 (the original hard-coded
    // value used by all transient confirmations like "Saved to Pictures").
    // Long messages such as the on-launch update prompt bump this up.
    int toast_duration_ms_ = 1500;
    void show_toast(const std::string& text, int duration_ms = 1500);

    // Tooltip
    SDL_Texture* tooltip_tex_ = nullptr;
    int tooltip_tex_w_ = 0, tooltip_tex_h_ = 0;
    int last_tooltip_btn_ = -1;

    // Hover delay (1 second) for tooltips. Tracks which UI key (string id)
    // is currently hovered and when that hover started.
    std::string hover_key_;
    std::chrono::steady_clock::time_point hover_start_;
    // Returns true if the cursor has been hovering `key` for at least 1s.
    bool tooltip_ready(const std::string& key);
    // Draw a small tooltip pill near (anchor_x, anchor_y). The tip is drawn
    // above the anchor unless it would clip the window top.
    void draw_bezel_tooltip(const std::string& text, int anchor_x, int anchor_y,
                            bool prefer_below = false);
    SDL_Texture* bezel_tip_tex_ = nullptr;
    int bezel_tip_w_ = 0, bezel_tip_h_ = 0;
    std::string bezel_tip_str_;
    int bezel_tip_font_h_ = 0;
    // Optional second line, rendered smaller and dimmer below the main
    // text. The full string passed in may contain a single '\n' to split.
    SDL_Texture* bezel_tip_tex2_ = nullptr;
    int bezel_tip_w2_ = 0, bezel_tip_h2_ = 0;

    // Logo texture (loaded from PNG)
    SDL_Texture* logo_texture_ = nullptr;
    int logo_tex_w_ = 0, logo_tex_h_ = 0;
    void load_logo_texture();

    // Waiting screen
    SDL_Texture* waiting_tex_ = nullptr;
    int waiting_tex_w_ = 0, waiting_tex_h_ = 0;

    // Per-device "waiting for screen updates from <name>" overlay shown
    // briefly after the user switches active source. Cleared on the next
    // incoming frame.
    std::string pending_source_name_;
    std::string pending_overlay_text_; // text currently baked into texture
    SDL_Texture* pending_overlay_tex_ = nullptr;
    int pending_overlay_w_ = 0, pending_overlay_h_ = 0;

    // Protected/blank-content detector. The Android capture path returns
    // an all-black surface for any window with FLAG_SECURE (lock screen,
    // banking apps, DRM video, MDM-restricted documents). The framework
    // hides the cause from us, so we heuristically detect a sustained
    // near-black frame and surface a non-modal overlay.
    std::chrono::steady_clock::time_point black_frame_since_;
    bool black_frame_active_ = false;     // currently in a black streak
    bool protected_overlay_visible_ = false;
    SDL_Texture* protected_overlay_tex_ = nullptr;
    int protected_overlay_w_ = 0, protected_overlay_h_ = 0;
    bool is_frame_near_black() const;

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
    std::vector<FooterSeg> footer_line1_, footer_line2_, footer_line3_;
    // Same content as the waiting-screen footer, but baked at the
    // smaller font size used by the Info panel's network-requirement
    // lines so the visual weight matches the surrounding text.
    std::vector<FooterSeg> info_footer_line1_, info_footer_line2_, info_footer_line3_;
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
    // Clickable "Copy network test script" button at the bottom of the
    // Info panel — copies a PowerShell troubleshooting script (firewall
    // rules + listening ports) to the clipboard.
    BtnRect info_copy_ps_btn_;
    // "Check for updates" button at the very bottom of the Info panel.
    // Same visual style as the "Copy network test script" button.
    BtnRect info_check_btn_;
    struct InfoLine {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::vector<InfoLine> info_lines_;
    // Small "About" header rendered above the footer block in the Info
    // panel. Same point size / colour as the "Network requirements"
    // section header so it visually pairs with it.
    InfoLine info_about_header_;

    // Version panel
    bool version_panel_visible_ = false;
    bool version_panel_animating_ = false;
    float version_panel_anim_ = 0.0f; // 0=hidden, 1=fully visible
    std::chrono::steady_clock::time_point version_panel_anim_start_;
    BtnRect version_panel_rect_;
    std::vector<InfoLine> version_lines_;
    int version_scroll_offset_ = 0;
    void draw_version_panel();

    // Settings panel (gear icon in island)
    opm::Settings settings_;
    bool settings_panel_visible_ = false;
    bool settings_panel_animating_ = false;
    float settings_panel_anim_ = 0.0f;
    std::chrono::steady_clock::time_point settings_panel_anim_start_;
    BtnRect settings_panel_rect_;
    // Hit rects computed during draw, consumed on click.
    std::vector<std::pair<int /*preset_index*/, BtnRect>> settings_swatch_btns_;
    BtnRect settings_toggle_save_btn_;
    BtnRect settings_toggle_clip_btn_;
    BtnRect settings_toggle_snagit_btn_;
    BtnRect settings_toggle_compname_btn_;
    BtnRect settings_toggle_aot_btn_;
    BtnRect settings_toggle_telemetry_btn_;
    // Session-only toggle: when on, std::cout is mirrored to
    // <screenshot_dir>/1PhoneMirror.log. Reset to false on every launch
    // (intentionally NOT persisted to settings.ini).
    BtnRect settings_toggle_log_btn_;
    bool log_to_file_session_ = false;
    BtnRect settings_fmt_mp4_btn_;
    BtnRect settings_fmt_gif_btn_;
    // Update-check state (the manual "Check for updates" link lives in
    // the Info panel footer, not the Settings panel anymore).
    std::atomic<bool> update_check_in_progress_{false};
    // Last-known release URL (filled by the update checker). Clicking the
    // "Update available" toast/banner will open this in the browser.
    std::mutex update_check_mutex_;
    std::string update_latest_version_;
    std::string update_release_url_;

    // Persistent two-line "Update available" banner (replaces the old
    // single-line toast). Line 1: version notice. Line 2: clickable
    // GitHub repo link with the same hover/tooltip behaviour as the
    // footer links. Stays up until the user clicks the link or the close
    // glyph.
    bool update_banner_active_ = false;
    std::chrono::steady_clock::time_point update_banner_start_;
    BtnRect update_link_rect_;
    BtnRect update_close_rect_;
    SDL_Texture* update_line1_tex_ = nullptr;
    int update_line1_w_ = 0, update_line1_h_ = 0;
    SDL_Texture* update_link_tex_ = nullptr;
    int update_link_w_ = 0, update_link_h_ = 0;
    std::string update_line1_cached_;
    void draw_update_banner();

    void draw_settings_panel();
    void apply_bezel_color(uint8_t r, uint8_t g, uint8_t b);
    // Drawer / sub-panel base colour derived from the current bezel colour
    // so the log panel always contrasts cleanly against the phone body.
    void drawer_color(uint8_t& r, uint8_t& g, uint8_t& b) const;

    // Log panel (slides out to the right of phone frame)
    bool log_panel_visible_ = false;
    bool log_panel_animating_ = false;
    float log_panel_anim_ = 0.0f; // 0=hidden, 1=fully visible
    std::chrono::steady_clock::time_point log_panel_anim_start_;
    int log_scroll_offset_ = 0;
    int log_panel_full_w_ = 0; // dynamic full-open width (px), recomputed each frame
    uint64_t log_last_version_ = 0;
    // Cached wrapped+rasterized log rows. Rebuilt only when the log version,
    // font size, or full-open panel width changes — so during the slide
    // animation we just re-clip to the visible drawer width instead of
    // re-wrapping and re-rasterizing every frame.
    struct LogRowCache {
        std::string text;
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        uint8_t r = 200, g = 200, b = 200;
    };
    std::vector<LogRowCache> log_row_cache_;
    uint64_t log_cache_version_ = 0;
    int log_cache_font_sz_ = 0;
    int log_cache_full_w_ = 0;
    int log_cache_total_h_ = 0;
    void clear_log_row_cache();
    BtnRect log_btn_; // right bezel star button
    bool log_scrollbar_dragging_ = false;
    int log_sb_track_y_ = 0, log_sb_track_h_ = 0;
    int log_sb_thumb_h_ = 0;
    int log_sb_max_scroll_ = 0;
    void draw_log_panel();

    // Frame position
    int frame_dst_x_ = 0, frame_dst_y_ = 0;
    int frame_dst_w_ = 0, frame_dst_h_ = 0;

    // PIN overlay (AirPlay PIN pairing)
    std::mutex pin_mutex_;
    std::string pin_code_;
    SDL_Texture* pin_label_tex_ = nullptr;
    int pin_label_w_ = 0, pin_label_h_ = 0;
    SDL_Texture* pin_digits_tex_ = nullptr;
    int pin_digits_w_ = 0, pin_digits_h_ = 0;
    std::string pin_digits_cached_;
    SDL_Texture* pin_note_tex_ = nullptr;
    int pin_note_w_ = 0, pin_note_h_ = 0;
    void draw_pin_overlay();

    std::string screenshot_dir_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reset_requested_{false};

    // ---- Screenshot annotation overlay (Ctrl+Shift+S) ----
    // Modal editor: freezes the current composited screenshot, lets the
    // user mark it up with arrow / rectangle / highlight / pixelate
    // strokes, then routes the baked PNG through the same save +
    // clipboard pipeline as a plain Ctrl+S.
    enum class AnnoTool { Arrow, Rect, Highlight, Pixelate, Text };
    struct AnnoStroke {
        AnnoTool tool = AnnoTool::Arrow;
        // Image-space coordinates (in the captured RGBA buffer).
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        // Stroke colour. Pixelate ignores this.
        uint8_t r = 235, g = 80, b = 80;
        // Line thickness in image-space pixels. Used by Arrow + Rect.
        int thickness = 8;
        // Pre-computed mosaic block colours for Pixelate strokes.
        // Layout: row-major, `pix_cols` × `pix_rows` of packed RGB.
        std::vector<uint32_t> pix_blocks;
        int pix_cols = 0, pix_rows = 0;
        int pix_block_size = 0;
        int pix_origin_x = 0, pix_origin_y = 0;
        // Text strokes only: the typed string and target glyph height in
        // image-space pixels. text_tex is owned by the renderer's stroke
        // list and freed in end_annotation().
        std::string text;
        int font_px = 0;
        SDL_Texture* text_tex = nullptr;
        int text_w = 0, text_h = 0;
    };
    bool annotator_active_ = false;
    SDL_Texture* annotator_bg_tex_ = nullptr;
    std::vector<uint8_t> annotator_bg_orig_;   // original captured RGBA
    int annotator_bg_w_ = 0, annotator_bg_h_ = 0;
    std::vector<AnnoStroke> annotator_strokes_;
    AnnoTool annotator_tool_ = AnnoTool::Arrow;
    int annotator_color_idx_ = 0;              // index into k_anno_palette
    int annotator_line_width_ = 8;             // image-space px (1..32)
    bool annotator_drawing_ = false;
    int annotator_drag_x0_ = 0, annotator_drag_y0_ = 0;
    int annotator_drag_x1_ = 0, annotator_drag_y1_ = 0;
    // Text-input mode (Text tool): user clicked, now typing into a buffer
    // anchored at (text_x_, text_y_) in image space. Rendered live; not
    // committed as a stroke until Enter or click-elsewhere.
    bool annotator_text_input_active_ = false;
    std::string annotator_text_buf_;
    int annotator_text_x_ = 0, annotator_text_y_ = 0;
    int annotator_text_font_px_ = 0;
    SDL_Texture* annotator_text_preview_tex_ = nullptr;
    int annotator_text_preview_w_ = 0, annotator_text_preview_h_ = 0;
    std::string annotator_text_preview_cached_; // last text used for cache
    // Slider drag state for the line-width control.
    bool annotator_slider_drag_ = false;
    // Display mapping for image-to-window coords (recomputed each frame).
    int annotator_dst_x_ = 0, annotator_dst_y_ = 0;
    int annotator_dst_w_ = 0, annotator_dst_h_ = 0;
    // Toolbar hit rects (recomputed each frame).
    BtnRect anno_btn_arrow_, anno_btn_rect_, anno_btn_highlight_, anno_btn_pixelate_, anno_btn_text_;
    BtnRect anno_slider_rect_;
    BtnRect anno_swatch_btns_[5];
    BtnRect anno_btn_undo_, anno_btn_save_, anno_btn_cancel_;
    void begin_annotation();
    void end_annotation();
    void save_annotated();
    void draw_annotator();
    bool handle_annotator_event(const SDL_Event& ev); // returns true if consumed
    void bake_pixelate_stroke(AnnoStroke& s) const;
    void rasterize_strokes_to(uint8_t* rgba, int w, int h, int stride) const;
    void commit_text_stroke();
    void rebuild_text_preview();

    // ---- OCR copy (Ctrl+Shift+T) ----
    // Modal region picker over the same captured composite the annotator
    // uses. On mouse-up the cropped RGBA is shipped to a worker thread
    // running Windows.Media.Ocr; the renderer polls `ocr_result_pending_`
    // each frame, then writes the recognised text to the clipboard.
    bool ocr_active_ = false;
    SDL_Texture* ocr_bg_tex_ = nullptr;
    std::vector<uint8_t> ocr_bg_rgba_;          // packed RGBA, no padding
    int ocr_bg_w_ = 0, ocr_bg_h_ = 0;
    int ocr_dst_x_ = 0, ocr_dst_y_ = 0;
    int ocr_dst_w_ = 0, ocr_dst_h_ = 0;
    bool ocr_drawing_ = false;
    int ocr_drag_x0_ = 0, ocr_drag_y0_ = 0;     // image-space
    int ocr_drag_x1_ = 0, ocr_drag_y1_ = 0;
    std::atomic<bool> ocr_running_{false};
    std::mutex ocr_result_mu_;
    bool ocr_result_pending_ = false;
    bool ocr_result_ok_ = false;
    std::string ocr_result_text_;
    std::string ocr_result_error_;
    void begin_ocr();
    void end_ocr();
    void draw_ocr_overlay();
    bool handle_ocr_event(const SDL_Event& ev); // returns true if consumed
    void launch_ocr_job(int ix, int iy, int iw, int ih);
    void process_ocr_result();

    // Reset window to the default first-launch size based on the active
    // device's frame aspect ratio. Shared by Ctrl+0 and the right-click
    // "Reset to default size" menu on the resize grip.
    void reset_window_to_default_size();

    // Android connect panel (in-app, themed to match info panel)
    bool android_panel_visible_ = false;
    bool android_panel_animating_ = false;
    float android_panel_anim_ = 0.0f;
    std::chrono::steady_clock::time_point android_panel_anim_start_;
    int android_focus_ = 0;            // 0=ip, 1=port, 2=pin, 3=connect_port
    std::string android_ip_;
    std::string android_port_;
    std::string android_pin_;
    std::string android_connect_port_;
    std::string android_status_;
    bool android_status_is_error_ = false;
    bool android_busy_ = false;
    std::mutex android_status_mutex_;
    BtnRect android_panel_rect_;
    BtnRect android_field_rects_[4]{};
    BtnRect android_connect_btn_;
    BtnRect android_disconnect_btn_;
    BtnRect android_close_btn_;
    BtnRect android_help_btn_;
    BtnRect android_help_close_btn_;
    BtnRect android_help_panel_rect_;
    bool android_help_visible_ = false;
    int  android_help_scroll_ = 0;       // pixels scrolled down
    int  android_help_max_scroll_ = 0;   // computed each draw
    BtnRect android_help_track_rect_{};  // scrollbar track hit area
    BtnRect android_help_thumb_rect_{};  // scrollbar thumb hit area
    bool android_help_dragging_ = false; // mouse-drag on scrollbar thumb
    int  android_help_drag_offset_ = 0;
    AndroidConnectFn android_connect_fn_;
    AndroidDisconnectFn android_disconnect_fn_;
    void draw_android_panel();
    void draw_android_help();
    void android_submit();

    // Background mDNS discovery for the connect panel.
    AndroidDiscoverFn android_discover_fn_;
    std::vector<DiscoveredAndroidDevice> android_discovered_;
    std::mutex android_discovered_mutex_;
    std::thread android_discover_thread_;
    std::atomic<bool> android_discover_running_{false};
    std::atomic<bool> android_discover_in_progress_{false};
    std::vector<BtnRect> android_discover_btns_; // hit rects per row
    void start_android_discovery();
    void stop_android_discovery();
};

} // namespace opm::media
