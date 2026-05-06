#include <openmirror/media/renderer.h>
#include <openmirror/log_buffer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>

#ifdef _WIN32
#include <shlobj.h>
#include <shellapi.h>
#endif

extern "C" {
    int stbi_write_png(const char* filename, int w, int h, int comp,
                       const void* data, int stride_in_bytes);
    int stbi_write_png_to_func(void (*func)(void*, void*, int), void* context,
                               int w, int h, int comp, const void* data,
                               int stride_in_bytes);
    unsigned char* stbi_load(const char* filename, int* x, int* y,
                             int* channels_in_file, int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
}

// ---------------------------------------------------------------------------
// GDI text helpers → SDL_Texture  (Segoe UI, anti-aliased)
// ---------------------------------------------------------------------------
#ifdef _WIN32

// ANSI version
static SDL_Texture* make_text_texture(SDL_Renderer* renderer, const std::string& text,
                                       int font_height, uint8_t cr, uint8_t cg, uint8_t cb,
                                       int* out_w, int* out_h) {
    if (text.empty()) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT font = CreateFontA(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, font);

    SIZE sz;
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz.cx;
    bmi.bmiHeader.biHeight = -sz.cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);

    memset(bits, 0, sz.cx * sz.cy * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();

    // ClearType renders sub-pixel RGB. Combine into luminance-based alpha.
    auto* src = static_cast<uint8_t*>(bits);
    std::vector<uint8_t> rgba(sz.cx * sz.cy * 4);
    for (int i = 0; i < sz.cx * sz.cy; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        rgba[i * 4 + 0] = cr;
        rgba[i * 4 + 1] = cg;
        rgba[i * 4 + 2] = cb;
        rgba[i * 4 + 3] = alpha;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz.cx, sz.cy);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz.cx * 4);
    }
    *out_w = sz.cx;
    *out_h = sz.cy;

    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return tex;
}

// Wide/Unicode version (for ♫, ©, etc.)
static SDL_Texture* make_text_texture_w(SDL_Renderer* renderer, const std::wstring& text,
                                          int font_height, uint8_t cr, uint8_t cg, uint8_t cb,
                                          int* out_w, int* out_h) {
    if (text.empty()) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT font = CreateFontW(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, font);

    SIZE sz;
    GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz.cx;
    bmi.bmiHeader.biHeight = -sz.cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);

    memset(bits, 0, sz.cx * sz.cy * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();

    auto* src = static_cast<uint8_t*>(bits);
    std::vector<uint8_t> rgba(sz.cx * sz.cy * 4);
    for (int i = 0; i < sz.cx * sz.cy; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        rgba[i * 4 + 0] = cr;
        rgba[i * 4 + 1] = cg;
        rgba[i * 4 + 2] = cb;
        rgba[i * 4 + 3] = alpha;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz.cx, sz.cy);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz.cx * 4);
    }
    *out_w = sz.cx;
    *out_h = sz.cy;

    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return tex;
}

#endif // _WIN32

static void png_write_vec(void* context, void* data, int size) {
    auto* vec = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<const uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

static bool in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void fill_circle(SDL_Renderer* r, int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)std::sqrt(radius * radius - dy * dy);
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// ---------------------------------------------------------------------------
// Platform icon generators (128px monochrome, downscaled for crispness)
// ---------------------------------------------------------------------------
static SDL_Texture* create_apple_icon(SDL_Renderer* renderer, int sz,
                                       uint8_t cr, uint8_t cg, uint8_t cb) {
    std::vector<uint8_t> rgba(sz * sz * 4, 0);
    float sc = sz / 64.0f;
    for (int py = 0; py < sz; py++) {
        for (int px = 0; px < sz; px++) {
            float x = px / sc, y = py / sc;
            bool filled = false;
            // Left body lobe
            if ((x-26)*(x-26) + (y-38)*(y-38) <= 16*16) filled = true;
            // Right body lobe
            if ((x-38)*(x-38) + (y-38)*(y-38) <= 16*16) filled = true;
            // Bottom fill
            if ((x-32)*(x-32) + (y-42)*(y-42) <= 14*14) filled = true;
            // Bite cutout (right side)
            if ((x-52)*(x-52) + (y-30)*(y-30) <= 11*11) filled = false;
            // Top cutoff
            if (y < 23) filled = false;
            // Stem
            if (x >= 31 && x <= 33.5f && y >= 12 && y <= 23) filled = true;
            // Leaf (tilted ellipse)
            float lx = (x-37)*0.87f + (y-16)*0.5f;
            float ly = -(x-37)*0.5f + (y-16)*0.87f;
            if ((lx*lx)/(6*6) + (ly*ly)/(3*3) <= 1) filled = true;

            if (filled) {
                int idx = (py * sz + px) * 4;
                rgba[idx+0] = cr; rgba[idx+1] = cg; rgba[idx+2] = cb; rgba[idx+3] = 255;
            }
        }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz, sz);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz * 4);
    }
    return tex;
}

static SDL_Texture* create_android_icon(SDL_Renderer* renderer, int sz,
                                          uint8_t cr, uint8_t cg, uint8_t cb) {
    std::vector<uint8_t> rgba(sz * sz * 4, 0);
    float sc = sz / 64.0f;
    for (int py = 0; py < sz; py++) {
        for (int px = 0; px < sz; px++) {
            float x = px / sc, y = py / sc;
            bool filled = false;
            // Head dome (top half of circle)
            if ((x-32)*(x-32) + (y-38)*(y-38) <= 18*18 && y <= 38) filled = true;
            // Head flat bottom
            if (x >= 14 && x <= 50 && y > 38 && y <= 46) filled = true;
            // Eye cutouts
            if ((x-24)*(x-24) + (y-33)*(y-33) <= 3*3) filled = false;
            if ((x-40)*(x-40) + (y-33)*(y-33) <= 3*3) filled = false;
            // Left antenna
            {
                float ax=21, ay=14, bx=25, by=22;
                float dx=bx-ax, dy=by-ay, len2=dx*dx+dy*dy;
                float t = std::clamp(((x-ax)*dx + (y-ay)*dy) / len2, 0.0f, 1.0f);
                float px2=ax+t*dx, py2=ay+t*dy;
                if ((x-px2)*(x-px2) + (y-py2)*(y-py2) <= 2.2f*2.2f) filled = true;
                if ((x-ax)*(x-ax) + (y-ay)*(y-ay) <= 2.5f*2.5f) filled = true;
            }
            // Right antenna
            {
                float ax=43, ay=14, bx=39, by=22;
                float dx=bx-ax, dy=by-ay, len2=dx*dx+dy*dy;
                float t = std::clamp(((x-ax)*dx + (y-ay)*dy) / len2, 0.0f, 1.0f);
                float px2=ax+t*dx, py2=ay+t*dy;
                if ((x-px2)*(x-px2) + (y-py2)*(y-py2) <= 2.2f*2.2f) filled = true;
                if ((x-ax)*(x-ax) + (y-ay)*(y-ay) <= 2.5f*2.5f) filled = true;
            }

            if (filled) {
                int idx = (py * sz + px) * 4;
                rgba[idx+0] = cr; rgba[idx+1] = cg; rgba[idx+2] = cb; rgba[idx+3] = 255;
            }
        }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz, sz);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz * 4);
    }
    return tex;
}

namespace openmirror::media {

Renderer::Renderer() = default;

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(const std::string& title, int /*width*/, int /*height*/) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "[Renderer] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    window_ = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        400, 800,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS
    );
    if (!window_) {
        std::cerr << "[Renderer] Failed to create window: " << SDL_GetError() << "\n";
        return false;
    }

    sdl_renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer_) {
        std::cerr << "[Renderer] Failed to create renderer: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window_); window_ = nullptr;
        return false;
    }
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    load_icon_texture();
    load_logo_texture();

    phone_frame_.generate(sdl_renderer_, 390, 844);

    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    int fw = phone_frame_.frame_width();
    int fh = phone_frame_.frame_height();
    float s = std::min(dm.w * 0.32f / fw, dm.h * 0.65f / fh);
    SDL_SetWindowSize(window_, (int)(fw * s), (int)(fh * s));
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    // Set window icon for taskbar
#ifdef _WIN32
    {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window_, &info)) {
            HICON hIcon = LoadIconA(GetModuleHandle(nullptr), MAKEINTRESOURCE(101));
            if (hIcon) {
                SendMessage(info.info.win.window, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessage(info.info.win.window, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }
        }
    }
#endif

    // Pre-render waiting text
#ifdef _WIN32
    waiting_tex_ = make_text_texture(sdl_renderer_, "Waiting for connection...",
                                      48, 180, 180, 180, &waiting_tex_w_, &waiting_tex_h_);
    ios_instr_tex_ = make_text_texture(sdl_renderer_,
        "Control Center > Screen Mirroring > 1PhoneMirror",
        32, 130, 130, 130, &ios_instr_w_, &ios_instr_h_);
    android_instr_tex_ = make_text_texture(sdl_renderer_,
        "Quick Settings > Cast / Screen Cast > 1PhoneMirror",
        32, 130, 130, 130, &android_instr_w_, &android_instr_h_);
    // Load platform icons from PNG files
    {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::string icon_dir(exe_path);
        auto sep = icon_dir.find_last_of("\\/");
        if (sep != std::string::npos) icon_dir = icon_dir.substr(0, sep);

        int iw = 0, ih = 0, ic = 0;
        unsigned char* apple_data = stbi_load((icon_dir + "\\apple_icon.png").c_str(), &iw, &ih, &ic, 4);
        if (apple_data) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
            ios_icon_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC, iw, ih);
            if (ios_icon_tex_) {
                SDL_SetTextureBlendMode(ios_icon_tex_, SDL_BLENDMODE_BLEND);
                SDL_UpdateTexture(ios_icon_tex_, nullptr, apple_data, iw * 4);
                ios_icon_sz_ = iw;
            }
            stbi_image_free(apple_data);
        } else {
            ios_icon_tex_ = create_apple_icon(sdl_renderer_, 128, 130, 130, 130);
            ios_icon_sz_ = 128;
        }

        unsigned char* android_data = stbi_load((icon_dir + "\\android_icon.png").c_str(), &iw, &ih, &ic, 4);
        if (android_data) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
            android_icon_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                                   SDL_TEXTUREACCESS_STATIC, iw, ih);
            if (android_icon_tex_) {
                SDL_SetTextureBlendMode(android_icon_tex_, SDL_BLENDMODE_BLEND);
                SDL_UpdateTexture(android_icon_tex_, nullptr, android_data, iw * 4);
                android_icon_sz_ = iw;
            }
            stbi_image_free(android_data);
        } else {
            android_icon_tex_ = create_android_icon(sdl_renderer_, 128, 130, 130, 130);
            android_icon_sz_ = 128;
        }
    }
#endif

    // Footer segments for waiting screen
#ifdef _WIN32
    {
        auto seg = [&](const std::wstring& text, uint8_t r, uint8_t g, uint8_t b,
                        const std::string& url = "", const std::string& tip = "") -> FooterSeg {
            FooterSeg fs;
            fs.url = url;
            fs.tooltip = tip;
            fs.tex = make_text_texture_w(sdl_renderer_, text, 40, r, g, b, &fs.w, &fs.h);
            return fs;
        };
        // Line 1: "1PhoneMirror by MSEndpointMgr"
        footer_line1_.push_back(seg(L"1PhoneMirror by ", 120, 120, 120));
        footer_line1_.push_back(seg(L"MSEndpointMgr", 120, 120, 120,
                                     "https://msendpointmgr.com/", "Open MSEndpointMgr"));
        // Line 2: "© 2026 ♫ Simon Skotheimsvik, MVP · v0.1.0"
        footer_line2_.push_back(seg(L"\u00A9 2026 ", 100, 100, 100));
        footer_line2_.push_back(seg(L"\u266B", 100, 100, 100,
                                     "", "Tuned carefully for the community"));
        footer_line2_.push_back(seg(L" ", 100, 100, 100));
        footer_line2_.push_back(seg(L"Simon Skotheimsvik, MVP", 100, 100, 100,
                                     "https://linktr.ee/simonskotheimsvik", "More info of Simon"));
        footer_line2_.push_back(seg(L" \u00B7 v0.2.2", 100, 100, 100,
                                     "", "Version history"));
    }
#endif

    // Info panel content
#ifdef _WIN32
    {
        auto make_info = [&](const std::wstring& text, int font_sz,
                              uint8_t r, uint8_t g, uint8_t b) -> InfoLine {
            InfoLine line;
            line.tex = make_text_texture_w(sdl_renderer_, text, font_sz, r, g, b, &line.w, &line.h);
            return line;
        };
        info_lines_.push_back(make_info(L"1PhoneMirror v0.2.2", 44, 255, 255, 255));
        info_lines_.push_back(make_info(L"AirPlay \u00B7 Miracast \u00B7 Google Cast", 34, 160, 160, 160));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"F Fullscreen \u00B7 M Menu \u00B7 L Log", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"I Info \u00B7 V Version \u00B7 Ctrl+S Screenshot \u00B7 Esc Quit", 30, 130, 130, 130));
    }
#endif

    // Version panel content (newest first)
#ifdef _WIN32
    {
        auto make_ver = [&](const std::wstring& text, int font_sz,
                             uint8_t r, uint8_t g, uint8_t b) -> InfoLine {
            InfoLine line;
            line.tex = make_text_texture_w(sdl_renderer_, text, font_sz, r, g, b, &line.w, &line.h);
            return line;
        };
        version_lines_.push_back(make_ver(L"Version History", 40, 255, 255, 255));
        version_lines_.push_back({nullptr, 0, 0}); // spacer
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.2.2", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Sametime iOS", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.2.1", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"AirPlay Pin", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.1.4", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Log viewer", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"05.05.2026 \u2013 0.1.2", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Island menu", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"05.05.2026 \u2013 0.1.1", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Screenshot option", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"05.05.2026 \u2013 0.1.0", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"iOS AirPlay support", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"04.05.2026 \u2013 0.0.5", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Initial ideas and implementation", 30, 160, 160, 160));
    }
#endif

    // Screenshot folder
#ifdef _WIN32
    char pictures[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYPICTURES, nullptr, 0, pictures)))
        screenshot_dir_ = std::string(pictures) + "\\1PhoneMirror by MSEndpointMgr";
    else
        screenshot_dir_ = "screenshots";
#else
    screenshot_dir_ = "screenshots";
#endif

    std::cout << "[Renderer] Window initialized\n";
    return true;
}

void Renderer::shutdown() {
    running_.store(false);
    for (auto& s : footer_line1_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : footer_line2_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    footer_line1_.clear();
    footer_line2_.clear();
    if (footer_tooltip_tex_) { SDL_DestroyTexture(footer_tooltip_tex_); footer_tooltip_tex_ = nullptr; }
    if (toast_tex_) { SDL_DestroyTexture(toast_tex_); toast_tex_ = nullptr; }
    if (tooltip_tex_) { SDL_DestroyTexture(tooltip_tex_); tooltip_tex_ = nullptr; }
    if (bezel_tip_tex_) { SDL_DestroyTexture(bezel_tip_tex_); bezel_tip_tex_ = nullptr; }
    if (waiting_tex_) { SDL_DestroyTexture(waiting_tex_); waiting_tex_ = nullptr; }
    if (pin_label_tex_) { SDL_DestroyTexture(pin_label_tex_); pin_label_tex_ = nullptr; }
    if (pin_digits_tex_) { SDL_DestroyTexture(pin_digits_tex_); pin_digits_tex_ = nullptr; }
    if (icon_texture_) { SDL_DestroyTexture(icon_texture_); icon_texture_ = nullptr; }
    if (logo_texture_) { SDL_DestroyTexture(logo_texture_); logo_texture_ = nullptr; }
    if (ios_instr_tex_) { SDL_DestroyTexture(ios_instr_tex_); ios_instr_tex_ = nullptr; }
    if (android_instr_tex_) { SDL_DestroyTexture(android_instr_tex_); android_instr_tex_ = nullptr; }
    if (ios_icon_tex_) { SDL_DestroyTexture(ios_icon_tex_); ios_icon_tex_ = nullptr; }
    if (android_icon_tex_) { SDL_DestroyTexture(android_icon_tex_); android_icon_tex_ = nullptr; }
    for (auto& l : info_lines_) { if (l.tex) SDL_DestroyTexture(l.tex); }
    info_lines_.clear();
    for (auto& l : version_lines_) { if (l.tex) SDL_DestroyTexture(l.tex); }
    version_lines_.clear();
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
    if (sdl_renderer_) { SDL_DestroyRenderer(sdl_renderer_); sdl_renderer_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    SDL_Quit();
}

void Renderer::submit_frame(VideoFrame frame) {
    std::lock_guard lock(frame_mutex_);
    pending_frame_ = std::move(frame);
    has_new_frame_ = true;
}

void Renderer::load_icon_texture() {
#ifdef _WIN32
    HICON hIcon = (HICON)LoadImageA(GetModuleHandle(nullptr),
                                     MAKEINTRESOURCE(101), IMAGE_ICON,
                                     64, 64, LR_DEFAULTCOLOR);
    if (!hIcon) return;

    ICONINFO ii;
    if (!GetIconInfo(hIcon, &ii)) { DestroyIcon(hIcon); return; }

    BITMAP bm;
    GetObject(ii.hbmColor, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    std::vector<uint8_t> pixels(w * h * 4);
    HDC hdc = CreateCompatibleDC(nullptr);
    GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);
    DeleteDC(hdc);

    for (int i = 0; i < w * h; i++)
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);

    icon_texture_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (icon_texture_) {
        SDL_SetTextureBlendMode(icon_texture_, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(icon_texture_, nullptr, pixels.data(), w * 4);
        icon_tex_w_ = w;
        icon_tex_h_ = h;
    }

    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(hIcon);
#endif
}

void Renderer::load_logo_texture() {
#ifdef _WIN32
    // Find logo PNG next to executable
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string dir(exe_path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    std::string logo_path = dir + "\\MSEndpointMGRLogoBig.PNG";

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(logo_path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        std::cout << "[Renderer] Logo not found: " << logo_path << "\n";
        return;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    logo_texture_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (logo_texture_) {
        SDL_SetTextureBlendMode(logo_texture_, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(logo_texture_, nullptr, data, w * 4);
        logo_tex_w_ = w;
        logo_tex_h_ = h;
        std::cout << "[Renderer] Logo loaded: " << w << "x" << h << "\n";
    }
    stbi_image_free(data);
#endif
}

void Renderer::run() {
    running_.store(true);

    while (running_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running_.store(false);
                return;

            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running_.store(false);
                    return;
                }
                if (event.key.keysym.sym == SDLK_f) {
                    uint32_t flags = SDL_GetWindowFlags(window_);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window_, 0);
                        window_shape_set_ = false; // recalculate on next render
                    } else {
#ifdef _WIN32
                        SDL_SysWMinfo info; SDL_VERSION(&info.version);
                        if (SDL_GetWindowWMInfo(window_, &info))
                            SetWindowRgn(info.info.win.window, nullptr, TRUE);
#endif
                        SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
                        window_shape_set_ = false; // re-apply shape after layout
                    }
                }
                if (event.key.keysym.sym == SDLK_l) {
                    log_panel_visible_ = !log_panel_visible_;
                    log_panel_animating_ = true;
                    log_panel_anim_start_ = std::chrono::steady_clock::now();
                }
                if (event.key.keysym.sym == SDLK_m) {
                    island_visible_ = !island_visible_;
                    island_animating_ = true;
                    island_anim_start_ = std::chrono::steady_clock::now();
                }
                if (event.key.keysym.sym == SDLK_i) {
                    info_panel_visible_ = !info_panel_visible_;
                    info_panel_animating_ = true;
                    info_panel_anim_start_ = std::chrono::steady_clock::now();
                    if (info_panel_visible_ && version_panel_visible_) {
                        version_panel_visible_ = false;
                        version_panel_animating_ = true;
                        version_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                }
                if (event.key.keysym.sym == SDLK_v) {
                    version_panel_visible_ = !version_panel_visible_;
                    version_panel_animating_ = true;
                    version_panel_anim_start_ = std::chrono::steady_clock::now();
                    version_scroll_offset_ = 0;
                    if (version_panel_visible_ && info_panel_visible_) {
                        info_panel_visible_ = false;
                        info_panel_animating_ = true;
                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                }
                if (event.key.keysym.sym == SDLK_s && (event.key.keysym.mod & KMOD_CTRL)) {
                    screenshot_requested_ = true;
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    running_.store(false);
                    return;
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    int mx = event.button.x, my = event.button.y;
                    // Right-click on any bezel button opens its popup menu.
                    std::string target;
                    if (menu_btn_.w > 0 && in_rect(mx, my, menu_btn_.x, menu_btn_.y,
                                                    menu_btn_.w, menu_btn_.h)) {
                        target = "menu";
                    } else if (log_btn_.w > 0 && in_rect(mx, my, log_btn_.x, log_btn_.y,
                                                          log_btn_.w, log_btn_.h)) {
                        target = "log";
                    } else {
                        for (auto& [id, r] : source_btns_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                target = "src:" + id;
                                break;
                            }
                        }
                    }
                    if (!target.empty()) {
                        bezel_menu_visible_ = true;
                        bezel_menu_target_ = target;
                        bezel_menu_anchor_x_ = mx;
                        bezel_menu_anchor_y_ = my;
                        bezel_menu_items_.clear();
                        bezel_menu_animating_ = true;
                        bezel_menu_anim_start_ = std::chrono::steady_clock::now();
                        bezel_menu_anim_ = 0.0f;
                        // Hide any pending bezel hover tooltip while menu is up.
                        hover_key_.clear();
                    } else {
                        // Right-click elsewhere dismisses any open menu.
                        bezel_menu_visible_ = false;
                        bezel_menu_anim_ = 0.0f;
                    }
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int mx = event.button.x, my = event.button.y;

                    // Bezel popup menu — handle first if visible.
                    if (bezel_menu_visible_) {
                        std::string clicked_action;
                        for (auto& [act, r] : bezel_menu_items_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                clicked_action = act;
                                break;
                            }
                        }
                        std::string tgt = bezel_menu_target_;
                        bezel_menu_visible_ = false;
                        bezel_menu_anim_ = 0.0f;
                        bezel_menu_animating_ = false;
                        if (!clicked_action.empty()) {
                            if (clicked_action == "disconnect" &&
                                tgt.rfind("src:", 0) == 0 && disconnect_source_fn_) {
                                disconnect_source_fn_(tgt.substr(4));
                            } else if (clicked_action == "exit") {
                                running_.store(false);
                                return;
                            } else if (clicked_action == "copy") {
                                std::string all;
                                for (auto& ln : openmirror::LogBuffer::instance().get_lines()) {
                                    all += ln;
                                    all += '\n';
                                }
                                SDL_SetClipboardText(all.c_str());
                                std::cout << "[Renderer] Log copied to clipboard ("
                                          << all.size() << " bytes)\n";
                            }
                            break;
                        }
                        // Click outside menu dismisses it; fall through.
                    }

                    // Resize grip (check first, highest priority corner)
                    if (phone_frame_enabled_ && resize_grip_.w > 0 &&
                        in_rect(mx, my, resize_grip_.x, resize_grip_.y,
                                resize_grip_.w, resize_grip_.h)) {
                        resizing_ = true;
                        SDL_GetGlobalMouseState(&resize_start_gx_, &resize_start_gy_);
                        SDL_GetWindowSize(window_, &resize_start_w_, &resize_start_h_);
                        break;
                    }

                    // Source picker dots (bottom bezel) — switch active source
                    {
                        bool handled = false;
                        for (auto& [id, r] : source_btns_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                if (set_active_source_fn_) set_active_source_fn_(id);
                                handled = true;
                                break;
                            }
                        }
                        if (handled) break;
                    }

                    // Log scrollbar drag start
                    if (log_panel_visible_ && log_sb_track_h_ > 0 && log_sb_max_scroll_ > 0) {
                        int sb_w = std::max(6, log_sb_track_h_ / 80);
                        int sb_hit_w = sb_w + 12; // generous hit area
                        int lp_w = (int)(frame_dst_h_ * 0.55f * log_panel_anim_);
                        int lp_margin = std::max(4, frame_dst_h_ / 40);
                        int drawer_inset = frame_dst_h_ / 16;
                        int panel_x = frame_dst_x_ + frame_dst_w_;
                        int panel_w = lp_w - lp_margin;
                        int pad = std::max(6, std::max(8, panel_w / 30));
                        int sb_x = panel_x + panel_w - pad;
                        if (mx >= sb_x - sb_hit_w / 2 && mx <= sb_x + sb_w + sb_hit_w / 2 &&
                            my >= log_sb_track_y_ && my <= log_sb_track_y_ + log_sb_track_h_) {
                            log_scrollbar_dragging_ = true;
                            // Jump scroll to click position
                            float frac = (float)(my - log_sb_track_y_ - log_sb_thumb_h_ / 2) /
                                         (float)(log_sb_track_h_ - log_sb_thumb_h_);
                            frac = std::max(0.0f, std::min(1.0f, frac));
                            log_scroll_offset_ = (int)(frac * log_sb_max_scroll_);
                            break;
                        }
                    }

                    // Menu star toggle (island visibility)
                    if (menu_btn_.w > 0 &&
                        in_rect(mx, my, menu_btn_.x, menu_btn_.y,
                                menu_btn_.w, menu_btn_.h)) {
                        island_visible_ = !island_visible_;
                        island_animating_ = true;
                        island_anim_start_ = std::chrono::steady_clock::now();
                        break;
                    }

                    // Log star toggle (log panel visibility)
                    if (log_btn_.w > 0 &&
                        in_rect(mx, my, log_btn_.x, log_btn_.y,
                                log_btn_.w, log_btn_.h)) {
                        log_panel_visible_ = !log_panel_visible_;
                        log_panel_animating_ = true;
                        log_panel_anim_start_ = std::chrono::steady_clock::now();
                        break;
                    }

                    // Icon button toggle (info panel)
                    if (icon_btn_.w > 0 &&
                        in_rect(mx, my, icon_btn_.x, icon_btn_.y,
                                icon_btn_.w, icon_btn_.h)) {
                        info_panel_visible_ = !info_panel_visible_;
                        info_panel_animating_ = true;
                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                        if (info_panel_visible_ && version_panel_visible_) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                        break;
                    }
                    // Dismiss info panel on outside click
                    if (info_panel_visible_ && info_panel_anim_ >= 1.0f) {
                        info_panel_visible_ = false;
                        info_panel_animating_ = true;
                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                        break;
                    }
                    // Dismiss version panel on outside click
                    if (version_panel_visible_ && version_panel_anim_ >= 1.0f) {
                        if (!in_rect(mx, my, version_panel_rect_.x, version_panel_rect_.y,
                                     version_panel_rect_.w, version_panel_rect_.h)) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
                    }

                    // Footer link clicks (waiting screen only)
                    if (!ever_received_frame_) {
                        bool handled = false;
                        for (auto& hit : footer_hits_) {
                            if (in_rect(mx, my, hit.x, hit.y, hit.w, hit.h)) {
                                if (hit.tooltip == "Version history") {
                                    version_panel_visible_ = !version_panel_visible_;
                                    version_panel_animating_ = true;
                                    version_panel_anim_start_ = std::chrono::steady_clock::now();
                                    version_scroll_offset_ = 0;
                                    if (version_panel_visible_ && info_panel_visible_) {
                                        info_panel_visible_ = false;
                                        info_panel_animating_ = true;
                                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                                    }
                                    handled = true;
                                    break;
                                }
                                if (!hit.url.empty()) {
#ifdef _WIN32
                                    ShellExecuteA(nullptr, "open", hit.url.c_str(),
                                                  nullptr, nullptr, SW_SHOWNORMAL);
#endif
                                    handled = true;
                                    break;
                                }
                            }
                        }
                        if (handled) break;
                    }

                    // Close button
                    if (in_rect(mx, my, close_btn_.x, close_btn_.y,
                                close_btn_.w, close_btn_.h)) {
                        running_.store(false);
                        return;
                    }
                    // Screenshot button
                    if (in_rect(mx, my, screenshot_btn_.x, screenshot_btn_.y,
                                screenshot_btn_.w, screenshot_btn_.h)) {
                        screenshot_requested_ = true;
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }
                    // Folder button
                    if (in_rect(mx, my, folder_btn_.x, folder_btn_.y,
                                folder_btn_.w, folder_btn_.h)) {
                        open_screenshot_folder();
                        break;
                    }
                    // Bezel drag
                    if (phone_frame_enabled_ && frame_dst_w_ > 0) {
                        if (in_rect(mx, my, frame_dst_x_, frame_dst_y_,
                                    frame_dst_w_, frame_dst_h_)) {
                            float sc = (float)frame_dst_w_ / phone_frame_.frame_width();
                            int vx = frame_dst_x_ + (int)(phone_frame_.screen_x() * sc);
                            int vy = frame_dst_y_ + (int)(phone_frame_.screen_y() * sc);
                            int vw = (int)(phone_frame_.screen_width() * sc);
                            int vh = (int)(phone_frame_.screen_height() * sc);
                            if (!in_rect(mx, my, vx, vy, vw, vh))
                                begin_window_drag();
                        }
                    } else {
                        begin_window_drag();
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                if (log_scrollbar_dragging_ && log_sb_track_h_ > 0 && log_sb_max_scroll_ > 0) {
                    int my = event.motion.y;
                    float frac = (float)(my - log_sb_track_y_ - log_sb_thumb_h_ / 2) /
                                 (float)(log_sb_track_h_ - log_sb_thumb_h_);
                    frac = std::max(0.0f, std::min(1.0f, frac));
                    log_scroll_offset_ = (int)(frac * log_sb_max_scroll_);
                } else if (resizing_ && phone_frame_.frame_width() > 0) {
                    int gmx, gmy;
                    SDL_GetGlobalMouseState(&gmx, &gmy);
                    float aspect = (float)phone_frame_.frame_height() /
                                   (float)phone_frame_.frame_width();
                    int new_w = std::max(200, resize_start_w_ + (gmx - resize_start_gx_));
                    SDL_DisplayMode dm;
                    SDL_GetCurrentDisplayMode(0, &dm);
                    new_w = std::min(new_w, (int)(dm.w * 0.95f));
                    int new_h = (int)(new_w * aspect);
                    if (new_h > (int)(dm.h * 0.95f)) {
                        new_h = (int)(dm.h * 0.95f);
                        new_w = (int)(new_h / aspect);
                    }
                    SDL_SetWindowSize(window_, new_w, new_h);
                    window_shape_set_ = false;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    if (resizing_) resizing_ = false;
                    if (log_scrollbar_dragging_) log_scrollbar_dragging_ = false;
                }
                break;

            case SDL_MOUSEWHEEL:
                if (version_panel_visible_) {
                    int scroll_amount = std::max(8, frame_dst_w_ / 30);
                    version_scroll_offset_ -= event.wheel.y * scroll_amount;
                } else if (log_panel_visible_) {
                    int scroll_amount = std::max(12, frame_dst_w_ / 20);
                    log_scroll_offset_ -= event.wheel.y * scroll_amount;
                }
                break;
            }
        }

        // Handle disconnect reset (from TEARDOWN on another thread)
        if (reset_requested_.load()) {
            reset_requested_.store(false);
            if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
            tex_width_ = tex_height_ = 0;
            ever_received_frame_ = false;
            last_frame_data_.clear();
            last_frame_w_ = last_frame_h_ = last_frame_stride_ = 0;
            std::cout << "[Renderer] Reset to waiting screen\n";
        }

        // Periodic status heartbeat (every 30s) so log panel shows activity
        {
            static auto last_heartbeat = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();
            if (elapsed >= 30) {
                last_heartbeat = now;
                if (ever_received_frame_)
                    std::cout << "[Status] Streaming active\n";
                else
                    std::cout << "[Status] Waiting for connection...\n";
            }
        }

        render_frame();

        if (first_render_) {
            SDL_ShowWindow(window_);
            first_render_ = false;
        }

        SDL_Delay(1);
    }
}

void Renderer::render_frame() {
    // 1. Update texture from pending frame
    {
        std::lock_guard lock(frame_mutex_);
        if (has_new_frame_ && pending_frame_.data) {
            if (pending_frame_.width != tex_width_ || pending_frame_.height != tex_height_) {
                if (texture_) SDL_DestroyTexture(texture_);
                texture_ = SDL_CreateTexture(sdl_renderer_,
                    SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                    pending_frame_.width, pending_frame_.height);
                tex_width_ = pending_frame_.width;
                tex_height_ = pending_frame_.height;
                std::cout << "[Renderer] Texture resized: " << tex_width_ << "x" << tex_height_ << "\n";

                window_shape_set_ = false;
                if (phone_frame_enabled_) {
                    phone_frame_.generate(sdl_renderer_, tex_width_, tex_height_);
                    SDL_DisplayMode dm;
                    SDL_GetCurrentDisplayMode(0, &dm);
                    int fw = phone_frame_.frame_width();
                    int fh = phone_frame_.frame_height();
                    float s = std::min(dm.w * 0.9f / fw, dm.h * 0.85f / fh);
                    SDL_SetWindowSize(window_, (int)(fw * s), (int)(fh * s));
                    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                }
            }

            int data_size = pending_frame_.stride * pending_frame_.height;
            if ((int)last_frame_data_.size() != data_size) last_frame_data_.resize(data_size);
            memcpy(last_frame_data_.data(), pending_frame_.data, data_size);
            last_frame_w_ = pending_frame_.width;
            last_frame_h_ = pending_frame_.height;
            last_frame_stride_ = pending_frame_.stride;
            SDL_UpdateTexture(texture_, nullptr, pending_frame_.data, pending_frame_.stride);
            has_new_frame_ = false;
            ever_received_frame_ = true;
        }
    }

    // 2. Compute layout
    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    if (!phone_frame_enabled_ || !phone_frame_.is_generated()) {
        if (!texture_) return;
        // If we still have a phone-shaped window region, clear it so the
        // un-framed view isn't clipped to the old phone outline.
        if (window_shape_set_) {
            frame_dst_w_ = 0;
            update_window_shape();
            window_shape_set_ = false;
        }
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_);
        float sx = (float)win_w / tex_width_, sy = (float)win_h / tex_height_;
        float sc = std::min(sx, sy);
        SDL_Rect dst = {(int)((win_w - tex_width_ * sc) / 2), (int)((win_h - tex_height_ * sc) / 2),
                        (int)(tex_width_ * sc), (int)(tex_height_ * sc)};
        SDL_RenderCopy(sdl_renderer_, texture_, nullptr, &dst);
        close_btn_ = screenshot_btn_ = folder_btn_ = icon_btn_ = {};
        resize_grip_ = {};
        frame_dst_w_ = 0;
        SDL_RenderPresent(sdl_renderer_);
        return;
    }

    // Phone frame mode
    int fw = phone_frame_.frame_width(), fh = phone_frame_.frame_height();

    // Animate log panel
    if (log_panel_animating_) {
        float elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - log_panel_anim_start_).count() / 1000.0f;
        float duration = 300.0f; // ms
        float t = std::min(1.0f, elapsed / duration);
        // Ease out cubic for smooth deceleration
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        log_panel_anim_ = log_panel_visible_ ? eased : (1.0f - eased);
        if (t >= 1.0f) {
            log_panel_animating_ = false;
            if (!log_panel_visible_) log_panel_anim_ = 0.0f;
            else log_panel_anim_ = 1.0f;
        }
    }

    // Phone frame size is always determined by window HEIGHT (height never changes)
    float scale = (float)win_h / fh;
    frame_dst_w_ = (int)(fw * scale);
    frame_dst_h_ = (int)(fh * scale);

    // Log panel width based on frame height
    int log_panel_w = (int)(frame_dst_h_ * 0.55f * log_panel_anim_);

    // Window width = phone frame + log panel (expands rightward, phone stays put).
    // In fullscreen we don't resize the window; instead we centre the phone
    // (and its log drawer) inside the fullscreen rect.
    bool is_fullscreen =
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    int needed_w = frame_dst_w_ + log_panel_w;
    if (!is_fullscreen && win_w != needed_w) {
        SDL_SetWindowSize(window_, needed_w, win_h);
        win_w = needed_w;
        window_shape_set_ = false;
    }

    if (is_fullscreen) {
        // Centre the phone+drawer block horizontally in the fullscreen rect.
        frame_dst_x_ = std::max(0, (win_w - needed_w) / 2);
    } else {
        frame_dst_x_ = 0; // Phone at left edge; log panel expands right
    }
    frame_dst_y_ = (win_h - frame_dst_h_) / 2;

    int svx = frame_dst_x_ + (int)std::round(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)std::round(phone_frame_.screen_y() * scale);
    int svw = (int)std::round(phone_frame_.screen_width() * scale);
    int svh = (int)std::round(phone_frame_.screen_height() * scale);

    if (texture_ && ever_received_frame_) {
        // --- Video mode ---
        SDL_SetRenderDrawColor(sdl_renderer_, 28, 28, 30, 255);
        SDL_RenderClear(sdl_renderer_);
        SDL_Rect vdst = {svx - 1, svy - 1, svw + 2, svh + 2};
        SDL_RenderCopy(sdl_renderer_, texture_, nullptr, &vdst);
    } else {
        // --- Waiting screen ---
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_);

        // Logo (or icon fallback) centered in top third
        if (logo_texture_) {
            // Scale logo to fit nicely: max width 60% of screen, max height 25% of screen
            float logo_max_w = svw * 0.6f;
            float logo_max_h = svh * 0.25f;
            float logo_scale = std::min(logo_max_w / logo_tex_w_, logo_max_h / logo_tex_h_);
            int lw = (int)(logo_tex_w_ * logo_scale);
            int lh = (int)(logo_tex_h_ * logo_scale);
            SDL_Rect lr = {svx + (svw - lw) / 2, svy + svh / 5 - lh / 2, lw, lh};
            SDL_RenderCopy(sdl_renderer_, logo_texture_, nullptr, &lr);
        } else if (icon_texture_) {
            int icon_sz = std::max(32, svw / 4);
            SDL_Rect ir = {svx + (svw - icon_sz) / 2, svy + svh / 4 - icon_sz / 2,
                           icon_sz, icon_sz};
            SDL_RenderCopy(sdl_renderer_, icon_texture_, nullptr, &ir);
        }

        // "Waiting for connection..." text
        if (waiting_tex_) {
            float ts = std::min(1.0f, svw * 0.6f / waiting_tex_w_);
            int tw = (int)(waiting_tex_w_ * ts), th = (int)(waiting_tex_h_ * ts);
            int wait_y = svy + (int)(svh * 0.36f);
            SDL_Rect tr = {svx + (svw - tw) / 2, wait_y, tw, th};
            SDL_RenderCopy(sdl_renderer_, waiting_tex_, nullptr, &tr);
        }

        // Cast instructions below waiting text — icon centered on its own line, text below
        {
            int instr_y = svy + (int)(svh * 0.46f);
            int icon_text_gap = (int)(svh * 0.005f);
            int section_spacing = (int)(svh * 0.04f);
            int icon_disp_sz = std::max(36, svw / 6); // larger centered icon

            if (ios_icon_tex_ && ios_instr_tex_) {
                // Icon centered
                SDL_Rect icr = {svx + (svw - icon_disp_sz) / 2, instr_y, icon_disp_sz, icon_disp_sz};
                SDL_RenderCopy(sdl_renderer_, ios_icon_tex_, nullptr, &icr);
                instr_y += icon_disp_sz + icon_text_gap;
                // Text centered below icon
                float is = std::min(1.0f, svw * 0.85f / (float)ios_instr_w_);
                int iw = (int)(ios_instr_w_ * is), ih = (int)(ios_instr_h_ * is);
                SDL_Rect tr = {svx + (svw - iw) / 2, instr_y, iw, ih};
                SDL_RenderCopy(sdl_renderer_, ios_instr_tex_, nullptr, &tr);
                instr_y += ih + section_spacing;
            }

            if (android_icon_tex_ && android_instr_tex_) {
                // Icon centered
                SDL_Rect icr = {svx + (svw - icon_disp_sz) / 2, instr_y, icon_disp_sz, icon_disp_sz};
                SDL_RenderCopy(sdl_renderer_, android_icon_tex_, nullptr, &icr);
                instr_y += icon_disp_sz + icon_text_gap;
                // Text centered below icon
                float as = std::min(1.0f, svw * 0.85f / (float)android_instr_w_);
                int aw = (int)(android_instr_w_ * as), ah = (int)(android_instr_h_ * as);
                SDL_Rect tr = {svx + (svw - aw) / 2, instr_y, aw, ah};
                SDL_RenderCopy(sdl_renderer_, android_instr_tex_, nullptr, &tr);
            }
        }

        // Footer with interactive segments
        draw_footer(svx, svy, svw, svh);
    }

    // Animate island visibility
    if (island_animating_) {
        float elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - island_anim_start_).count() / 1000.0f;
        float duration = 200.0f; // ms
        float t = std::min(1.0f, elapsed / duration);
        island_anim_ = island_visible_ ? t : (1.0f - t);
        if (t >= 1.0f) island_animating_ = false;
    }

    // Info/Version panels (furthest back — emerge from behind island)
    if (info_panel_visible_ || info_panel_animating_) draw_info_panel();
    if (version_panel_visible_ || version_panel_animating_) draw_version_panel();

    // Island bar (behind frame — slides behind bezel)
    if (island_anim_ > 0.01f) draw_island();

    // Phone frame overlay (on top — covers island and panel edges)
    phone_frame_.render(sdl_renderer_, frame_dst_x_, frame_dst_y_, frame_dst_w_, frame_dst_h_);

    // Log panel (slides out to the left)
    if (log_panel_anim_ > 0.01f) draw_log_panel();

    // Menu star — center top bezel of frame
    // Track which bezel UI element (if any) the cursor is currently over,
    // so we can emit a single delayed tooltip after all dots are drawn.
    std::string bezel_hover_key;
    std::string bezel_hover_text;
    int bezel_hover_ax = 0, bezel_hover_ay = 0;

    // Menu star — center top bezel
    {
        float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_top = (int)(phone_frame_.screen_y() * scale);
        int dot_r = std::max(2, frame_dst_w_ / 200);
        int star_cx = frame_dst_x_ + frame_dst_w_ / 2;
        int star_cy = frame_dst_y_ + bezel_top / 2;
        int hit_sz = std::max(16, dot_r * 6);
        menu_btn_ = {star_cx - hit_sz / 2, star_cy - hit_sz / 2, hit_sz, hit_sz};

        int smx, smy;
        SDL_GetMouseState(&smx, &smy);
        bool star_hover = in_rect(smx, smy, menu_btn_.x, menu_btn_.y, menu_btn_.w, menu_btn_.h);
        uint8_t sa = star_hover ? 240 : 160;
        SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, sa);
        fill_circle(sdl_renderer_, star_cx, star_cy, dot_r);
        if (star_hover) {
            bezel_hover_key = "menu";
            bezel_hover_text = "Menu";
            bezel_hover_ax = star_cx;
            bezel_hover_ay = star_cy + dot_r + 4;
        }
    }

    // Log star — center right bezel
    {
        float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_right = frame_dst_w_ - (int)((phone_frame_.screen_x() + phone_frame_.screen_width()) * scale);
        int dot_r = std::max(2, frame_dst_w_ / 200);
        int star_cx = frame_dst_x_ + frame_dst_w_ - bezel_right / 2;
        int star_cy = frame_dst_y_ + frame_dst_h_ / 2;
        int hit_sz = std::max(16, dot_r * 6);
        log_btn_ = {star_cx - hit_sz / 2, star_cy - hit_sz / 2, hit_sz, hit_sz};

        int smx, smy;
        SDL_GetMouseState(&smx, &smy);
        bool log_hover = in_rect(smx, smy, log_btn_.x, log_btn_.y, log_btn_.w, log_btn_.h);
        uint8_t la = (log_hover || log_panel_visible_) ? 240 : 160;
        SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, la);
        fill_circle(sdl_renderer_, star_cx, star_cy, dot_r);
        if (log_hover) {
            bezel_hover_key = "log";
            bezel_hover_text = "Show log";
            bezel_hover_ax = star_cx - dot_r * 6;
            bezel_hover_ay = star_cy;
        }
    }

    // Source picker — small dots in bottom bezel, one per connected device
    source_btns_.clear();
    if (get_sources_fn_) {
        auto sources = get_sources_fn_();
        if (sources.size() >= 1) {
            float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
            int bezel_bottom = frame_dst_h_ -
                (int)((phone_frame_.screen_y() + phone_frame_.screen_height()) * scale);
            int dot_r = std::max(3, frame_dst_w_ / 140);
            int spacing = dot_r * 5;
            int total_w = spacing * (int)(sources.size() - 1);
            int start_x = frame_dst_x_ + frame_dst_w_ / 2 - total_w / 2;
            int cy = frame_dst_y_ + frame_dst_h_ - bezel_bottom / 2;
            int hit_sz = std::max(20, dot_r * 5);

            int pmx, pmy;
            SDL_GetMouseState(&pmx, &pmy);

            for (size_t i = 0; i < sources.size(); ++i) {
                int cx = start_x + (int)i * spacing;
                BtnRect r{cx - hit_sz / 2, cy - hit_sz / 2, hit_sz, hit_sz};
                bool hover = in_rect(pmx, pmy, r.x, r.y, r.w, r.h);
                if (sources[i].active) {
                    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 255);
                    fill_circle(sdl_renderer_, cx, cy, dot_r);
                } else {
                    uint8_t a = hover ? 230 : (sources[i].streaming ? 180 : 110);
                    SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, a);
                    fill_circle(sdl_renderer_, cx, cy, dot_r - 1);
                }
                source_btns_.emplace_back(sources[i].id, r);
                if (hover) {
                    bezel_hover_key = "src:" + sources[i].id;
                    bezel_hover_text = sources[i].name +
                                       " (left-click: switch, right-click: menu)";
                    bezel_hover_ax = cx;
                    bezel_hover_ay = cy - dot_r - 4;
                }
            }
        }
    }

    // Resize grip — 3 dots along the phone corner arc
    {
        float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
        int cr = std::max(4, (int)(phone_frame_.corner_radius() * scale));
        int dot_r = std::max(2, frame_dst_w_ / 200);
        // Arc center is at bottom-right corner inset by corner radius
        int cx = frame_dst_x_ + frame_dst_w_ - cr;
        int cy = frame_dst_y_ + frame_dst_h_ - cr;
        // Place dots just inside the corner arc (radius slightly less than cr)
        float arc_r = cr - dot_r * 2.5f;

        // Hit-test region covers the corner area
        resize_grip_ = {cx, cy, cr, cr};

        int rmx, rmy;
        SDL_GetMouseState(&rmx, &rmy);
        bool grip_hover = in_rect(rmx, rmy, resize_grip_.x, resize_grip_.y,
                                   resize_grip_.w, resize_grip_.h);
        uint8_t ga = grip_hover ? 230 : 140;
        SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, ga);

        // 3 dots at 15°, 45°, 75° in the bottom-right quadrant
        const float pi = 3.14159265f;
        float angles[] = {15.0f, 45.0f, 75.0f};
        for (float deg : angles) {
            float rad = deg * pi / 180.0f;
            int dx = cx + (int)(arc_r * cosf(rad));
            int dy = cy + (int)(arc_r * sinf(rad));
            fill_circle(sdl_renderer_, dx, dy, dot_r);
        }
        if (grip_hover) {
            bezel_hover_key = "resize";
            bezel_hover_text = "Drag to resize window";
            bezel_hover_ax = cx - cr / 2;
            bezel_hover_ay = cy - cr / 2;
        }
    }

    // Delayed (1 s) tooltip for any bezel button currently hovered.
    // Hidden while a popup menu is open to avoid visual conflict.
    if (!bezel_menu_visible_ && !bezel_hover_key.empty() && tooltip_ready(bezel_hover_key)) {
        draw_bezel_tooltip(bezel_hover_text, bezel_hover_ax, bezel_hover_ay);
    } else if (bezel_hover_key.empty() || bezel_menu_visible_) {
        // Reset hover tracker when the cursor leaves all bezel buttons so
        // re-entering restarts the 1 s delay.
        if (hover_key_.rfind("menu", 0) == 0 ||
            hover_key_.rfind("log", 0) == 0 ||
            hover_key_.rfind("src:", 0) == 0 ||
            hover_key_.rfind("resize", 0) == 0) {
            hover_key_.clear();
        }
    }

    // Generic right-click popup menu for any bezel button.
    if (bezel_menu_visible_) {
        // Build the item list for this target.
        struct Item { std::string action, label; };
        std::vector<Item> items;
        if (bezel_menu_target_ == "menu") {
            items.push_back({"exit", "Exit application"});
        } else if (bezel_menu_target_ == "log") {
            items.push_back({"copy", "Copy log to clipboard"});
        } else if (bezel_menu_target_.rfind("src:", 0) == 0) {
            std::string src_id = bezel_menu_target_.substr(4);
            std::string name = "device";
            if (get_sources_fn_) {
                for (auto& s : get_sources_fn_()) {
                    if (s.id == src_id) { name = s.name; break; }
                }
            }
            items.push_back({"disconnect", "Disconnect " + name});
        }
        if (items.empty()) {
            bezel_menu_visible_ = false;
        } else {
            // Slide-in animation, matching info/version panels.
            const float anim_duration = 200.0f;
            if (bezel_menu_animating_) {
                float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - bezel_menu_anim_start_).count();
                float t = std::min(1.0f, elapsed / anim_duration);
                float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
                bezel_menu_anim_ = eased;
                if (t >= 1.0f) bezel_menu_animating_ = false;
            } else {
                bezel_menu_anim_ = 1.0f;
            }

            // Cap font size in tablet mode so the popup stays the same neat
            // phone-sized format.
            int eq_w = phone_frame_.is_tablet()
                           ? (int)(frame_dst_w_ * 0.62f)
                           : frame_dst_w_;
            int font_h = std::max(12, eq_w / 36);
            int line_gap = std::max(3, font_h / 4);
            int pad = std::max(8, font_h);

            // Render each item to a texture and find the widest.
            struct R { SDL_Texture* tex; int w, h; std::string action; };
            std::vector<R> rendered;
            int max_w = 0, item_h = font_h;
            for (auto& it : items) {
                int tw = 0, th = 0;
                SDL_Texture* tex = make_text_texture(
                    sdl_renderer_, it.label, font_h, 240, 240, 240, &tw, &th);
                rendered.push_back({tex, tw, th, it.action});
                if (tw > max_w) max_w = tw;
                if (th > item_h) item_h = th;
            }
            int row_h = item_h + line_gap * 2;
            int panel_w = max_w + pad * 2;
            int panel_h = pad + (int)rendered.size() * row_h + pad - line_gap;

            // Choose slide direction based on which bezel button was clicked:
            //   menu (top)    → slide down
            //   log  (right)  → slide left
            //   src  (bottom) → slide up
            //   resize/other  → slide up by default
            int dx = 0, dy = 0;          // initial offset before easing
            int panel_x = 0, panel_y = 0; // final position
            int win_w = 0, win_h = 0;
            SDL_GetWindowSize(window_, &win_w, &win_h);

            if (bezel_menu_target_ == "menu") {
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ + 12;
                dy = -(int)(panel_h * 0.3f);
            } else if (bezel_menu_target_ == "log") {
                panel_x = bezel_menu_anchor_x_ - panel_w - 12;
                panel_y = bezel_menu_anchor_y_ - panel_h / 2;
                dx = (int)(panel_w * 0.3f);
            } else {
                // Sources (bottom) and fallback: slide up from the button.
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ - panel_h - 12;
                dy = (int)(panel_h * 0.3f);
            }
            // Clamp inside window.
            if (panel_x + panel_w > win_w - 4) panel_x = win_w - panel_w - 4;
            if (panel_x < 4) panel_x = 4;
            if (panel_y + panel_h > win_h - 4) panel_y = win_h - panel_h - 4;
            if (panel_y < 4) panel_y = 4;

            // Apply slide-in offset (decays with anim progress).
            float inv = 1.0f - bezel_menu_anim_;
            int draw_x = panel_x + (int)(dx * inv);
            int draw_y = panel_y + (int)(dy * inv);
            uint8_t alpha = (uint8_t)(240 * bezel_menu_anim_);
            uint8_t text_alpha = (uint8_t)(255 * bezel_menu_anim_);

            int mmx, mmy;
            SDL_GetMouseState(&mmx, &mmy);

            // Rounded-rect background, same recipe as draw_info_panel.
            int pr = std::max(6, pad / 2);
            SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
            SDL_Rect body{draw_x + pr, draw_y, panel_w - pr * 2, panel_h};
            SDL_RenderFillRect(sdl_renderer_, &body);
            SDL_Rect ls{draw_x, draw_y + pr, pr, panel_h - pr * 2};
            SDL_RenderFillRect(sdl_renderer_, &ls);
            SDL_Rect rs{draw_x + panel_w - pr, draw_y + pr, pr, panel_h - pr * 2};
            SDL_RenderFillRect(sdl_renderer_, &rs);
            fill_circle(sdl_renderer_, draw_x + pr, draw_y + pr, pr);
            fill_circle(sdl_renderer_, draw_x + panel_w - pr, draw_y + pr, pr);
            fill_circle(sdl_renderer_, draw_x + pr, draw_y + panel_h - pr, pr);
            fill_circle(sdl_renderer_, draw_x + panel_w - pr, draw_y + panel_h - pr, pr);

            bezel_menu_items_.clear();
            int row_y = draw_y + pad - line_gap;
            for (size_t i = 0; i < rendered.size(); ++i) {
                SDL_Rect row{draw_x + pr, row_y, panel_w - pr * 2, row_h};
                bool hov = in_rect(mmx, mmy, row.x, row.y, row.w, row.h);
                if (hov && bezel_menu_anim_ >= 0.5f) {
                    SDL_SetRenderDrawColor(sdl_renderer_, 70, 70, 80, alpha);
                    SDL_RenderFillRect(sdl_renderer_, &row);
                }
                if (rendered[i].tex) {
                    int dw = rendered[i].w;
                    int dh = rendered[i].h;
                    SDL_Rect td{draw_x + (panel_w - dw) / 2,
                                row.y + (row_h - dh) / 2, dw, dh};
                    SDL_SetTextureAlphaMod(rendered[i].tex, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, rendered[i].tex, nullptr, &td);
                    SDL_DestroyTexture(rendered[i].tex);
                }
                // Hit-rect uses the FINAL panel position so clicks line up
                // even mid-animation.
                bezel_menu_items_.emplace_back(
                    rendered[i].action,
                    BtnRect{panel_x + pr, panel_y + (pad - line_gap) + (int)i * row_h,
                            panel_w - pr * 2, row_h});
                row_y += row_h;
            }
        }
    }

    // Window shape
    if (!window_shape_set_ || log_panel_animating_) {
        update_window_shape();
        window_shape_set_ = true;
    }

    // PIN overlay (drawn last, on top of everything)
    draw_pin_overlay();

    SDL_RenderPresent(sdl_renderer_);

    if (screenshot_requested_) {
        screenshot_requested_ = false;
        take_screenshot();
    }
}

bool Renderer::tooltip_ready(const std::string& key) {
    if (key.empty()) {
        hover_key_.clear();
        return false;
    }
    if (hover_key_ != key) {
        hover_key_ = key;
        hover_start_ = std::chrono::steady_clock::now();
        return false;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hover_start_).count();
    return elapsed >= 1000;
}

void Renderer::draw_bezel_tooltip(const std::string& text, int anchor_x, int anchor_y) {
    if (text.empty()) return;

    // Cap font size in tablet mode so tooltip stays the same neat size.
    int eq_w = phone_frame_.is_tablet()
                   ? (int)(frame_dst_w_ * 0.62f)
                   : frame_dst_w_;
    int target_h = std::max(12, eq_w / 38);

#ifdef _WIN32
    // Render at the actual target font height for crisp text (no scaling).
    if (text != bezel_tip_str_ || target_h != bezel_tip_font_h_) {
        if (bezel_tip_tex_) { SDL_DestroyTexture(bezel_tip_tex_); bezel_tip_tex_ = nullptr; }
        bezel_tip_tex_ = make_text_texture(sdl_renderer_, text, target_h,
                                           255, 255, 255, &bezel_tip_w_, &bezel_tip_h_);
        bezel_tip_str_ = text;
        bezel_tip_font_h_ = target_h;
    }
#endif
    if (!bezel_tip_tex_) return;

    int disp_w = bezel_tip_w_;
    int disp_h = bezel_tip_h_;
    int tp = std::max(3, disp_h / 3);
    int tw = disp_w + tp * 2;
    int th = disp_h + tp * 2;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    int tx = anchor_x - tw / 2;
    int ty = anchor_y - th - 6;
    if (ty < 4) ty = anchor_y + 12;
    if (tx + tw > win_w - 4) tx = win_w - tw - 4;
    if (tx < 4) tx = 4;

    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 32, 230);
    SDL_Rect bg{tx, ty, tw, th};
    SDL_RenderFillRect(sdl_renderer_, &bg);
    SDL_SetRenderDrawColor(sdl_renderer_, 100, 100, 100, 220);
    SDL_RenderDrawRect(sdl_renderer_, &bg);
    SDL_Rect td{tx + tp, ty + tp, disp_w, disp_h};
    SDL_RenderCopy(sdl_renderer_, bezel_tip_tex_, nullptr, &td);
}

void Renderer::draw_island() {
    if (frame_dst_w_ == 0) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    // For tablet aspect ratios the screen is much wider relative to its
    // height than a phone. Scaling the island off `svw` would make it
    // huge on iPad. Instead, derive button size from the screen height
    // (a phone-equivalent width) so the menu stays the same neat size
    // regardless of source device.
    int phone_eq_w = (std::min)(svw, svh / 2);
    int btn_sz = std::max(20, phone_eq_w / 14);
    int pad = btn_sz / 3;
    int row_h = btn_sz + pad * 2;

    // Apply slide animation offset
    int slide_offset = -(int)((1.0f - island_anim_) * (row_h + pad * 2));
    svy += slide_offset;

    // Toast area
    bool show_toast = false;
    int toast_h = 0;
    if (toast_active_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - toast_start_).count();
        if (elapsed < 1500) {
            show_toast = true;
            toast_h = std::max(14, btn_sz / 2) + pad;
        } else {
            toast_active_ = false;
        }
    }

    int island_w = std::max(160, (int)(phone_eq_w * 0.80f));
    int island_h = row_h + (show_toast ? toast_h : 0);
    int island_x = svx + (svw - island_w) / 2;
    int island_y = svy + pad;

    // Pill background
    int pill_r = row_h / 2;
    SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 22, 200);
    SDL_Rect body = {island_x + pill_r, island_y, island_w - pill_r * 2, island_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    fill_circle(sdl_renderer_, island_x + pill_r, island_y + pill_r, pill_r);
    fill_circle(sdl_renderer_, island_x + island_w - pill_r, island_y + pill_r, pill_r);
    if (island_h > row_h) {
        SDL_Rect ext = {island_x, island_y + pill_r, island_w, island_h - pill_r};
        SDL_RenderFillRect(sdl_renderer_, &ext);
        int br = std::min(pill_r, (island_h - row_h + pill_r));
        fill_circle(sdl_renderer_, island_x + br, island_y + island_h - br, br);
        fill_circle(sdl_renderer_, island_x + island_w - br, island_y + island_h - br, br);
        SDL_Rect bb = {island_x + br, island_y + island_h - br, island_w - br * 2, br};
        SDL_RenderFillRect(sdl_renderer_, &bb);
    }

    int mx, my;
    SDL_GetMouseState(&mx, &my);

    // Icon (left side)
    int icon_sz = btn_sz - 4;
    int icon_x = island_x + pad + 2;
    int icon_y = island_y + (row_h - icon_sz) / 2;
    if (icon_texture_) {
        SDL_Rect ir = {icon_x, icon_y, icon_sz, icon_sz};
        SDL_RenderCopy(sdl_renderer_, icon_texture_, nullptr, &ir);
    }
    icon_btn_ = {icon_x, icon_y, icon_sz, icon_sz};
    bool icon_hover = in_rect(mx, my, icon_x, icon_y, icon_sz, icon_sz);

    // Buttons (right-aligned): screenshot, folder, close
    int bx = island_x + island_w - pad - btn_sz;
    int by = island_y + (row_h - btn_sz) / 2;
    int gap = pad / 2 + 2;

    auto now = std::chrono::steady_clock::now();
    bool flashing = btn_flash_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - btn_flash_start_).count() < 200;
    if (btn_flash_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - btn_flash_start_).count() >= 200)
        btn_flash_ = false;

    // Close button (rightmost)
    close_btn_ = {bx, by, btn_sz, btn_sz};
    bool close_hover = in_rect(mx, my, bx, by, btn_sz, btn_sz);
    SDL_SetRenderDrawColor(sdl_renderer_, close_hover ? 80 : 50, close_hover ? 30 : 30, close_hover ? 30 : 30, 180);
    fill_circle(sdl_renderer_, bx + btn_sz / 2, by + btn_sz / 2, btn_sz / 2);
    int xr = btn_sz / 5;
    int xcx = bx + btn_sz / 2, xcy = by + btn_sz / 2;
    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, close_hover ? 255 : 200);
    for (int t = -1; t <= 1; t++) {
        SDL_RenderDrawLine(sdl_renderer_, xcx - xr, xcy - xr + t, xcx + xr, xcy + xr + t);
        SDL_RenderDrawLine(sdl_renderer_, xcx - xr, xcy + xr + t, xcx + xr, xcy - xr + t);
    }

    // Folder button
    bx -= btn_sz + gap;
    folder_btn_ = {bx, by, btn_sz, btn_sz};
    bool folder_hover = in_rect(mx, my, bx, by, btn_sz, btn_sz);
    SDL_SetRenderDrawColor(sdl_renderer_, folder_hover ? 70 : 50, folder_hover ? 70 : 50, folder_hover ? 75 : 55, 180);
    fill_circle(sdl_renderer_, bx + btn_sz / 2, by + btn_sz / 2, btn_sz / 2);
    int fr = btn_sz / 4;
    int fcx = bx + btn_sz / 2, fcy = by + btn_sz / 2;
    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, folder_hover ? 255 : 200);
    SDL_Rect folder_body = {fcx - fr, fcy - fr + 2, fr * 2, (int)(fr * 1.5f)};
    SDL_RenderDrawRect(sdl_renderer_, &folder_body);
    SDL_Rect folder_tab = {fcx - fr, fcy - fr, fr, 2};
    SDL_RenderFillRect(sdl_renderer_, &folder_tab);

    // Screenshot button
    bx -= btn_sz + gap;
    screenshot_btn_ = {bx, by, btn_sz, btn_sz};
    bool ss_hover = in_rect(mx, my, bx, by, btn_sz, btn_sz);
    uint8_t ss_bg = flashing ? 200 : (ss_hover ? 70 : 50);
    SDL_SetRenderDrawColor(sdl_renderer_, ss_bg, ss_bg, ss_bg + 5, 180);
    fill_circle(sdl_renderer_, bx + btn_sz / 2, by + btn_sz / 2, btn_sz / 2);
    int cr = btn_sz / 4;
    int ccx = bx + btn_sz / 2, ccy = by + btn_sz / 2;
    uint8_t ca = flashing ? 255 : (ss_hover ? 255 : 200);
    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, ca);
    for (int dy = -cr; dy <= cr; dy++) {
        for (int dx = -cr; dx <= cr; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= cr * cr && d2 >= (cr - 1) * (cr - 1))
                SDL_RenderDrawPoint(sdl_renderer_, ccx + dx, ccy + dy);
        }
    }
    int dot = std::max(1, cr / 3);
    fill_circle(sdl_renderer_, ccx, ccy, dot);

    // Hover detection (0=close, 1=screenshot, 2=folder, 3=icon)
    int new_hover = -1;
    if (close_hover) new_hover = 0;
    else if (ss_hover) new_hover = 1;
    else if (folder_hover) new_hover = 2;
    else if (icon_hover) new_hover = 3;

    // Update tooltip texture on hover change
    if (new_hover != last_tooltip_btn_) {
        last_tooltip_btn_ = new_hover;
        if (tooltip_tex_) { SDL_DestroyTexture(tooltip_tex_); tooltip_tex_ = nullptr; }
#ifdef _WIN32
        const char* tip = nullptr;
        if (new_hover == 0) tip = "Close (Esc)";
        else if (new_hover == 1) tip = "Screenshot (Ctrl+S)";
        else if (new_hover == 2) tip = "Open Screenshots Folder";
        else if (new_hover == 3) tip = "About 1PhoneMirror";
        if (tip) {
            tooltip_tex_ = make_text_texture(sdl_renderer_, tip, 48,
                                              255, 255, 255, &tooltip_tex_w_, &tooltip_tex_h_);
        }
#endif
    }

    // Draw tooltip pill
    if (tooltip_tex_ && new_hover >= 0 && tooltip_ready("island:" + std::to_string(new_hover))) {
        // High-res 48px render scaled to ~btn_sz*0.6 visual height
        float tip_scale = std::max(0.22f, (float)btn_sz * 0.6f / 48.0f);
        int disp_tw = (int)(tooltip_tex_w_ * tip_scale);
        int disp_th = (int)(tooltip_tex_h_ * tip_scale);
        int tp = std::max(3, disp_th / 3);
        int tw = disp_tw + tp * 2;
        int th = disp_th + tp * 2;
        int tx = island_x + (island_w - tw) / 2;
        int ty = island_y + island_h + 4;
        // Rounded pill background
        int pr = th / 2;
        SDL_SetRenderDrawColor(sdl_renderer_, 40, 40, 44, 235);
        SDL_Rect bg_body = {tx + pr, ty, tw - pr * 2, th};
        SDL_RenderFillRect(sdl_renderer_, &bg_body);
        fill_circle(sdl_renderer_, tx + pr, ty + pr, pr);
        fill_circle(sdl_renderer_, tx + tw - pr, ty + pr, pr);
        SDL_Rect tdst = {tx + tp, ty + tp, disp_tw, disp_th};
        SDL_RenderCopy(sdl_renderer_, tooltip_tex_, nullptr, &tdst);
    }

    // Toast text (in island, below button row)
    if (show_toast) {
#ifdef _WIN32
        if (toast_tex_str_ != toast_text_) {
            if (toast_tex_) { SDL_DestroyTexture(toast_tex_); toast_tex_ = nullptr; }
            int fh_toast = std::max(28, btn_sz * 2);
            toast_tex_ = make_text_texture(sdl_renderer_, toast_text_, fh_toast,
                                            220, 220, 220, &toast_tex_w_, &toast_tex_h_);
            toast_tex_str_ = toast_text_;
        }
#endif
        if (toast_tex_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - toast_start_).count();
            uint8_t alpha = 255;
            if (elapsed < 150) alpha = (uint8_t)(elapsed * 255 / 150);
            else if (elapsed > 1200) alpha = (uint8_t)((1500 - elapsed) * 255 / 300);
            SDL_SetTextureAlphaMod(toast_tex_, alpha);
            float t_scale = (float)std::max(11, btn_sz / 2) / std::max(28, btn_sz * 2);
            int disp_tw = (int)(toast_tex_w_ * t_scale);
            int disp_th = (int)(toast_tex_h_ * t_scale);
            int ttx = island_x + (island_w - disp_tw) / 2;
            int tty = island_y + row_h + (toast_h - disp_th) / 2;
            SDL_Rect tdst = {ttx, tty, disp_tw, disp_th};
            SDL_RenderCopy(sdl_renderer_, toast_tex_, nullptr, &tdst);
            SDL_SetTextureAlphaMod(toast_tex_, 255);
        }
    }

    hover_btn_ = new_hover;
}

void Renderer::draw_footer(int svx, int svy, int svw, int svh) {
    if (footer_line1_.empty()) return;

    footer_hits_.clear();

    // Scale factor: textures rendered at 40px GDI font for crispness,
    // scale down to desired display size relative to screen width.
    // Target: ~13px equivalent at svw=300.
    float sf = std::max(0.3f, svw / 300.0f) * (13.0f / 40.0f);
    sf = std::min(sf, 0.6f);

    auto line_w = [&](const std::vector<FooterSeg>& line) {
        int total = 0;
        for (auto& s : line) total += (int)(s.w * sf);
        return total;
    };

    int total_w1 = line_w(footer_line1_);
    int total_w2 = line_w(footer_line2_);
    int line_h = footer_line1_.empty() ? 0 : (int)(footer_line1_[0].h * sf);
    int gap = std::max(2, line_h / 4);

    // Position near bottom of screen area
    int footer_y = svy + svh - line_h * 2 - gap - std::max(6, svh / 25);

    // Line 1
    int x1 = svx + (svw - total_w1) / 2;
    for (auto& s : footer_line1_) {
        int sw = (int)(s.w * sf), sh = (int)(s.h * sf);
        SDL_Rect dst = {x1, footer_y, sw, sh};
        if (s.tex) SDL_RenderCopy(sdl_renderer_, s.tex, nullptr, &dst);
        if (!s.url.empty() || !s.tooltip.empty())
            footer_hits_.push_back({x1, footer_y, sw, sh, s.url, s.tooltip});
        x1 += sw;
    }

    // Line 2
    int y2 = footer_y + line_h + gap;
    int x2 = svx + (svw - total_w2) / 2;
    for (auto& s : footer_line2_) {
        int sw = (int)(s.w * sf), sh = (int)(s.h * sf);
        SDL_Rect dst = {x2, y2, sw, sh};
        if (s.tex) SDL_RenderCopy(sdl_renderer_, s.tex, nullptr, &dst);
        if (!s.url.empty() || !s.tooltip.empty())
            footer_hits_.push_back({x2, y2, sw, sh, s.url, s.tooltip});
        x2 += sw;
    }

    // Hover tooltip for footer segments
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    std::string hovered;
    int hx = 0, hy = 0, hw = 0;
    for (auto& h : footer_hits_) {
        if (!h.tooltip.empty() && in_rect(mx, my, h.x, h.y, h.w, h.h)) {
            hovered = h.tooltip;
            hx = h.x; hy = h.y; hw = h.w;
            break;
        }
    }

    if (!hovered.empty() && tooltip_ready("footer:" + hovered)) {
        if (footer_tooltip_str_ != hovered) {
            if (footer_tooltip_tex_) { SDL_DestroyTexture(footer_tooltip_tex_); footer_tooltip_tex_ = nullptr; }
#ifdef _WIN32
            footer_tooltip_tex_ = make_text_texture(sdl_renderer_, hovered, 32,
                                                     255, 255, 255, &footer_tooltip_w_, &footer_tooltip_h_);
#endif
            footer_tooltip_str_ = hovered;
        }
        if (footer_tooltip_tex_) {
            // Display at same visual size as footer text
            float ft_scale = sf * (40.0f / 32.0f);
            int disp_w = (int)(footer_tooltip_w_ * ft_scale);
            int disp_h = (int)(footer_tooltip_h_ * ft_scale);
            int tp = std::max(3, disp_h / 3);
            int tw = disp_w + tp * 2;
            int th = disp_h + tp * 2;
            int tx = hx + hw / 2 - tw / 2;
            int ty = hy - th - 4;
            // Clamp to screen area
            tx = std::max(svx, std::min(tx, svx + svw - tw));
            // Rounded pill background
            int pr = th / 2;
            SDL_SetRenderDrawColor(sdl_renderer_, 40, 40, 44, 235);
            SDL_Rect bg_body = {tx + pr, ty, tw - pr * 2, th};
            SDL_RenderFillRect(sdl_renderer_, &bg_body);
            fill_circle(sdl_renderer_, tx + pr, ty + pr, pr);
            fill_circle(sdl_renderer_, tx + tw - pr, ty + pr, pr);
            SDL_Rect tdst = {tx + tp, ty + tp, disp_w, disp_h};
            SDL_RenderCopy(sdl_renderer_, footer_tooltip_tex_, nullptr, &tdst);
        }
    } else if (!footer_tooltip_str_.empty()) {
        footer_tooltip_str_.clear();
    }
}

void Renderer::draw_info_panel() {
    if (frame_dst_w_ == 0 || info_lines_.empty()) return;

    // Update animation
    const float anim_duration = 200.0f; // ms
    if (info_panel_animating_) {
        float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - info_panel_anim_start_).count();
        float t = std::min(1.0f, elapsed / anim_duration);
        // Ease out cubic
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        if (info_panel_visible_)
            info_panel_anim_ = eased;
        else
            info_panel_anim_ = 1.0f - eased;
        if (t >= 1.0f) info_panel_animating_ = false;
    } else {
        info_panel_anim_ = info_panel_visible_ ? 1.0f : 0.0f;
    }

    if (info_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);

    int pad = std::max(8, svw / 20);
    int line_gap = std::max(3, pad / 4);
    int spacer_h = std::max(4, pad / 2);

    // Scale text to fit panel
    float max_text_w = svw * 0.7f;
    float text_scale = 0.5f;
    for (auto& line : info_lines_) {
        if (line.tex && line.w * text_scale > max_text_w)
            text_scale = max_text_w / line.w;
    }

    // Compute total height
    int total_h = pad;
    for (auto& line : info_lines_) {
        if (line.tex)
            total_h += (int)(line.h * text_scale) + line_gap;
        else
            total_h += spacer_h;
    }
    total_h += pad - line_gap;

    int panel_w = (int)(svw * 0.80f);
    int panel_x = svx + (svw - panel_w) / 2;

    // Position below island bar
    int btn_sz = std::max(20, svw / 14);
    int ipad = btn_sz / 3;
    int island_bottom = svy + ipad + btn_sz + ipad * 2;
    int panel_y_target = island_bottom + pad / 2;

    // Animate: slide down from island_bottom, with fade
    int slide_offset = (int)((1.0f - info_panel_anim_) * total_h * 0.3f);
    int panel_y = panel_y_target - slide_offset;
    uint8_t alpha = (uint8_t)(240 * info_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * info_panel_anim_);

    info_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    // Rounded rect background
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr * 2, total_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, total_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, total_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + total_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + total_h - pr, pr);

    // Draw text lines centered
    int cy = panel_y + pad;
    for (auto& line : info_lines_) {
        if (line.tex) {
            int dw = (int)(line.w * text_scale);
            int dh = (int)(line.h * text_scale);
            int lx = panel_x + (panel_w - dw) / 2;
            SDL_Rect dst = {lx, cy, dw, dh};
            SDL_SetTextureAlphaMod(line.tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, line.tex, nullptr, &dst);
            cy += dh + line_gap;
        } else {
            cy += spacer_h;
        }
    }
}

void Renderer::draw_version_panel() {
    if (frame_dst_w_ == 0 || version_lines_.empty()) return;

    // Update animation
    const float anim_duration = 200.0f; // ms
    if (version_panel_animating_) {
        float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - version_panel_anim_start_).count();
        float t = std::min(1.0f, elapsed / anim_duration);
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        if (version_panel_visible_)
            version_panel_anim_ = eased;
        else
            version_panel_anim_ = 1.0f - eased;
        if (t >= 1.0f) version_panel_animating_ = false;
    } else {
        version_panel_anim_ = version_panel_visible_ ? 1.0f : 0.0f;
    }

    if (version_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    int pad = std::max(8, svw / 20);
    int line_gap = std::max(3, pad / 4);
    int spacer_h = std::max(4, pad / 2);

    // Scale text to fit panel
    float max_text_w = svw * 0.7f;
    float text_scale = 0.5f;
    for (auto& line : version_lines_) {
        if (line.tex && line.w * text_scale > max_text_w)
            text_scale = max_text_w / line.w;
    }

    // Compute total content height
    int content_h = pad;
    for (auto& line : version_lines_) {
        if (line.tex)
            content_h += (int)(line.h * text_scale) + line_gap;
        else
            content_h += spacer_h;
    }
    content_h += pad - line_gap;

    int panel_w = (int)(svw * 0.85f);
    int panel_x = svx + (svw - panel_w) / 2;

    // Position below island bar
    int btn_sz = std::max(20, svw / 14);
    int ipad = btn_sz / 3;
    int island_bottom = svy + ipad + btn_sz + ipad * 2;
    int panel_y_target = island_bottom + pad / 2;

    // Animate: slide down from island, with fade
    int slide_offset = (int)((1.0f - version_panel_anim_) * content_h * 0.3f);
    int panel_y = panel_y_target - slide_offset;
    uint8_t alpha = (uint8_t)(240 * version_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * version_panel_anim_);

    // Limit panel height to screen area
    int max_panel_h = svy + svh - panel_y - pad * 2;
    int panel_h = std::min(content_h, max_panel_h);
    bool needs_scroll = content_h > panel_h;

    // Clamp scroll offset
    if (needs_scroll) {
        int max_scroll = content_h - panel_h;
        if (version_scroll_offset_ < 0) version_scroll_offset_ = 0;
        if (version_scroll_offset_ > max_scroll) version_scroll_offset_ = max_scroll;
    } else {
        version_scroll_offset_ = 0;
    }

    version_panel_rect_ = {panel_x, panel_y, panel_w, panel_h};

    // Rounded rect background
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr * 2, panel_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, panel_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, panel_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + panel_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + panel_h - pr, pr);

    // Set clip rect for scrolling content
    SDL_Rect clip = {panel_x, panel_y, panel_w, panel_h};
    SDL_RenderSetClipRect(sdl_renderer_, &clip);

    // Draw text lines centered, offset by scroll
    int cy = panel_y + pad - version_scroll_offset_;
    for (auto& line : version_lines_) {
        if (line.tex) {
            int dw = (int)(line.w * text_scale);
            int dh = (int)(line.h * text_scale);
            int lx = panel_x + (panel_w - dw) / 2;
            SDL_Rect dst = {lx, cy, dw, dh};
            SDL_SetTextureAlphaMod(line.tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, line.tex, nullptr, &dst);
            cy += dh + line_gap;
        } else {
            cy += spacer_h;
        }
    }

    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // Draw scrollbar if needed
    if (needs_scroll) {
        int sb_w = std::max(3, pad / 4);
        int sb_x = panel_x + panel_w - sb_w - pr;
        int track_y = panel_y + pr;
        int track_h = panel_h - pr * 2;
        float visible_frac = (float)panel_h / content_h;
        int thumb_h = std::max(pr * 2, (int)(track_h * visible_frac));
        int max_scroll = content_h - panel_h;
        float scroll_frac = (max_scroll > 0) ? (float)version_scroll_offset_ / max_scroll : 0.0f;
        int thumb_y = track_y + (int)((track_h - thumb_h) * scroll_frac);

        SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, (uint8_t)(180 * version_panel_anim_));
        SDL_Rect thumb = {sb_x, thumb_y, sb_w, thumb_h};
        SDL_RenderFillRect(sdl_renderer_, &thumb);
    }
}

void Renderer::draw_log_panel() {
#ifdef _WIN32
    if (frame_dst_w_ == 0 || log_panel_anim_ < 0.01f) return;

    int lp_w = (int)(frame_dst_h_ * 0.55f * log_panel_anim_);
    int lp_margin = std::max(4, frame_dst_h_ / 40);
    int drawer_inset = frame_dst_h_ / 16; // slightly shorter than phone
    int panel_x = frame_dst_x_ + frame_dst_w_; // flush against phone frame
    int panel_y = frame_dst_y_ + drawer_inset;
    int panel_w = lp_w - lp_margin;
    int panel_h = frame_dst_h_ - drawer_inset * 2;

    if (panel_w < 40) return;

    // Background — flat left edge (flush with phone), rounded right edge (drawer)
    int pr = std::max(8, panel_w / 30);
    SDL_SetRenderDrawColor(sdl_renderer_, 22, 22, 26, 240);
    // Main body
    SDL_Rect body = {panel_x, panel_y, panel_w - pr, panel_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    // Right strip
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, panel_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    // Right rounded corners only
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + panel_h - pr, pr);

    // Get log lines
    auto lines = openmirror::LogBuffer::instance().get_lines();
    if (lines.empty()) return;

    // Render text at a small font size
    int font_sz = std::max(9, panel_h / 85);
    int line_h = font_sz + 2;
    int pad = std::max(6, pr);
    int content_h = (int)lines.size() * line_h;
    int visible_h = panel_h - pad * 2;

    // Clamp scroll
    int max_scroll = std::max(0, content_h - visible_h);
    if (log_scroll_offset_ < 0) log_scroll_offset_ = 0;
    if (log_scroll_offset_ > max_scroll) log_scroll_offset_ = max_scroll;

    // Auto-scroll to bottom when new lines arrive — but not while the user
    // is actively dragging the scrollbar thumb.
    uint64_t cur_ver = openmirror::LogBuffer::instance().version();
    if (cur_ver != log_last_version_) {
        log_last_version_ = cur_ver;
        if (!log_scrollbar_dragging_) {
            log_scroll_offset_ = max_scroll;
        }
    }

    // Clip rect
    SDL_Rect clip = {panel_x + pad, panel_y + pad, panel_w - pad * 2, visible_h};
    SDL_RenderSetClipRect(sdl_renderer_, &clip);

    // Only render visible lines for performance
    int first_visible = log_scroll_offset_ / line_h;
    int last_visible = (log_scroll_offset_ + visible_h) / line_h + 1;
    first_visible = std::max(0, first_visible);
    last_visible = std::min((int)lines.size(), last_visible);

    int text_x = panel_x + pad;
    int max_text_w = panel_w - pad * 2;

    for (int i = first_visible; i < last_visible; i++) {
        int y = panel_y + pad + i * line_h - log_scroll_offset_;

        // Truncate long lines for display
        std::string display_line = lines[i];
        if ((int)display_line.size() > max_text_w / (font_sz / 3))
            display_line = display_line.substr(0, max_text_w / (font_sz / 3));

        // Color based on content
        uint8_t r = 160, g = 160, b = 160;
        if (display_line.find("[Cast]") != std::string::npos) { r = 120; g = 200; b = 120; }
        else if (display_line.find("[AirPlay]") != std::string::npos) { r = 120; g = 160; b = 255; }
        else if (display_line.find("[Renderer]") != std::string::npos) { r = 200; g = 180; b = 120; }
        else if (display_line.find("Warning") != std::string::npos ||
                 display_line.find("Failed") != std::string::npos ||
                 display_line.find("Error") != std::string::npos) { r = 255; g = 120; b = 120; }

        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, display_line, font_sz, r, g, b, &tw, &th);
        if (tex) {
            SDL_Rect dst = {text_x, y, tw, th};
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
        }
    }

    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // Scrollbar
    if (content_h > visible_h) {
        int sb_w = std::max(3, pad / 3);
        int sb_x = panel_x + panel_w - pad;
        int track_y = panel_y + pad;
        int track_h = visible_h;
        float visible_frac = (float)visible_h / content_h;
        int thumb_h = std::max(pr * 2, (int)(track_h * visible_frac));
        float scroll_frac = (max_scroll > 0) ? (float)log_scroll_offset_ / max_scroll : 0.0f;
        int thumb_y = track_y + (int)((track_h - thumb_h) * scroll_frac);

        // Store geometry for drag handling
        log_sb_track_y_ = track_y;
        log_sb_track_h_ = track_h;
        log_sb_thumb_h_ = thumb_h;
        log_sb_max_scroll_ = max_scroll;

        uint8_t sb_alpha = log_scrollbar_dragging_ ? 220 : 150;
        SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, sb_alpha);
        SDL_Rect thumb_rect = {sb_x, thumb_y, sb_w, thumb_h};
        SDL_RenderFillRect(sdl_renderer_, &thumb_rect);
    } else {
        log_sb_track_h_ = 0;
        log_sb_max_scroll_ = 0;
    }
#endif
}

void Renderer::take_screenshot() {
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        std::cout << "[Screenshot] No frame data available\n";
        return;
    }

    std::filesystem::create_directories(screenshot_dir_);

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local_tm);
    std::string filename = screenshot_dir_ + "/screenshot_" + timestamp + ".png";

    bool saved = false;
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        int out_w, out_h;
        uint8_t* composite = phone_frame_.composite_screenshot(
            sdl_renderer_,
            last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
            &out_w, &out_h);
        if (composite) {
            if (stbi_write_png(filename.c_str(), out_w, out_h, 4, composite, out_w * 4)) {
                saved = true;
                copy_to_clipboard(composite, out_w, out_h);
                std::cout << "[Screenshot] Saved: " << filename << " (" << out_w << "x" << out_h << ")\n";
            }
            delete[] composite;
        }
    } else {
        if (stbi_write_png(filename.c_str(), last_frame_w_, last_frame_h_, 4,
                           last_frame_data_.data(), last_frame_stride_)) {
            saved = true;
            copy_to_clipboard(last_frame_data_.data(), last_frame_w_, last_frame_h_);
            std::cout << "[Screenshot] Saved: " << filename << "\n";
        }
    }

    if (saved) {
        toast_text_ = "Saved to Pictures & copied to clipboard";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
    }
}

void Renderer::open_screenshot_folder() {
#ifdef _WIN32
    std::filesystem::create_directories(screenshot_dir_);
    ShellExecuteA(nullptr, "explore", screenshot_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void Renderer::copy_to_clipboard(const uint8_t* rgba, int w, int h) {
#ifdef _WIN32
    std::vector<uint8_t> png_data;
    stbi_write_png_to_func(png_write_vec, &png_data, w, h, 4, rgba, w * 4);

    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();

    UINT png_fmt = RegisterClipboardFormatA("PNG");
    if (png_fmt && !png_data.empty()) {
        HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, png_data.size());
        if (hPng) {
            void* p = GlobalLock(hPng);
            memcpy(p, png_data.data(), png_data.size());
            GlobalUnlock(hPng);
            SetClipboardData(png_fmt, hPng);
        }
    }

    size_t hdr_size = sizeof(BITMAPV5HEADER);
    size_t pixel_size = (size_t)w * h * 4;
    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, hdr_size + pixel_size);
    if (hDib) {
        void* pDib = GlobalLock(hDib);
        auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(pDib);
        memset(hdr, 0, hdr_size);
        hdr->bV5Size = (DWORD)hdr_size;
        hdr->bV5Width = w;
        hdr->bV5Height = -h;
        hdr->bV5Planes = 1;
        hdr->bV5BitCount = 32;
        hdr->bV5Compression = BI_BITFIELDS;
        hdr->bV5SizeImage = (DWORD)pixel_size;
        hdr->bV5RedMask = 0x00FF0000;
        hdr->bV5GreenMask = 0x0000FF00;
        hdr->bV5BlueMask = 0x000000FF;
        hdr->bV5AlphaMask = 0xFF000000;
        hdr->bV5CSType = LCS_sRGB;
        hdr->bV5Intent = LCS_GM_IMAGES;

        auto* dst = reinterpret_cast<uint8_t*>(pDib) + hdr_size;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t a = rgba[i * 4 + 3];
            dst[i * 4 + 0] = (uint8_t)(rgba[i * 4 + 2] * a / 255);
            dst[i * 4 + 1] = (uint8_t)(rgba[i * 4 + 1] * a / 255);
            dst[i * 4 + 2] = (uint8_t)(rgba[i * 4 + 0] * a / 255);
            dst[i * 4 + 3] = a;
        }
        GlobalUnlock(hDib);
        SetClipboardData(CF_DIBV5, hDib);
    }

    CloseClipboard();
    std::cout << "[Clipboard] Copied " << w << "x" << h << " PNG + DIBv5\n";
#endif
}

void Renderer::update_window_shape() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window_, &info)) return;

    if (!phone_frame_enabled_ || frame_dst_w_ == 0) {
        SetWindowRgn(info.info.win.window, nullptr, TRUE);
        return;
    }

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int cr = (int)(phone_frame_.corner_radius() * scale);
    if (cr < 4) cr = 4;

    HRGN phone_rgn = CreateRoundRectRgn(
        frame_dst_x_, frame_dst_y_,
        frame_dst_x_ + frame_dst_w_ + 1,
        frame_dst_y_ + frame_dst_h_ + 1,
        cr * 2, cr * 2);

    if (log_panel_anim_ > 0.01f) {
        // Combine phone frame region with log panel region (right side, drawer)
        // Flat left edge, rounded right edge only
        int lp_w = (int)(frame_dst_h_ * 0.55f * log_panel_anim_);
        int lp_margin = std::max(4, frame_dst_h_ / 40);
        int drawer_inset = frame_dst_h_ / 16;
        int lp_cr = std::max(8, cr / 2);
        int lp_left = frame_dst_x_ + frame_dst_w_; // flush with phone
        int lp_right = frame_dst_x_ + frame_dst_w_ + lp_w - lp_margin;
        int lp_top = frame_dst_y_ + drawer_inset;
        int lp_bottom = frame_dst_y_ + frame_dst_h_ - drawer_inset + 1;
        if (lp_right - lp_left > lp_cr * 2) {
            // Right-rounded region
            HRGN log_round = CreateRoundRectRgn(
                lp_left, lp_top,
                lp_right + 1, lp_bottom,
                lp_cr * 2, lp_cr * 2);
            // Flat-left rect to fill in left rounded corners
            HRGN log_flat = CreateRectRgn(
                lp_left, lp_top,
                lp_left + lp_cr, lp_bottom);
            if (log_round && log_flat) {
                CombineRgn(log_round, log_round, log_flat, RGN_OR);
                CombineRgn(phone_rgn, phone_rgn, log_round, RGN_OR);
                DeleteObject(log_flat);
                DeleteObject(log_round);
            } else {
                if (log_round) DeleteObject(log_round);
                if (log_flat) DeleteObject(log_flat);
            }
        }
    }

    SetWindowRgn(info.info.win.window, phone_rgn, TRUE);
#endif
}

void Renderer::begin_window_drag() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window_, &info)) {
        ReleaseCapture();
        SendMessage(info.info.win.window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
#endif
}

void Renderer::stop() { running_.store(false); }

void Renderer::set_pin_code(const std::string& pin) {
    std::lock_guard<std::mutex> lk(pin_mutex_);
    pin_code_ = pin;
}

void Renderer::draw_pin_overlay() {
    if (!sdl_renderer_ || !window_) return;
    std::string pin;
    {
        std::lock_guard<std::mutex> lk(pin_mutex_);
        pin = pin_code_;
    }
    if (pin.empty()) {
        // Free cached textures when not in use.
        if (pin_label_tex_)  { SDL_DestroyTexture(pin_label_tex_);  pin_label_tex_  = nullptr; }
        if (pin_digits_tex_) { SDL_DestroyTexture(pin_digits_tex_); pin_digits_tex_ = nullptr; }
        pin_digits_cached_.clear();
        return;
    }

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    // Build textures lazily / when PIN changes.
    if (!pin_label_tex_) {
        pin_label_tex_ = make_text_texture(sdl_renderer_, "AirPlay PIN",
                                           36, 220, 220, 220,
                                           &pin_label_w_, &pin_label_h_);
    }
    if (!pin_note_tex_) {
        pin_note_tex_ = make_text_texture(sdl_renderer_,
                                          "Note: PIN pairing is experimental and may not work on all devices.",
                                          14, 170, 170, 170,
                                          &pin_note_w_, &pin_note_h_);
    }
    if (pin != pin_digits_cached_) {
        if (pin_digits_tex_) { SDL_DestroyTexture(pin_digits_tex_); pin_digits_tex_ = nullptr; }
        // Letter-spaced for readability ("1 2 3 4")
        std::string spaced;
        for (size_t i = 0; i < pin.size(); ++i) {
            if (i) spaced += ' ';
            spaced += pin[i];
        }
        pin_digits_tex_ = make_text_texture(sdl_renderer_, spaced,
                                             140, 255, 255, 255,
                                             &pin_digits_w_, &pin_digits_h_);
        pin_digits_cached_ = pin;
    }

    // Centered card with semi-transparent dark backdrop.
    int card_w = std::max(420, std::max(pin_digits_w_, pin_note_w_) + 80);
    int card_h = pin_label_h_ + pin_digits_h_ + pin_note_h_ + 96;
    int card_x = (win_w - card_w) / 2;
    int card_y = (win_h - card_h) / 2;

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // Dim the screen behind the card.
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 160);
    SDL_Rect bg = {0, 0, win_w, win_h};
    SDL_RenderFillRect(sdl_renderer_, &bg);

    // Card background.
    SDL_SetRenderDrawColor(sdl_renderer_, 28, 28, 32, 235);
    SDL_Rect card = {card_x, card_y, card_w, card_h};
    SDL_RenderFillRect(sdl_renderer_, &card);

    // Card border.
    SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, 255);
    SDL_RenderDrawRect(sdl_renderer_, &card);

    // Label.
    if (pin_label_tex_) {
        SDL_Rect dst = {card_x + (card_w - pin_label_w_) / 2,
                        card_y + 18,
                        pin_label_w_, pin_label_h_};
        SDL_RenderCopy(sdl_renderer_, pin_label_tex_, nullptr, &dst);
    }
    // Digits.
    if (pin_digits_tex_) {
        SDL_Rect dst = {card_x + (card_w - pin_digits_w_) / 2,
                        card_y + 18 + pin_label_h_ + 12,
                        pin_digits_w_, pin_digits_h_};
        SDL_RenderCopy(sdl_renderer_, pin_digits_tex_, nullptr, &dst);
    }
    // Footnote.
    if (pin_note_tex_) {
        SDL_Rect dst = {card_x + (card_w - pin_note_w_) / 2,
                        card_y + 18 + pin_label_h_ + 12 + pin_digits_h_ + 16,
                        pin_note_w_, pin_note_h_};
        SDL_RenderCopy(sdl_renderer_, pin_note_tex_, nullptr, &dst);
    }
}

} // namespace openmirror::media
