#include <openmirror/media/renderer.h>
#include <openmirror/log_buffer.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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
    // Add right-padding so glyph overhang (italic / ClearType subpixel /
    // last-char ABC overhang) is not clipped. GetTextExtentPoint32 reports
    // advance widths only, not the actual ink bounds of the last glyph.
    {
        TEXTMETRICA tm;
        if (GetTextMetricsA(hdc, &tm)) sz.cx += tm.tmOverhang;
        sz.cx += (font_height + 3) / 4;
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

// Wide/Unicode version (for ♫, ©, etc.). Optional font_name lets
// callers pick e.g. "Segoe UI Symbol" for glyphs missing from "Segoe UI".
static SDL_Texture* make_text_texture_w(SDL_Renderer* renderer, const std::wstring& text,
                                          int font_height, uint8_t cr, uint8_t cg, uint8_t cb,
                                          int* out_w, int* out_h,
                                          const wchar_t* font_name = L"Segoe UI") {
    if (text.empty()) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT font = CreateFontW(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, font_name);
    HFONT old_font = (HFONT)SelectObject(hdc, font);

    SIZE sz;
    GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    // Add right-padding so glyph overhang (italic / ClearType subpixel /
    // last-char ABC overhang) is not clipped. GetTextExtentPoint32 reports
    // advance widths only, not the actual ink bounds of the last glyph.
    {
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdc, &tm)) sz.cx += tm.tmOverhang;
        sz.cx += (font_height + 3) / 4;
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

// Cheap GDI text measurement (no texture creation). Includes the same
// overhang/ClearType padding used by make_text_texture so the result matches
// what will actually be rasterised.
static int measure_text_width_a(const std::string& text, int font_height) {
    if (text.empty()) return 0;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT font = CreateFontA(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SIZE sz{};
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    TEXTMETRICA tm{};
    if (GetTextMetricsA(hdc, &tm)) sz.cx += tm.tmOverhang;
    sz.cx += (font_height + 3) / 4;
    SelectObject(hdc, old_font);
    DeleteObject(font);
    DeleteDC(hdc);
    return sz.cx;
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

    // Load persisted user settings (bezel colour, screenshot toggles) BEFORE
    // the first phone-frame generate so the initial texture uses the saved
    // colour. Otherwise set_bezel_color() would invalidate the freshly-built
    // frame texture and the waiting screen would never render.
    settings_ = openmirror::Settings::load();
    phone_frame_.set_bezel_color(settings_.bezel_r, settings_.bezel_g, settings_.bezel_b);

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
        "Press A  -  Settings > Wireless debugging > Pair with code",
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
                        const std::string& url = "", const std::string& tip = "",
                        const wchar_t* font = L"Segoe UI") -> FooterSeg {
            FooterSeg fs;
            fs.url = url;
            fs.tooltip = tip;
            fs.tex = make_text_texture_w(sdl_renderer_, text, 40, r, g, b, &fs.w, &fs.h, font);
            return fs;
        };
        // Line 1: "1PhoneMirror by MSEndpointMgr"
        footer_line1_.push_back(seg(L"1PhoneMirror by ", 120, 120, 120));
        footer_line1_.push_back(seg(L"MSEndpointMgr", 120, 120, 120,
                                     "https://msendpointmgr.com/", "Open MSEndpointMgr"));
        // Line 2: "(c) 2026 \u266B Simon Skotheimsvik, MVP \u00B7 v0.2.4"
        footer_line2_.push_back(seg(L"\u00A9 2026 ", 100, 100, 100));
        // Beamed-eighth-notes glyph — render via Segoe UI Symbol so it works
        // on Windows builds where the regular Segoe UI font lacks U+266B.
        footer_line2_.push_back(seg(L"\u266B", 100, 100, 100,
                                     "", "Tuned carefully for the community",
                                     L"Segoe UI Symbol"));
        footer_line2_.push_back(seg(L" ", 100, 100, 100));
        footer_line2_.push_back(seg(L"Simon Skotheimsvik, MVP", 100, 100, 100,
                                     "https://linktr.ee/simonskotheimsvik", "More info of Simon"));
        footer_line2_.push_back(seg(L" \u00B7 v0.2.4", 100, 100, 100,
                                     "", "Version history (V)"));
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
        info_lines_.push_back(make_info(L"1PhoneMirror v0.2.4", 44, 255, 255, 255));
        info_lines_.push_back(make_info(L"AirPlay (iOS) \u00B7 scrcpy (Android)", 34, 160, 160, 160));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"(F) Fullscreen \u00B7 (M) Menu \u00B7 (L) Log \u00B7 (A) Add Android", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(I) Info \u00B7 (V) Version \u00B7 (S) Settings \u00B7 (Esc) Quit", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(Ctrl+S) Screenshot \u00B7 In log: (Ctrl+C) Copy \u00B7 (Ctrl+X) Clear", 30, 130, 130, 130));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"Network requirements", 34, 160, 160, 160));
        info_lines_.push_back(make_info(L"Same Wi-Fi / VLAN as the phone, mDNS (UDP 5353) allowed", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"AirPlay: TCP 7000/7001/7100, UDP 6000-6010", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"Android: TCP 27183 (loopback) + ADB pair port from phone", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"Installer adds Windows Firewall rules for the .exe", 30, 130, 130, 130));
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
        version_lines_.push_back(make_ver(L"08.05.2026 \u2013 0.2.4", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Refined bezel toggles and auto-collapse on connect", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"08.05.2026 \u2013 0.2.3", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Better Android experience and quicker screenshots", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"08.05.2026 \u2013 0.2.2", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Settings: identify as computer name on the network", 30, 160, 160, 160));
        version_lines_.push_back(make_ver(L"Bezel device dots show play/pause icon per source", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"07.05.2026 \u2013 0.2.1", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Settings panel (S): bezel colour + screenshot options", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.2.0", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Android mirroring (Wireless debugging, press A)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.1.6", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Multiple iOS devices stay paired, switch from bezel dots", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.1.5", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"AirPlay PIN pairing for trusted-device security", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"06.05.2026 \u2013 0.1.4", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Slide-out log viewer with live filtering (press L)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"05.05.2026 \u2013 0.1.2", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Dynamic island menu in top bezel for quick actions", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"05.05.2026 \u2013 0.1.1", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Screenshot capture to clipboard and Pictures folder", 30, 160, 160, 160));
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

    // (Settings already loaded earlier in init() before the first phone-frame
    // generate so the initial waiting screen uses the saved bezel colour.)

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
    if (bezel_tip_tex2_) { SDL_DestroyTexture(bezel_tip_tex2_); bezel_tip_tex2_ = nullptr; }
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
                // When the Android panel is open, capture keys for editing.
                if (android_panel_visible_) {
                    auto& fld = (android_focus_ == 0 ? android_ip_
                              :  android_focus_ == 1 ? android_connect_port_
                              :  android_focus_ == 2 ? android_port_
                              :                        android_pin_);
                    if (event.key.keysym.sym == SDLK_BACKSPACE) {
                        if (!fld.empty()) fld.pop_back();
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_TAB) {
                        android_focus_ = (android_focus_ + 1) % 4;
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_RETURN ||
                        event.key.keysym.sym == SDLK_KP_ENTER) {
                        android_submit();
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        android_panel_visible_ = false;
                        android_panel_animating_ = true;
                        android_panel_anim_start_ = std::chrono::steady_clock::now();
                        SDL_StopTextInput();
                        break;
                    }
                    // Swallow everything else; SDL_TEXTINPUT delivers chars.
                    break;
                }

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
                    if (info_panel_visible_ && settings_panel_visible_) {
                        settings_panel_visible_ = false;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
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
                    if (version_panel_visible_ && settings_panel_visible_) {
                        settings_panel_visible_ = false;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                }
                if (event.key.keysym.sym == SDLK_s && (event.key.keysym.mod & KMOD_CTRL)) {
                    screenshot_requested_ = true;
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Plain (S) without Ctrl toggles the Settings panel.
                if (event.key.keysym.sym == SDLK_s && !(event.key.keysym.mod & KMOD_CTRL)) {
                    settings_panel_visible_ = !settings_panel_visible_;
                    settings_panel_animating_ = true;
                    settings_panel_anim_start_ = std::chrono::steady_clock::now();
                    if (settings_panel_visible_) {
                        if (info_panel_visible_) {
                            info_panel_visible_ = false;
                            info_panel_animating_ = true;
                            info_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                        if (version_panel_visible_) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                    }
                }
                // Log shortcuts: only when log panel is visible and no
                // text input panel is open.
                if (log_panel_visible_ && !android_panel_visible_ &&
                    (event.key.keysym.mod & KMOD_CTRL)) {
                    if (event.key.keysym.sym == SDLK_c) {
                        std::string all;
                        for (auto& ln : openmirror::LogBuffer::instance().get_lines()) {
                            all += ln; all += '\n';
                        }
                        SDL_SetClipboardText(all.c_str());
                        std::cout << "[Renderer] Log copied to clipboard ("
                                  << all.size() << " bytes)\n";
                    } else if (event.key.keysym.sym == SDLK_x) {
                        openmirror::LogBuffer::instance().clear();
                        std::cout << "[Renderer] Log cleared\n";
                    }
                }
                if (event.key.keysym.sym == SDLK_a && add_android_fn_) {
                    show_android_panel();
                }
                break;

            case SDL_TEXTINPUT:
                if (android_panel_visible_) {
                    auto& fld = (android_focus_ == 0 ? android_ip_
                              :  android_focus_ == 1 ? android_connect_port_
                              :  android_focus_ == 2 ? android_port_
                              :                        android_pin_);
                    for (const char* p = event.text.text; *p; ++p) {
                        char c = *p;
                        if (android_focus_ == 0) {
                            // IP: digits and dots
                            if ((c >= '0' && c <= '9') || c == '.') fld.push_back(c);
                        } else {
                            // Ports + PIN: digits only
                            if (c >= '0' && c <= '9') fld.push_back(c);
                        }
                    }
                    if (android_focus_ == 1 && android_connect_port_.size() > 5)
                        android_connect_port_.resize(5);
                    if (android_focus_ == 2 && android_port_.size() > 5)
                        android_port_.resize(5);
                    if (android_focus_ == 3 && android_pin_.size() > 6)
                        android_pin_.resize(6);
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    running_.store(false);
                    return;
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                // Android panel intercepts left-clicks while open.
                if (android_panel_visible_ && event.button.button == SDL_BUTTON_LEFT) {
                    int mx = event.button.x, my = event.button.y;
                    auto in = [&](const BtnRect& r) {
                        return r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h);
                    };
                    // When the help overlay is showing, it captures all
                    // clicks. Only the help close button (or the help
                    // toggle) dismisses it; clicks inside the help body
                    // do nothing, clicks outside do nothing (so they
                    // don't accidentally close the parent connect panel).
                    if (android_help_visible_) {
                        if (in(android_help_close_btn_) || in(android_help_btn_)) {
                            android_help_visible_ = false;
                        }
                        break;
                    }
                    if (in(android_close_btn_)) {
                        android_panel_visible_ = false;
                        android_panel_animating_ = true;
                        android_panel_anim_start_ = std::chrono::steady_clock::now();
                        SDL_StopTextInput();
                        break;
                    }
                    if (in(android_help_btn_)) {
                        android_help_visible_ = !android_help_visible_;
                        break;
                    }
                    if (in(android_connect_btn_) && !android_busy_) {
                        android_submit();
                        break;
                    }
                    if (in(android_disconnect_btn_)) {
                        if (android_disconnect_fn_) android_disconnect_fn_();
                        std::lock_guard lk(android_status_mutex_);
                        android_status_ = "Disconnected.";
                        android_status_is_error_ = false;
                        break;
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (in(android_field_rects_[i])) { android_focus_ = i; break; }
                    }
                    // Click outside panel? close it.
                    if (!in_rect(mx, my, android_panel_rect_.x, android_panel_rect_.y,
                                 android_panel_rect_.w, android_panel_rect_.h)) {
                        android_panel_visible_ = false;
                        android_panel_animating_ = true;
                        android_panel_anim_start_ = std::chrono::steady_clock::now();
                        SDL_StopTextInput();
                    }
                    break;
                }

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
                            } else if (clicked_action == "clear") {
                                openmirror::LogBuffer::instance().clear();
                                std::cout << "[Renderer] Log cleared\n";
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
                        // Track the PHONE width as the starting size, not the
                        // whole window width — the window includes the log
                        // drawer when it is open and the auto-resize loop in
                        // render_frame() forces window_w = phone_w + drawer_w
                        // every frame, which would otherwise undo the resize.
                        resize_start_w_ = frame_dst_w_;
                        resize_start_h_ = frame_dst_h_;
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
                        int lp_w = (int)(log_panel_full_w_ * log_panel_anim_);
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

                    // Bezel screenshot button (only present when the menu
                    // is hidden — same shortcut as Ctrl+S / island camera).
                    if (bezel_screenshot_btn_.w > 0 &&
                        in_rect(mx, my, bezel_screenshot_btn_.x, bezel_screenshot_btn_.y,
                                bezel_screenshot_btn_.w, bezel_screenshot_btn_.h)) {
                        screenshot_requested_ = true;
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }

                    // Bezel close (X) button — only present when the menu
                    // is hidden. Same effect as the island close button.
                    if (bezel_close_btn_.w > 0 &&
                        in_rect(mx, my, bezel_close_btn_.x, bezel_close_btn_.y,
                                bezel_close_btn_.w, bezel_close_btn_.h)) {
                        running_.store(false);
                        return;
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
                        if (info_panel_visible_ && settings_panel_visible_) {
                            settings_panel_visible_ = false;
                            settings_panel_animating_ = true;
                            settings_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                        break;
                    }
                    // Footer link clicks (waiting screen only)
                    if (!ever_received_frame_) {
                        bool handled = false;
                        for (auto& hit : footer_hits_) {
                            if (in_rect(mx, my, hit.x, hit.y, hit.w, hit.h)) {
                                if (hit.tooltip.rfind("Version history", 0) == 0) {
                                    version_panel_visible_ = !version_panel_visible_;
                                    version_panel_animating_ = true;
                                    version_panel_anim_start_ = std::chrono::steady_clock::now();
                                    version_scroll_offset_ = 0;
                                    if (version_panel_visible_ && info_panel_visible_) {
                                        info_panel_visible_ = false;
                                        info_panel_animating_ = true;
                                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                                    }
                                    if (version_panel_visible_ && settings_panel_visible_) {
                                        settings_panel_visible_ = false;
                                        settings_panel_animating_ = true;
                                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
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
                    // Settings (gear) button — toggle settings panel
                    if (settings_btn_.w > 0 &&
                        in_rect(mx, my, settings_btn_.x, settings_btn_.y,
                                settings_btn_.w, settings_btn_.h)) {
                        settings_panel_visible_ = !settings_panel_visible_;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                        // Close other panels for clarity
                        if (settings_panel_visible_) {
                            if (info_panel_visible_) {
                                info_panel_visible_ = false;
                                info_panel_animating_ = true;
                                info_panel_anim_start_ = std::chrono::steady_clock::now();
                            }
                            if (version_panel_visible_) {
                                version_panel_visible_ = false;
                                version_panel_animating_ = true;
                                version_panel_anim_start_ = std::chrono::steady_clock::now();
                            }
                        }
                        break;
                    }
                    // Settings panel inner clicks (swatches + toggles).
                    // Use a low threshold so clicks register as soon as the
                    // panel is mostly open, rather than waiting for the full
                    // 200ms slide-in animation to finish.
                    if (settings_panel_visible_ && settings_panel_anim_ > 0.3f) {
                        bool inside = in_rect(mx, my, settings_panel_rect_.x, settings_panel_rect_.y,
                                              settings_panel_rect_.w, settings_panel_rect_.h);
                        if (inside) {
                            for (auto& sw : settings_swatch_btns_) {
                                if (in_rect(mx, my, sw.second.x, sw.second.y, sw.second.w, sw.second.h)) {
                                    static const uint8_t presets[][3] = {
                                        { 28,  28,  30},  // Dark titanium (default)
                                        { 90,  90,  95},  // Graphite
                                        { 36,  46,  72},  // Midnight blue
                                        { 36,  60,  44},  // Forest green
                                        { 80,  34,  34},  // Deep red
                                        {110,  84,  46},  // Bronze
                                    };
                                    int idx = sw.first;
                                    if (idx >= 0 && idx < (int)(sizeof(presets) / sizeof(presets[0]))) {
                                        apply_bezel_color(presets[idx][0], presets[idx][1], presets[idx][2]);
                                    }
                                    break;
                                }
                            }
                            if (in_rect(mx, my, settings_toggle_save_btn_.x, settings_toggle_save_btn_.y,
                                        settings_toggle_save_btn_.w, settings_toggle_save_btn_.h)) {
                                settings_.screenshot_save_to_folder = !settings_.screenshot_save_to_folder;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_clip_btn_.x, settings_toggle_clip_btn_.y,
                                        settings_toggle_clip_btn_.w, settings_toggle_clip_btn_.h)) {
                                settings_.screenshot_copy_to_clipboard = !settings_.screenshot_copy_to_clipboard;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_compname_btn_.x, settings_toggle_compname_btn_.y,
                                        settings_toggle_compname_btn_.w, settings_toggle_compname_btn_.h)) {
                                settings_.use_computer_name = !settings_.use_computer_name;
                                settings_.save();
                            }
                            break;
                        } else {
                            // Click outside the open settings panel closes it.
                            settings_panel_visible_ = false;
                            settings_panel_animating_ = true;
                            settings_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
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
                    // Dismiss info panel on click outside its rect.
                    // Placed AFTER all bezel-button checks so that clicking
                    // a bezel button (e.g. the gear) is not eaten by this
                    // dismiss handler.
                    if (info_panel_visible_ && info_panel_anim_ >= 1.0f) {
                        if (!in_rect(mx, my, info_panel_rect_.x, info_panel_rect_.y,
                                     info_panel_rect_.w, info_panel_rect_.h)) {
                            info_panel_visible_ = false;
                            info_panel_animating_ = true;
                            info_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
                    }
                    // Dismiss version panel on click outside its rect.
                    if (version_panel_visible_ && version_panel_anim_ >= 1.0f) {
                        if (!in_rect(mx, my, version_panel_rect_.x, version_panel_rect_.y,
                                     version_panel_rect_.w, version_panel_rect_.h)) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
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
                    // resize_start_w_/h_ are the PHONE dimensions at drag start.
                    int new_phone_w = std::max(200, resize_start_w_ + (gmx - resize_start_gx_));
                    SDL_DisplayMode dm;
                    SDL_GetCurrentDisplayMode(0, &dm);
                    int new_phone_h = (int)(new_phone_w * aspect);
                    // Add the current drawer width (scales with phone height)
                    // so the resulting window fits both phone and drawer and
                    // the auto-resize in render_frame() will not fight it.
                    int drawer_full_w = (int)(new_phone_h * 0.55f);
                    int drawer_w = (int)(drawer_full_w * log_panel_anim_);
                    int margin = std::max(4, new_phone_h / 40);
                    int needed_w = new_phone_w + std::max(0, drawer_w - margin);
                    if (needed_w > (int)(dm.w * 0.95f)) {
                        needed_w = (int)(dm.w * 0.95f);
                        new_phone_w = needed_w - std::max(0, drawer_w - margin);
                        new_phone_h = (int)(new_phone_w * aspect);
                    }
                    if (new_phone_h > (int)(dm.h * 0.95f)) {
                        new_phone_h = (int)(dm.h * 0.95f);
                        new_phone_w = (int)(new_phone_h / aspect);
                        drawer_full_w = (int)(new_phone_h * 0.55f);
                        drawer_w = (int)(drawer_full_w * log_panel_anim_);
                        margin = std::max(4, new_phone_h / 40);
                        needed_w = new_phone_w + std::max(0, drawer_w - margin);
                    }
                    SDL_SetWindowSize(window_, needed_w, new_phone_h);
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
            // Reveal the island menu again so the waiting screen offers
            // its full set of pairing/help affordances (counterpart to the
            // auto-collapse on first frame).
            if (!island_visible_) {
                island_visible_ = true;
                island_animating_ = true;
                island_anim_start_ = std::chrono::steady_clock::now();
            }
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

                // A new stream started (first frame, or a different device
                // joined) — auto-collapse the island menu so the freshly
                // mirrored screen is unobstructed. The bezel chevron, X
                // and screenshot icons remain visible for quick access.
                if (island_visible_) {
                    island_visible_ = false;
                    island_animating_ = true;
                    island_anim_start_ = std::chrono::steady_clock::now();
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
        float duration = 260.0f; // ms
        float t = std::min(1.0f, elapsed / duration);
        // Ease-in-out cubic: smooth start AND smooth end (no harsh deceleration
        // that makes the drawer look like it "stops" near full extension).
        float eased = (t < 0.5f)
                          ? 4.0f * t * t * t
                          : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
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

    // Log panel full-open width: fixed at 55% of phone height (legacy look).
    log_panel_full_w_ = (int)(frame_dst_h_ * 0.55f);

    // Log panel width based on frame height
    int log_panel_w = (int)(log_panel_full_w_ * log_panel_anim_);

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
    if (settings_panel_visible_ || settings_panel_animating_) draw_settings_panel();
    if (android_panel_visible_ || android_panel_animating_) draw_android_panel();
    if (android_panel_visible_ && android_help_visible_) draw_android_help();

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

        // Toggle indicator — closed "-" and open "v" share the same
        // horizontal span. The chevron is drawn with a thinner stroke so it
        // doesn't read as bold next to the slim collapsed bar.
        int g_sz   = std::max(8, dot_r * 6);     // total span (line length)
        int g_half = g_sz / 2;
        int t_bar  = std::max(1, dot_r);         // thickness of the "-" bar
        int t_chev = std::max(1, dot_r - 1);     // thinner chevron stroke
        auto fill_rect_centered = [&](int cx, int cy, int w, int h) {
            SDL_Rect r{cx - w / 2, cy - h / 2, w, h};
            SDL_RenderFillRect(sdl_renderer_, &r);
        };
        auto draw_thick_seg = [&](int x1, int y1, int x2, int y2, int t) {
            int half = t / 2;
            for (int dx = -half; dx <= half; ++dx)
                for (int dy = -half; dy <= half; ++dy)
                    SDL_RenderDrawLine(sdl_renderer_, x1 + dx, y1 + dy,
                                                       x2 + dx, y2 + dy);
        };
        if (island_anim_ > 0.5f) {
            // Open state — downward chevron "v" spanning the same width
            // as the collapsed bar.
            int v_drop = g_half / 2;             // vertical depth of the V
            draw_thick_seg(star_cx - g_half, star_cy - v_drop / 2,
                           star_cx,          star_cy + v_drop, t_chev);
            draw_thick_seg(star_cx + g_half, star_cy - v_drop / 2,
                           star_cx,          star_cy + v_drop, t_chev);
        } else {
            // Closed state — horizontal bar "-".
            fill_rect_centered(star_cx, star_cy, g_sz, t_bar);
        }
        if (star_hover) {
            bezel_hover_key = "menu";
            bezel_hover_text = "Menu (M)";
            bezel_hover_ax = star_cx;
            bezel_hover_ay = star_cy + dot_r + 4;
        }
    }

    // Mini screenshot button in the top bezel — shown only when the island
    // menu is hidden so users still have a clickable shortcut for Ctrl+S.
    // Horizontal position matches the screenshot button on the island so
    // the affordance feels continuous when the menu is toggled.
    bezel_screenshot_btn_ = {};
    bezel_close_btn_ = {};
    if (island_anim_ < 0.5f) {
        float scale_b = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_top = (int)(phone_frame_.screen_y() * scale_b);
        int svx_b = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale_b);
        int svw_b = (int)(phone_frame_.screen_width() * scale_b);
        int svh_b = (int)(phone_frame_.screen_height() * scale_b);
        // Mirror the maths inside draw_island() so the X position is identical.
        int phone_eq_w = std::min(svw_b, svh_b / 2);
        int btn_sz_full = std::max(20, phone_eq_w / 14);
        int pad_full = btn_sz_full / 3;
        int gap_full = pad_full / 2 + 2;
        int island_w = std::max(160, (int)(phone_eq_w * 0.80f));
        int island_x = svx_b + (svw_b - island_w) / 2;
        // Order on the island (right→left): close, folder, screenshot.
        int ss_full_x = island_x + island_w - pad_full - btn_sz_full
                       - 2 * (btn_sz_full + gap_full);
        int ss_full_cx = ss_full_x + btn_sz_full / 2;

        // Mini button — sized to fit inside the top bezel with breathing room.
        int mini_sz = std::max(12, std::min(btn_sz_full * 2 / 3, bezel_top - 4));
        if (mini_sz < 12) mini_sz = 12;
        int mini_x = ss_full_cx - mini_sz / 2;
        int mini_y = frame_dst_y_ + (bezel_top - mini_sz) / 2;
        bezel_screenshot_btn_ = {mini_x, mini_y, mini_sz, mini_sz};

        int bmx, bmy;
        SDL_GetMouseState(&bmx, &bmy);
        bool bhov = in_rect(bmx, bmy, mini_x, mini_y, mini_sz, mini_sz);
        auto now_b = std::chrono::steady_clock::now();
        bool flashing_b = btn_flash_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now_b - btn_flash_start_).count() < 200;
        // Subtle background — reads as a button only on hover/flash.
        uint8_t bg = flashing_b ? 200 : (bhov ? 70 : 40);
        SDL_SetRenderDrawColor(sdl_renderer_, bg, bg, bg + 5, bhov || flashing_b ? 200 : 130);
        fill_circle(sdl_renderer_, mini_x + mini_sz / 2,
                    mini_y + mini_sz / 2, mini_sz / 2);
        // Camera glyph — same recipe as the island button (ring + center dot).
        int cr = std::max(2, mini_sz / 4);
        int ccx = mini_x + mini_sz / 2;
        int ccy = mini_y + mini_sz / 2;
        uint8_t ca = flashing_b ? 255 : (bhov ? 255 : 200);
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

        if (bhov) {
            bezel_hover_key = "bezel_ss";
            bezel_hover_text = "Screenshot (Ctrl+S)";
            bezel_hover_ax = ccx;
            bezel_hover_ay = mini_y + mini_sz + 4;
        }

        // Bezel close button — same vertical/horizontal placement strategy
        // as the bezel screenshot button, anchored to the island's close
        // button X. Drawn as two thin diagonal strokes (X) that match the
        // chevron aesthetic used by the menu/log toggle indicators rather
        // than a filled circle with a glyph.
        int close_full_x  = island_x + island_w - pad_full - btn_sz_full;
        int close_full_cx = close_full_x + btn_sz_full / 2;
        int close_mini_x  = close_full_cx - mini_sz / 2;
        int close_mini_y  = mini_y;
        bezel_close_btn_ = {close_mini_x, close_mini_y, mini_sz, mini_sz};

        bool xhov = in_rect(bmx, bmy, close_mini_x, close_mini_y, mini_sz, mini_sz);
        // Stroke geometry mirrors the menu/log toggle: same thinner chevron
        // stroke so the three indicators read as a set. Length is clamped a
        // bit tighter than the chevrons so the diagonals don't graze the
        // bezel edges (the X reaches the corners of its bounding box, where
        // a chevron only reaches one side).
        int dot_r_close = std::max(2, frame_dst_w_ / 200);
        int xg_sz   = std::max(6, dot_r_close * 4);
        // Also keep the X comfortably inside the mini button — leave at
        // least ~25% padding on every side.
        if (xg_sz > mini_sz * 3 / 4) xg_sz = mini_sz * 3 / 4;
        int xg_half = xg_sz / 2;
        int xt_chev = std::max(1, dot_r_close - 1);
        int xcx_c   = close_mini_x + mini_sz / 2;
        int xcy_c   = close_mini_y + mini_sz / 2;
        // Hover/normal alpha follows the toggle-indicator convention.
        uint8_t xa = xhov ? 240 : 160;
        // On hover, tint subtly red to communicate "destructive" affordance
        // without breaking the minimalist look.
        if (xhov) SDL_SetRenderDrawColor(sdl_renderer_, 235, 110, 110, xa);
        else      SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, xa);
        auto draw_thick_seg_x = [&](int x1, int y1, int x2, int y2, int t) {
            int half = t / 2;
            for (int dx = -half; dx <= half; ++dx)
                for (int dy = -half; dy <= half; ++dy)
                    SDL_RenderDrawLine(sdl_renderer_, x1 + dx, y1 + dy,
                                                       x2 + dx, y2 + dy);
        };
        draw_thick_seg_x(xcx_c - xg_half, xcy_c - xg_half,
                         xcx_c + xg_half, xcy_c + xg_half, xt_chev);
        draw_thick_seg_x(xcx_c - xg_half, xcy_c + xg_half,
                         xcx_c + xg_half, xcy_c - xg_half, xt_chev);

        if (xhov) {
            bezel_hover_key = "bezel_close";
            bezel_hover_text = "Close (Esc)";
            bezel_hover_ax = xcx_c;
            bezel_hover_ay = close_mini_y + mini_sz + 4;
        }

        // Standalone screenshot toast — shown when the island is hidden so
        // the "Saved to Pictures" / "Copied to clipboard" confirmation is
        // never lost just because the menu happens to be collapsed.
        if (toast_active_) {
            auto elapsed_t = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - toast_start_).count();
            if (elapsed_t < 1500) {
#ifdef _WIN32
                if (toast_tex_str_ != toast_text_) {
                    if (toast_tex_) { SDL_DestroyTexture(toast_tex_); toast_tex_ = nullptr; }
                    int fh_toast = std::max(28, btn_sz_full * 2);
                    toast_tex_ = make_text_texture(sdl_renderer_, toast_text_, fh_toast,
                                                    230, 230, 230,
                                                    &toast_tex_w_, &toast_tex_h_);
                    toast_tex_str_ = toast_text_;
                }
#endif
                if (toast_tex_) {
                    uint8_t alpha = 255;
                    if (elapsed_t < 150) alpha = (uint8_t)(elapsed_t * 255 / 150);
                    else if (elapsed_t > 1200)
                        alpha = (uint8_t)((1500 - elapsed_t) * 255 / 300);
                    float t_scale = (float)std::max(11, btn_sz_full / 2)
                                  / std::max(28, btn_sz_full * 2);
                    int disp_tw = (int)(toast_tex_w_ * t_scale);
                    int disp_th = (int)(toast_tex_h_ * t_scale);
                    int pillx_pad = std::max(8, btn_sz_full / 3);
                    int pill_w = disp_tw + pillx_pad * 2;
                    int pill_h = disp_th + std::max(6, pillx_pad / 2) * 2;
                    int pill_x = svx_b + (svw_b - pill_w) / 2;
                    int pill_y = frame_dst_y_ + bezel_top + std::max(6, btn_sz_full / 4);
                    int pr = pill_h / 2;
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 22,
                                           (uint8_t)(alpha * 200 / 255));
                    SDL_Rect body = {pill_x + pr, pill_y, pill_w - pr * 2, pill_h};
                    SDL_RenderFillRect(sdl_renderer_, &body);
                    fill_circle(sdl_renderer_, pill_x + pr, pill_y + pr, pr);
                    fill_circle(sdl_renderer_, pill_x + pill_w - pr, pill_y + pr, pr);
                    SDL_SetTextureAlphaMod(toast_tex_, alpha);
                    SDL_Rect tdst = {pill_x + pillx_pad,
                                     pill_y + (pill_h - disp_th) / 2,
                                     disp_tw, disp_th};
                    SDL_RenderCopy(sdl_renderer_, toast_tex_, nullptr, &tdst);
                    SDL_SetTextureAlphaMod(toast_tex_, 255);
                }
            } else {
                toast_active_ = false;
            }
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

        // Toggle indicator — closed "|" and open ">" share the same
        // vertical span. Chevron uses a thinner stroke so it does not
        // read as bold next to the slim collapsed bar.
        int g_sz   = std::max(8, dot_r * 6);
        int g_half = g_sz / 2;
        int t_bar  = std::max(1, dot_r);
        int t_chev = std::max(1, dot_r - 1);
        auto fill_rect_centered = [&](int cx, int cy, int w, int h) {
            SDL_Rect r{cx - w / 2, cy - h / 2, w, h};
            SDL_RenderFillRect(sdl_renderer_, &r);
        };
        auto draw_thick_seg = [&](int x1, int y1, int x2, int y2, int t) {
            int half = t / 2;
            for (int dx = -half; dx <= half; ++dx)
                for (int dy = -half; dy <= half; ++dy)
                    SDL_RenderDrawLine(sdl_renderer_, x1 + dx, y1 + dy,
                                                       x2 + dx, y2 + dy);
        };
        if (log_panel_anim_ > 0.5f) {
            int h_reach = g_half / 2;
            draw_thick_seg(star_cx - h_reach / 2, star_cy - g_half,
                           star_cx + h_reach,    star_cy, t_chev);
            draw_thick_seg(star_cx - h_reach / 2, star_cy + g_half,
                           star_cx + h_reach,    star_cy, t_chev);
        } else {
            fill_rect_centered(star_cx, star_cy, t_bar, g_sz);
        }
        if (log_hover) {
            bezel_hover_key = "log";
            bezel_hover_text = "Show log (L)";
            bezel_hover_ax = star_cx - dot_r * 6;
            bezel_hover_ay = star_cy;
        }
    }

    // Source picker — small dots in bottom bezel, one per connected device.
    // Each dot is drawn as a play triangle when the source is streaming
    // live, and as a pause icon (two bars) when the iOS client has signaled
    // the video stream is paused. Disambiguates state when several devices
    // are connected at once.
    source_btns_.clear();
    if (get_sources_fn_) {
        auto sources = get_sources_fn_();
        if (sources.size() >= 1) {
            float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
            int bezel_bottom = frame_dst_h_ -
                (int)((phone_frame_.screen_y() + phone_frame_.screen_height()) * scale);
            int dot_r = std::max(3, frame_dst_w_ / 140);
            int spacing = dot_r * 7;
            int total_w = spacing * (int)(sources.size() - 1);
            int start_x = frame_dst_x_ + frame_dst_w_ / 2 - total_w / 2;
            int cy = frame_dst_y_ + frame_dst_h_ - bezel_bottom / 2;
            int hit_sz = std::max(20, dot_r * 6);

            int pmx, pmy;
            SDL_GetMouseState(&pmx, &pmy);

            // Helper: filled triangle pointing right, vertices
            //   (cx-w, cy-h), (cx-w, cy+h), (cx+w, cy)
            auto fill_play = [&](int cx_, int cy_, int w, int h) {
                if (w <= 0) return;
                for (int x = 0; x <= w * 2; ++x) {
                    // At x=0:        height = h; at x=2w:    height = 0
                    int hh = h - (h * x) / (w * 2);
                    SDL_RenderDrawLine(sdl_renderer_,
                                       cx_ - w + x, cy_ - hh,
                                       cx_ - w + x, cy_ + hh);
                }
            };

            for (size_t i = 0; i < sources.size(); ++i) {
                int cx = start_x + (int)i * spacing;
                BtnRect r{cx - hit_sz / 2, cy - hit_sz / 2, hit_sz, hit_sz};
                bool hover = in_rect(pmx, pmy, r.x, r.y, r.w, r.h);

                uint8_t a;
                if (sources[i].active)        a = 255;
                else if (hover)               a = 230;
                else if (sources[i].streaming) a = 180;
                else                          a = 110;
                SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, a);
                if (sources[i].active) {
                    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 255);
                }

                if (sources[i].paused) {
                    // Pause icon — two vertical bars
                    int bw = std::max(2, (dot_r * 2) / 3);
                    int bh = dot_r * 2 + 1;
                    int gap = std::max(2, dot_r);
                    SDL_Rect lb{cx - gap / 2 - bw, cy - bh / 2, bw, bh};
                    SDL_Rect rb{cx + gap / 2,      cy - bh / 2, bw, bh};
                    SDL_RenderFillRect(sdl_renderer_, &lb);
                    SDL_RenderFillRect(sdl_renderer_, &rb);
                } else {
                    // Play icon — right-pointing triangle
                    fill_play(cx, cy, dot_r + 1, dot_r + 1);
                }

                source_btns_.emplace_back(sources[i].id, r);
                if (hover) {
                    bezel_hover_key = "src:" + sources[i].id;
                    std::string state = sources[i].paused ? " (paused)"
                                       : sources[i].streaming ? " (live)"
                                       : "";
                    bezel_hover_text = sources[i].name + state +
                                       "\nLeft-click: switch  \u00B7  Right-click: menu";
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
            hover_key_.rfind("resize", 0) == 0 ||
            hover_key_.rfind("bezel_", 0) == 0) {
            hover_key_.clear();
        }
    }

    // Generic right-click popup menu for any bezel button.
    if (bezel_menu_visible_) {
        // Build the item list for this target.
        struct Item { std::string action, label; };
        std::vector<Item> items;
        if (bezel_menu_target_ == "menu") {
            items.push_back({"exit", "Exit application (Esc)"});
        } else if (bezel_menu_target_ == "log") {
            items.push_back({"copy", "Copy log to clipboard (Ctrl+C)"});
            items.push_back({"clear", "Clear log (Ctrl+X)"});
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
            // Switch off blending so the body rect + side strips + corner
            // discs do not alpha-compound into visible darker arcs at their
            // overlap.
            int pr = std::max(6, pad / 2);
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
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
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

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

void Renderer::draw_bezel_tooltip(const std::string& text, int anchor_x, int anchor_y,
                                  bool prefer_below) {
    if (text.empty()) return;

    // Split on the first '\n' into a primary line and an optional secondary
    // line (rendered smaller/dimmer below).
    std::string line1 = text;
    std::string line2;
    auto nl = text.find('\n');
    if (nl != std::string::npos) {
        line1 = text.substr(0, nl);
        line2 = text.substr(nl + 1);
    }

    // Cap font size in tablet mode so tooltip stays the same neat size.
    int eq_w = phone_frame_.is_tablet()
                   ? (int)(frame_dst_w_ * 0.62f)
                   : frame_dst_w_;
    int target_h = std::max(12, eq_w / 38);
    int target_h2 = std::max(10, (target_h * 4) / 5);

#ifdef _WIN32
    // Render at the actual target font height for crisp text (no scaling).
    if (text != bezel_tip_str_ || target_h != bezel_tip_font_h_) {
        if (bezel_tip_tex_)  { SDL_DestroyTexture(bezel_tip_tex_);  bezel_tip_tex_  = nullptr; }
        if (bezel_tip_tex2_) { SDL_DestroyTexture(bezel_tip_tex2_); bezel_tip_tex2_ = nullptr; }
        bezel_tip_tex_ = make_text_texture(sdl_renderer_, line1, target_h,
                                           255, 255, 255, &bezel_tip_w_, &bezel_tip_h_);
        if (!line2.empty()) {
            bezel_tip_tex2_ = make_text_texture(sdl_renderer_, line2, target_h2,
                                                170, 170, 170, &bezel_tip_w2_, &bezel_tip_h2_);
        } else {
            bezel_tip_w2_ = bezel_tip_h2_ = 0;
        }
        bezel_tip_str_ = text;
        bezel_tip_font_h_ = target_h;
    }
#endif
    if (!bezel_tip_tex_) return;

    int line_gap = bezel_tip_tex2_ ? std::max(2, target_h2 / 4) : 0;
    int disp_w = std::max(bezel_tip_w_, bezel_tip_w2_);
    int disp_h = bezel_tip_h_ + (bezel_tip_tex2_ ? line_gap + bezel_tip_h2_ : 0);
    int tp = std::max(3, bezel_tip_h_ / 3);
    int tw = disp_w + tp * 2;
    int th = disp_h + tp * 2;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    int tx = anchor_x - tw / 2;
    int ty;
    if (prefer_below) {
        ty = anchor_y + 12;
        if (ty + th > win_h - 4) ty = anchor_y - th - 6; // flip up if no room
    } else {
        ty = anchor_y - th - 6;
        if (ty < 4) ty = anchor_y + 12;
    }
    if (tx + tw > win_w - 4) tx = win_w - tw - 4;
    if (tx < 4) tx = 4;

    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 32, 230);
    SDL_Rect bg{tx, ty, tw, th};
    SDL_RenderFillRect(sdl_renderer_, &bg);
    SDL_SetRenderDrawColor(sdl_renderer_, 100, 100, 100, 220);
    SDL_RenderDrawRect(sdl_renderer_, &bg);
    SDL_Rect td{tx + (tw - bezel_tip_w_) / 2, ty + tp, bezel_tip_w_, bezel_tip_h_};
    SDL_RenderCopy(sdl_renderer_, bezel_tip_tex_, nullptr, &td);
    if (bezel_tip_tex2_) {
        SDL_Rect td2{tx + (tw - bezel_tip_w2_) / 2,
                     td.y + bezel_tip_h_ + line_gap,
                     bezel_tip_w2_, bezel_tip_h2_};
        SDL_RenderCopy(sdl_renderer_, bezel_tip_tex2_, nullptr, &td2);
    }
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

    // Pill background. We temporarily switch the renderer to BLENDMODE_NONE
    // so the rounded end caps overwrite the body rect at their overlap
    // instead of alpha-compounding into a visible darker semi-circle.
    int pill_r = row_h / 2;
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
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
    // Restore alpha blending for everything that follows (buttons, icons,
    // text) so they composite correctly over the pill.
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

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
    bool gear_hover = false;

    // Settings (gear) button — placed to the right of the icon, far away from
    // the screenshot button on the right cluster so they cannot be confused.
    // The gear is intentionally smaller and dimmer than the right-cluster
    // buttons so it reads as a secondary, low-emphasis control.
    int gear_sz = std::max(16, btn_sz * 4 / 5);
    int by_left = island_y + (row_h - gear_sz) / 2;
    int gear_x = icon_x + icon_sz + std::max(4, pad / 2);
    settings_btn_ = {gear_x, by_left, gear_sz, gear_sz};
    {
        bool gh = in_rect(mx, my, gear_x, by_left, gear_sz, gear_sz);
        // Background pill — slightly transparent so it recedes into the island.
        SDL_SetRenderDrawColor(sdl_renderer_, gh ? 60 : 42, gh ? 60 : 42, gh ? 65 : 46, gh ? 160 : 110);
        fill_circle(sdl_renderer_, gear_x + gear_sz / 2, by_left + gear_sz / 2, gear_sz / 2);
        // Cleaner gear icon: 8 trapezoidal teeth + ring + center hole.
        int gcx = gear_x + gear_sz / 2, gcy = by_left + gear_sz / 2;
        int r_outer = gear_sz * 9 / 20;   // tip of teeth
        int r_inner = gear_sz * 7 / 20;   // base of teeth (= ring outer edge)
        int r_ring  = gear_sz * 4 / 20;   // ring inner edge
        int r_hole  = gear_sz * 2 / 20;   // center hole
        const uint8_t bg_r = gh ? 60 : 42, bg_g = gh ? 60 : 42, bg_b = gh ? 65 : 46;
        // Filled body of the gear: outer disk minus radial wedges between teeth.
        for (int dy = -r_outer; dy <= r_outer; dy++) {
            for (int dx = -r_outer; dx <= r_outer; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 > r_outer * r_outer) continue;
                float dist = std::sqrt((float)d2);
                bool in_body;
                if (dist <= r_inner) {
                    in_body = true;
                } else {
                    // Outside the ring → only render where a tooth lives.
                    // 8 teeth, each spans ~22 of the 45 degrees per slot.
                    float ang = std::atan2((float)dy, (float)dx); // -PI..PI
                    float slot = 6.28318f / 8.0f;                  // 45deg
                    float local = std::fmod(ang + 6.28318f, slot); // 0..slot
                    float half_tooth = slot * 0.28f;               // ~12.6deg each side of slot center
                    float center_off = std::fabs(local - slot * 0.5f);
                    in_body = (center_off <= half_tooth);
                }
                if (in_body) {
                    // Dimmer fill so the gear reads as a secondary control.
                    SDL_SetRenderDrawColor(sdl_renderer_, 200, 200, 205, gh ? 220 : 150);
                    SDL_RenderDrawPoint(sdl_renderer_, gcx + dx, gcy + dy);
                }
            }
        }
        // Punch out the ring interior (between r_ring and r_hole stays solid white,
        // inside r_hole is hollow — but we want a HOLE in the middle: so erase
        // the inner ring area back to background, then erase the hole.
        for (int dy = -r_ring; dy <= r_ring; dy++) {
            for (int dx = -r_ring; dx <= r_ring; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= r_ring * r_ring) {
                    SDL_SetRenderDrawColor(sdl_renderer_, bg_r, bg_g, bg_b, gh ? 200 : 160);
                    SDL_RenderDrawPoint(sdl_renderer_, gcx + dx, gcy + dy);
                }
            }
        }
        gear_hover = gh;
    }

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

    // Settings (gear) button is now drawn next to the icon on the LEFT side
    // (see block above). The right-cluster ends here with the screenshot button.

    // Hover detection (0=close, 1=screenshot, 2=folder, 3=icon)
    int new_hover = -1;
    if (close_hover) new_hover = 0;
    else if (ss_hover) new_hover = 1;
    else if (folder_hover) new_hover = 2;
    else if (icon_hover) new_hover = 3;
    else if (gear_hover) new_hover = 6;

    // Tooltip — use the same draw_bezel_tooltip() as the bezel dots so the
    // styling, font size, edge-clamping and 1 s hover delay are identical
    // everywhere. Anchor below the mouse cursor so it never covers the
    // button being explained.
    if (new_hover >= 0 &&
        tooltip_ready("island:" + std::to_string(new_hover))) {
        const char* tip = nullptr;
        switch (new_hover) {
            case 0: tip = "Close (Esc)";              break;
            case 1: tip = "Screenshot (Ctrl+S)";      break;
            case 2: tip = "Open Screenshots Folder";  break;
            case 3: tip = "About 1PhoneMirror (I)"; break;
            case 6: tip = "Settings (S)"; break;
        }
        if (tip) {
            // Anchor at the cursor, force tooltip BELOW so it never
            // covers the button being explained.
            draw_bezel_tooltip(tip, mx, my, /*prefer_below=*/true);
        }
    } else if (new_hover < 0 &&
               hover_key_.rfind("island:", 0) == 0) {
        // Reset the 1 s timer when the cursor leaves all island buttons.
        hover_key_.clear();
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

    auto line_w = [&](const std::vector<FooterSeg>& line, float scale) {
        int total = 0;
        for (auto& s : line) total += (int)(s.w * scale);
        return total;
    };

    // Line 2 (copyright) is rendered slightly smaller than line 1.
    float sf2 = sf * 0.85f;

    int total_w1 = line_w(footer_line1_, sf);
    int total_w2 = line_w(footer_line2_, sf2);
    int line_h  = footer_line1_.empty() ? 0 : (int)(footer_line1_[0].h * sf);
    int line_h2 = footer_line2_.empty() ? 0 : (int)(footer_line2_[0].h * sf2);
    int gap = std::max(2, line_h / 4);

    // Position near bottom of screen area
    int footer_y = svy + svh - line_h - line_h2 - gap - std::max(6, svh / 25);

    // Footer background band — distinct from waiting screen.
    {
        int pad_top = std::max(4, line_h / 3);
        int band_y = footer_y - pad_top;
        int band_h = (svy + svh) - band_y;
        SDL_SetRenderDrawColor(sdl_renderer_, 16, 18, 24, 255);
        SDL_Rect band = {svx, band_y, svw, band_h};
        SDL_RenderFillRect(sdl_renderer_, &band);
    }

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
        int sw = (int)(s.w * sf2), sh = (int)(s.h * sf2);
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
        // Use the unified bezel tooltip styling so footer hovers match
        // the rest of the UI. Anchor at the segment's top-center.
        draw_bezel_tooltip(hovered, hx + hw / 2, hy);
    } else if (hovered.empty() &&
               hover_key_.rfind("footer:", 0) == 0) {
        hover_key_.clear();
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

    // Scale text to fit panel, never below a readable minimum.
    float max_text_w = svw * 0.7f;
    const float min_text_scale = 0.40f;
    float text_scale = 0.5f;
    for (auto& line : info_lines_) {
        if (line.tex && line.w * text_scale > max_text_w)
            text_scale = max_text_w / line.w;
    }
    if (text_scale < min_text_scale) text_scale = min_text_scale;

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

    // Rounded rect background. BLENDMODE_NONE so the body + strips +
    // corner discs cannot alpha-compound at their overlap.
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
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
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

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

    // Scale text to fit panel, but never shrink below a readable minimum.
    // If a line is still too wide at the minimum scale it will overflow the
    // clip rect (acceptable) — the scrollbar handles vertical overflow.
    float max_text_w = svw * 0.7f;
    const float min_text_scale = 0.40f;
    float text_scale = 0.5f;
    for (auto& line : version_lines_) {
        if (line.tex && line.w * text_scale > max_text_w)
            text_scale = max_text_w / line.w;
    }
    if (text_scale < min_text_scale) text_scale = min_text_scale;

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

    // Rounded rect background. BLENDMODE_NONE so the body + strips +
    // corner discs cannot alpha-compound at their overlap.
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
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
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

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

// ----------------------------------------------------------------------------
// Settings panel — bezel colour swatches + screenshot toggles
// ----------------------------------------------------------------------------

void Renderer::drawer_color(uint8_t& r, uint8_t& g, uint8_t& b) const {
    // Derive the drawer / sub-panel colour from the bezel: a darker shade
    // of the same hue, clamped so log text remains legible.
    int br = phone_frame_.bezel_r();
    int bg = phone_frame_.bezel_g();
    int bb = phone_frame_.bezel_b();
    int dr = br - 8, dg = bg - 8, db = bb - 8;
    if (dr < 18) dr = 18; if (dr > 40) dr = 40;
    if (dg < 18) dg = 18; if (dg > 40) dg = 40;
    if (db < 22) db = 22; if (db > 45) db = 45;
    r = (uint8_t)dr; g = (uint8_t)dg; b = (uint8_t)db;
}

void Renderer::apply_bezel_color(uint8_t r, uint8_t g, uint8_t b) {
    settings_.bezel_r = r;
    settings_.bezel_g = g;
    settings_.bezel_b = b;
    settings_.save();
    // PhoneFrame::set_bezel_color invalidates its own texture/pixel caches.
    // We must immediately re-generate it for the current screen size, otherwise
    // is_generated() stays false until the next video frame arrives — which
    // would leave the waiting screen blank for a user who is not yet connected.
    phone_frame_.set_bezel_color(r, g, b);
    int gw = (tex_width_  > 0) ? tex_width_  : 390;
    int gh = (tex_height_ > 0) ? tex_height_ : 844;
    phone_frame_.generate(sdl_renderer_, gw, gh);
}

void Renderer::draw_settings_panel() {
    if (frame_dst_w_ == 0) return;

    // Update animation
    const float anim_duration = 200.0f;
    if (settings_panel_animating_) {
        float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - settings_panel_anim_start_).count();
        float t = std::min(1.0f, elapsed / anim_duration);
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        settings_panel_anim_ = settings_panel_visible_ ? eased : (1.0f - eased);
        if (t >= 1.0f) settings_panel_animating_ = false;
    } else {
        settings_panel_anim_ = settings_panel_visible_ ? 1.0f : 0.0f;
    }
    if (settings_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);

    int pad      = std::max(10, svw / 18);
    int row_gap  = std::max(6,  pad / 2);
    int title_h  = std::max(14, svw / 22);
    int label_h  = std::max(11, svw / 30);
    int swatch   = std::max(22, svw / 14);
    int sw_gap   = std::max(6,  swatch / 4);

    int panel_w = (int)(svw * 0.86f);
    int panel_x = svx + (svw - panel_w) / 2;

    // Bottom of island row
    int btn_sz = std::max(20, svw / 14);
    int ipad = btn_sz / 3;
    int island_bottom = svy + ipad + btn_sz + ipad * 2;
    int panel_y_target = island_bottom + pad / 2;

    // Two rows of 3 swatches, then 2 toggle rows
    const int N_PRESETS = 6;
    static const uint8_t presets[N_PRESETS][3] = {
        { 28,  28,  30},  // Dark titanium (default)
        { 90,  90,  95},  // Graphite
        { 36,  46,  72},  // Midnight blue
        { 36,  60,  44},  // Forest green
        { 80,  34,  34},  // Deep red
        {110,  84,  46},  // Bronze
    };

    int swatches_per_row = 3;
    int swatch_rows = (N_PRESETS + swatches_per_row - 1) / swatches_per_row;
    int total_h = pad + title_h + row_gap
                + swatch_rows * (swatch + row_gap)
                + row_gap + label_h + row_gap            // toggle 1
                + label_h + row_gap                       // toggle 2
                + label_h + row_gap                       // toggle 3 (computer name)
                + pad;

    // Slide animation
    int slide_offset = (int)((1.0f - settings_panel_anim_) * total_h * 0.3f);
    int panel_y = panel_y_target - slide_offset;
    uint8_t alpha      = (uint8_t)(240 * settings_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * settings_panel_anim_);

    settings_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    // Rounded rect background (same recipe as info panel). BLENDMODE_NONE
    // so the body + strips + corner discs cannot alpha-compound at their
    // overlap.
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
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
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    auto draw_label = [&](const std::string& s, int font_h, int cx, int cy_top,
                          uint8_t cr, uint8_t cg, uint8_t cb, bool centered) {
        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, s, font_h, cr, cg, cb, &tw, &th);
        if (!tex) return;
        int x = centered ? (cx - tw / 2) : cx;
        SDL_Rect dst = {x, cy_top, tw, th};
        SDL_SetTextureAlphaMod(tex, text_alpha);
        SDL_RenderCopy(sdl_renderer_, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    };

    int cy = panel_y + pad;
    draw_label("Settings", title_h, panel_x + panel_w / 2, cy, 230, 230, 235, true);
    cy += title_h + row_gap;

    // Swatches grid
    settings_swatch_btns_.clear();
    int grid_w = swatches_per_row * swatch + (swatches_per_row - 1) * sw_gap;
    int grid_x = panel_x + (panel_w - grid_w) / 2;
    for (int i = 0; i < N_PRESETS; i++) {
        int row = i / swatches_per_row;
        int col = i % swatches_per_row;
        int sx = grid_x + col * (swatch + sw_gap);
        int sy = cy + row * (swatch + row_gap);
        // Filled swatch
        SDL_SetRenderDrawColor(sdl_renderer_, presets[i][0], presets[i][1], presets[i][2], alpha);
        SDL_Rect sr = {sx, sy, swatch, swatch};
        SDL_RenderFillRect(sdl_renderer_, &sr);
        // Selected ring if currently active
        bool selected = (presets[i][0] == settings_.bezel_r &&
                          presets[i][1] == settings_.bezel_g &&
                          presets[i][2] == settings_.bezel_b);
        if (selected) {
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, text_alpha);
            for (int t = 0; t < 2; t++) {
                SDL_Rect r2 = {sx - 2 - t, sy - 2 - t, swatch + 4 + 2 * t, swatch + 4 + 2 * t};
                SDL_RenderDrawRect(sdl_renderer_, &r2);
            }
        } else {
            SDL_SetRenderDrawColor(sdl_renderer_, 90, 90, 95, text_alpha);
            SDL_RenderDrawRect(sdl_renderer_, &sr);
        }
        settings_swatch_btns_.push_back({i, BtnRect{sx, sy, swatch, swatch}});
    }
    cy += swatch_rows * (swatch + row_gap) + row_gap;

    // Toggle helper
    auto draw_toggle = [&](const std::string& label, bool on, int row_y) -> BtnRect {
        int box = std::max(12, label_h + 2);
        int row_x = panel_x + pad;
        // Checkbox box
        SDL_SetRenderDrawColor(sdl_renderer_, 50, 50, 55, alpha);
        SDL_Rect br = {row_x, row_y, box, box};
        SDL_RenderFillRect(sdl_renderer_, &br);
        SDL_SetRenderDrawColor(sdl_renderer_, 120, 120, 130, text_alpha);
        SDL_RenderDrawRect(sdl_renderer_, &br);
        if (on) {
            // Check mark (two simple lines)
            SDL_SetRenderDrawColor(sdl_renderer_, 90, 200, 130, text_alpha);
            int cx0 = row_x + box / 5;
            int cy0 = row_y + box / 2;
            int cx1 = row_x + box * 2 / 5;
            int cy1 = row_y + box * 4 / 5;
            int cx2 = row_x + box * 4 / 5;
            int cy2 = row_y + box / 5;
            for (int t = -1; t <= 1; t++) {
                SDL_RenderDrawLine(sdl_renderer_, cx0, cy0 + t, cx1, cy1 + t);
                SDL_RenderDrawLine(sdl_renderer_, cx1, cy1 + t, cx2, cy2 + t);
            }
        }
        // Label to the right of the box
        int label_x = row_x + box + std::max(6, box / 3);
        draw_label(label, label_h, label_x,
                   row_y + (box - label_h) / 2 - 1,
                   220, 220, 225, false);
        // Hit rect spans the full row (box + label) for easier clicking.
        return BtnRect{row_x, row_y, panel_w - pad * 2, box};
    };

    settings_toggle_save_btn_ = draw_toggle(
        "Save screenshots to Pictures folder",
        settings_.screenshot_save_to_folder, cy);
    cy += label_h + row_gap;
    settings_toggle_clip_btn_ = draw_toggle(
        "Copy screenshots to clipboard",
        settings_.screenshot_copy_to_clipboard, cy);
    cy += label_h + row_gap;
    settings_toggle_compname_btn_ = draw_toggle(
        "Identify as computer name (restart required)",
        settings_.use_computer_name, cy);
}

void Renderer::clear_log_row_cache() {
    for (auto& r : log_row_cache_) {
        if (r.tex) SDL_DestroyTexture(r.tex);
    }
    log_row_cache_.clear();
}

void Renderer::draw_log_panel() {
#ifdef _WIN32
    if (frame_dst_w_ == 0 || log_panel_anim_ < 0.01f) return;
    int lp_w = (int)(log_panel_full_w_ * log_panel_anim_);
    int lp_margin = std::max(4, frame_dst_h_ / 40);
    int drawer_inset = frame_dst_h_ / 16; // slightly shorter than phone
    int panel_x = frame_dst_x_ + frame_dst_w_; // flush against phone frame
    int panel_y = frame_dst_y_ + drawer_inset;
    int panel_w = lp_w - lp_margin;
    int panel_h = frame_dst_h_ - drawer_inset * 2;

    if (panel_w < 40) return;

    // Background — flat left edge (flush with phone), rounded right edge (drawer)
    int pr = std::max(8, panel_w / 30);
    // Right corner centers — placed so that the arc tangent points sit
    // EXACTLY on the panel edges (cx + pr = panel_x + panel_w - 1, etc.).
    int cx_r  = panel_x + panel_w - 1 - pr;
    int cy_t  = panel_y + pr;
    int cy_b  = panel_y + panel_h - 1 - pr;

    uint8_t dr, dg, db; drawer_color(dr, dg, db);
    // Compute the bezel-edge ("highlight") colour up front so we can both
    // stroke the border and use it as the disc fill OUTSIDE the body so
    // there is no alpha-blend mismatch creating a visible circle ghost.
    int bw = std::max(2, std::min(3, panel_h / 200 + 2));
    auto lite = [](uint8_t c, int d) -> uint8_t {
        int v = (int)c + d; if (v < 0) v = 0; if (v > 255) v = 255; return (uint8_t)v;
    };
    uint8_t br_ = phone_frame_.bezel_r();
    uint8_t bg_ = phone_frame_.bezel_g();
    uint8_t bb_ = phone_frame_.bezel_b();
    uint8_t er = lite(br_, 30), eg = lite(bg_, 30), eb = lite(bb_, 32);

    // ---- Drawer interior + corner border --------------------------------
    // Disable alpha blending for the silhouette so overlapping fills do not
    // alpha-compound into a visible darker arc.
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);

    // 1. Paint the bulk of the body (everything left of the right-side
    //    rounded corners) in DRAWER colour, then add the top/bottom/right
    //    border strips on top.
    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
    SDL_Rect drawer_body = {panel_x, panel_y + bw,
                            cx_r - panel_x, panel_h - bw * 2};
    SDL_RenderFillRect(sdl_renderer_, &drawer_body);
    // Right inner strip between the two corner centers.
    SDL_Rect drawer_strip = {cx_r, cy_t,
                             pr - bw, cy_b - cy_t + 1};
    SDL_RenderFillRect(sdl_renderer_, &drawer_strip);

    // 2. Border strips (top / bottom / right). Edges stop at the corner
    //    center column so the corner rasterizer below owns those pixels
    //    exclusively.
    SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb, 255);
    SDL_Rect e_top = {panel_x, panel_y, cx_r - panel_x, bw};
    SDL_RenderFillRect(sdl_renderer_, &e_top);
    SDL_Rect e_bot = {panel_x, panel_y + panel_h - bw, cx_r - panel_x, bw};
    SDL_RenderFillRect(sdl_renderer_, &e_bot);
    SDL_Rect e_rgt = {panel_x + panel_w - bw, cy_t, bw, cy_b - cy_t + 1};
    SDL_RenderFillRect(sdl_renderer_, &e_rgt);

    // 3. Two right-side corners drawn as a single per-pixel pass per corner.
    //    For each pixel in the corner bounding box we test float distance
    //    from the corner center and pick:
    //       distance <  in_pr      -> drawer colour
    //       in_pr <= d <= out_pr   -> border colour
    //       distance >  out_pr     -> skip (transparent)
    //    This guarantees the border band has uniform thickness `bw` along
    //    the entire arc, with no integer-rounding stair-step ghosts.
    auto paint_corner = [&](int cx, int cy, int quad) {
        float r_out = (float)pr + 0.5f;
        float r_in  = (float)(pr - bw) + 0.5f;
        for (int dy = -pr; dy <= pr; dy++) {
            for (int dx = -pr; dx <= pr; dx++) {
                if (quad == 0 && !(dx >= 0 && dy <= 0)) continue;
                if (quad == 1 && !(dx >= 0 && dy >= 0)) continue;
                float d = std::sqrt((float)(dx * dx + dy * dy));
                if (d > r_out) continue;
                if (d >= r_in) {
                    SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb, 255);
                } else {
                    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
                }
                SDL_RenderDrawPoint(sdl_renderer_, cx + dx, cy + dy);
            }
        }
    };
    paint_corner(cx_r, cy_t, 0);
    paint_corner(cx_r, cy_b, 1);

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // Get log lines
    auto lines = openmirror::LogBuffer::instance().get_lines();
    if (lines.empty()) return;

    // Layout is computed against the FULL-OPEN panel width, not the current
    // animated width. Otherwise word-wrap + text rasterization run on every
    // frame of the slide animation and produce visible hitches.
    int full_panel_w = log_panel_full_w_ - lp_margin;
    if (full_panel_w < 40) full_panel_w = panel_w;
    int font_sz = std::max(9, panel_h / 85);
    int line_h = font_sz + 2;
    int pad = std::max(6, pr);
    int max_text_w = full_panel_w - pad * 2;
    int visible_h = panel_h - pad * 2;

    // Rebuild the wrapped+rasterized row cache only when something the
    // layout depends on actually changes.
    uint64_t cur_ver = openmirror::LogBuffer::instance().version();
    bool need_rebuild = (cur_ver != log_cache_version_) ||
                        (font_sz != log_cache_font_sz_) ||
                        (full_panel_w != log_cache_full_w_);
    if (need_rebuild) {
        clear_log_row_cache();
        log_cache_version_ = cur_ver;
        log_cache_font_sz_ = font_sz;
        log_cache_full_w_ = full_panel_w;

        auto color_for = [](const std::string& s, uint8_t& r, uint8_t& g, uint8_t& b) {
            r = 160; g = 160; b = 160;
            if      (s.find("[Cast]")     != std::string::npos) { r = 120; g = 200; b = 120; }
            else if (s.find("[AirPlay]")  != std::string::npos) { r = 120; g = 160; b = 255; }
            else if (s.find("[Renderer]") != std::string::npos) { r = 200; g = 180; b = 120; }
            if (s.find("Warning") != std::string::npos ||
                s.find("Failed")  != std::string::npos ||
                s.find("Error")   != std::string::npos) { r = 255; g = 120; b = 120; }
        };

        const std::string cont_indent = "  ";
        int wrap_budget = std::max(8, max_text_w - font_sz);
        log_row_cache_.reserve(lines.size() + 16);
        for (auto& src : lines) {
            uint8_t lr, lg, lb; color_for(src, lr, lg, lb);
            auto push_row = [&](std::string s) {
                LogRowCache row;
                row.text = std::move(s);
                row.r = lr; row.g = lg; row.b = lb;
                row.tex = make_text_texture(sdl_renderer_, row.text, font_sz,
                                             row.r, row.g, row.b, &row.w, &row.h);
                log_row_cache_.push_back(std::move(row));
            };
            if (measure_text_width_a(src, font_sz) <= wrap_budget) {
                push_row(src);
                continue;
            }
            std::string remaining = src;
            bool first_row = true;
            while (!remaining.empty()) {
                std::string prefix = first_row ? std::string{} : cont_indent;
                int lo = 1, hi = (int)remaining.size(), best = 1;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    int w = measure_text_width_a(prefix + remaining.substr(0, mid), font_sz);
                    if (w <= wrap_budget) { best = mid; lo = mid + 1; }
                    else hi = mid - 1;
                }
                int brk = best;
                if (best < (int)remaining.size()) {
                    int sp = (int)remaining.rfind(' ', best);
                    if (sp > 0 && sp >= best / 2) brk = sp;
                }
                push_row(prefix + remaining.substr(0, brk));
                remaining.erase(0, brk);
                if (!remaining.empty() && remaining.front() == ' ') remaining.erase(0, 1);
                first_row = false;
            }
        }
        log_cache_total_h_ = (int)log_row_cache_.size() * line_h;
    }

    int content_h = log_cache_total_h_;

    // Clamp scroll
    int max_scroll = std::max(0, content_h - visible_h);
    if (log_scroll_offset_ < 0) log_scroll_offset_ = 0;
    if (log_scroll_offset_ > max_scroll) log_scroll_offset_ = max_scroll;

    // Auto-scroll to bottom on new lines (unless user is dragging the thumb).
    if (cur_ver != log_last_version_) {
        log_last_version_ = cur_ver;
        if (!log_scrollbar_dragging_) {
            log_scroll_offset_ = max_scroll;
        }
    }

    // Clip to the currently-visible portion of the drawer (animated width).
    SDL_Rect clip = {panel_x + pad, panel_y + pad, panel_w - pad * 2, visible_h};
    SDL_RenderSetClipRect(sdl_renderer_, &clip);

    int first_visible = log_scroll_offset_ / line_h;
    int last_visible = (log_scroll_offset_ + visible_h) / line_h + 1;
    first_visible = std::max(0, first_visible);
    last_visible = std::min((int)log_row_cache_.size(), last_visible);

    int text_x = panel_x + pad;

    for (int i = first_visible; i < last_visible; i++) {
        int y = panel_y + pad + i * line_h - log_scroll_offset_;
        const auto& row = log_row_cache_[i];
        if (row.tex) {
            SDL_Rect dst = {text_x, y, row.w, row.h};
            SDL_RenderCopy(sdl_renderer_, row.tex, nullptr, &dst);
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

// ----------------------------------------------------------------------------
// Android connect panel — themed to match info_panel
// ----------------------------------------------------------------------------

#ifdef _WIN32
// Discover this machine's primary IPv4 by "connecting" a UDP socket to a
// public address — the OS picks the outbound interface without sending
// any packets. Returns "192.168.0." (first 3 octets + dot) or "".
static std::string local_subnet_prefix() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = inet_addr("8.8.8.8");
    std::string out;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in name{};
        int len = sizeof(name);
        if (getsockname(s, (sockaddr*)&name, &len) == 0) {
            unsigned long a = ntohl(name.sin_addr.s_addr);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.",
                          (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF);
            out = buf;
        }
    }
    closesocket(s);
    return out;
}
#endif

void Renderer::show_android_panel() {
    android_panel_visible_ = true;
    android_panel_animating_ = true;
    android_panel_anim_start_ = std::chrono::steady_clock::now();
    android_help_visible_ = false;
    // Pair port and PIN are single-use; the phone shows fresh values every
    // time the Wireless debugging → Pair dialog is opened. Always clear
    // them so the user can't paste in stale values from the last session.
    // IP and Connect port survive sleep/wake on the phone, so we keep
    // them between panel opens within a single app session.
    android_port_.clear();
    android_pin_.clear();
#ifdef _WIN32
    if (android_ip_.empty()) {
        std::string pfx = local_subnet_prefix();
        if (!pfx.empty()) android_ip_ = pfx;
    }
#endif
    // Smart focus: jump to the field that needs typing next.
    //   - IP not set yet              -> focus IP (0)
    //   - IP set, no connect port yet -> focus Connect port (1)
    //   - both already filled         -> focus Pair port (2), the value the
    //                                    user must always re-type each session
    auto looks_complete_ip = [](const std::string& s) {
        if (s.empty() || s.back() == '.') return false;
        int dots = 0;
        for (char c : s) if (c == '.') ++dots;
        return dots == 3;
    };
    if (!looks_complete_ip(android_ip_))      android_focus_ = 0;
    else if (android_connect_port_.empty())   android_focus_ = 1;
    else                                       android_focus_ = 2;
    {
        std::lock_guard lk(android_status_mutex_);
        if (android_status_.empty())
            android_status_ = "* required. Press ? for setup help.";
    }
    SDL_StartTextInput();
}

namespace {
bool valid_ipv4(const std::string& s) {
    int parts = 0, n = 0, digits = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : '.';
        if (c == '.') {
            if (digits == 0 || n > 255) return false;
            ++parts; n = 0; digits = 0;
            if (parts > 4) return false;
        } else if (c >= '0' && c <= '9') {
            n = n * 10 + (c - '0'); ++digits;
            if (digits > 3) return false;
        } else {
            return false;
        }
    }
    return parts == 4;
}
}

void Renderer::android_submit() {
    auto fail = [&](const std::string& m) {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = m;
        android_status_is_error_ = true;
    };
    if (!valid_ipv4(android_ip_))      return fail("Invalid IP address.");
    if (android_port_.empty())          return fail("Pair port required.");
    if (android_pin_.size() != 6)       return fail("PIN must be 6 digits.");
    if (!android_connect_fn_)           return fail("Connect handler not wired.");

    {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = "Working...";
        android_status_is_error_ = false;
        android_busy_ = true;
    }
    std::string ip   = android_ip_;
    std::string port = android_port_;
    std::string pin  = android_pin_;
    std::string cport = android_connect_port_;
    auto fn = android_connect_fn_;
    std::thread([this, ip, port, pin, cport, fn]() {
        std::string r = fn(ip, port, pin, cport);
        // Treat anything that doesn't start with the success prefix
        // ("Mirroring <serial>") as an error. Keyword matching missed
        // cases like "Paired but device did not appear via mDNS..." and
        // auto-closed the panel before the user could read the message.
        bool err = (r.rfind("Mirroring ", 0) != 0);
        // Mirror the result into the log so the user can review long
        // failure messages there too (the panel wraps but is dismissed on
        // success, and the toast disappears quickly).
        if (err) std::cerr << "[Android] " << r << "\n";
        else     std::cout << "[Android] " << r << "\n";
        std::lock_guard lk(android_status_mutex_);
        android_status_ = r;
        android_status_is_error_ = err;
        android_busy_ = false;
        // On success, auto-close the pair panel so the projected phone
        // becomes visible immediately.
        if (!err) {
            android_panel_visible_ = false;
            android_panel_animating_ = true;
            android_panel_anim_start_ = std::chrono::steady_clock::now();
        }
    }).detach();
}

void Renderer::draw_android_panel() {
    if (frame_dst_w_ == 0) return;

    // Animation
    const float dur = 200.0f;
    if (android_panel_animating_) {
        float el = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - android_panel_anim_start_).count();
        float t = std::min(1.0f, el / dur);
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        android_panel_anim_ = android_panel_visible_ ? e : 1.0f - e;
        if (t >= 1.0f) android_panel_animating_ = false;
    } else {
        android_panel_anim_ = android_panel_visible_ ? 1.0f : 0.0f;
    }
    if (android_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    int panel_w = (int)(svw * 0.85f);
    int pad     = std::max(10, panel_w / 22);
    int label_h = std::max(14, panel_w / 26);
    int field_h = std::max(28, panel_w / 12);
    int btn_h   = std::max(28, panel_w / 11);
    int gap     = std::max(6, pad / 2);

    int title_h = std::max(20, panel_w / 16);

    // Pre-wrap the status text so total_h grows with longer error messages
    // (otherwise multi-line failures get clipped or the buttons drift off
    // the bottom of the rounded panel).
    std::vector<std::string> status_lines;
    bool status_err = false;
    {
        std::string s;
        {
            std::lock_guard lk(android_status_mutex_);
            s = android_status_;
            status_err = android_status_is_error_;
        }
        if (!s.empty()) {
            int max_w = panel_w - pad * 2;
            uint8_t cr = status_err ? 230 : 140;
            uint8_t cg = status_err ? 110 : 200;
            uint8_t cb = status_err ? 110 : 140;
            std::vector<std::string> words;
            {
                std::string cur;
                for (char c : s) {
                    if (c == ' ' || c == '\n') {
                        if (!cur.empty()) { words.push_back(cur); cur.clear(); }
                    } else cur.push_back(c);
                }
                if (!cur.empty()) words.push_back(cur);
            }
            std::string line;
            for (auto& w : words) {
                std::string trial = line.empty() ? w : line + " " + w;
                int tw = 0, th = 0;
                SDL_Texture* probe = make_text_texture(sdl_renderer_, trial,
                                                       label_h, cr, cg, cb,
                                                       &tw, &th);
                if (probe) SDL_DestroyTexture(probe);
                if (tw <= max_w || line.empty()) {
                    line = trial;
                } else {
                    status_lines.push_back(line);
                    line = w;
                }
            }
            if (!line.empty()) status_lines.push_back(line);
        }
    }
    int status_rows = std::max<int>(1, (int)status_lines.size());

    // Extra breathing room below the buttons, on top of the normal `pad`,
    // so the Connect/Disconnect row visibly sits above the panel's bottom
    // edge instead of touching it.
    int btn_bottom_pad = std::max(10, pad);
    int total_h = pad + title_h + gap*2
                + (label_h + 6 + field_h + gap) * 2   // 2 rows of 2 fields
                + label_h * status_rows + gap            // status line(s)
                + btn_h + pad + btn_bottom_pad;

    int panel_x = svx + (svw - panel_w) / 2;
    // Center vertically on the phone screen, then clamp so we keep a small
    // breathing margin below the buttons inside the frame.
    int bottom_margin = std::max(8, svh / 28);
    int panel_y = svy + (svh - total_h) / 2 + svh / 18;
    if (panel_y + total_h > svy + svh - bottom_margin)
        panel_y = svy + svh - total_h - bottom_margin;
    if (panel_y < svy + 4) panel_y = svy + 4;
    int slide_off = (int)((1.0f - android_panel_anim_) * 30);
    panel_y -= slide_off;
    uint8_t alpha = (uint8_t)(245 * android_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * android_panel_anim_);

    android_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    // Rounded background — same recipe as info panel
    int pr = std::max(8, pad / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr*2, total_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, total_h - pr*2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, total_h - pr*2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + total_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + total_h - pr, pr);

    int cy = panel_y + pad;

    // Title
    {
        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, "Add Android phone",
            title_h, 235, 235, 240, &tw, &th);
        if (tex) {
            int dx = panel_x + (panel_w - tw) / 2;
            SDL_Rect d = {dx, cy, tw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
        }
        cy += title_h + gap*2;
    }

    // Close button (small × top-right)
    {
        int sz = title_h;
        android_close_btn_ = {panel_x + panel_w - sz - pad/2, panel_y + pad/2, sz, sz};
        int tw=0, th=0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, "x", sz, 200, 200, 200, &tw, &th);
        if (tex) {
            SDL_Rect d = {android_close_btn_.x + (sz-tw)/2,
                          android_close_btn_.y + (sz-th)/2, tw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
        }
    }

    // Help button (small ? to the left of the close button)
    {
        int sz = title_h;
        android_help_btn_ = {android_close_btn_.x - sz - pad/4,
                             panel_y + pad/2, sz, sz};
        int tw=0, th=0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, "?", sz,
                                             200, 200, 200, &tw, &th);
        if (tex) {
            SDL_Rect d = {android_help_btn_.x + (sz-tw)/2,
                          android_help_btn_.y + (sz-th)/2, tw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
        }
    }

    // Helper for one labeled field. fx/fw lets the caller place the field
    // at a custom horizontal slot, so we can pack two fields side by side.
    auto draw_field = [&](int idx, const char* label, const std::string& val,
                          const char* placeholder, int fx, int fw,
                          bool advance_cy) {
        // If the label ends with " *", render the base text in the normal
        // muted colour and the trailing "*" in the same green used by the
        // status hint, so users can see at a glance which fields are
        // required.
        std::string lab_str(label);
        bool has_star = lab_str.size() >= 2 &&
                        lab_str.compare(lab_str.size() - 2, 2, " *") == 0;
        std::string base_str = has_star ? lab_str.substr(0, lab_str.size() - 1)
                                        : lab_str;  // keep trailing space
        int tw=0, th=0;
        SDL_Texture* lab = make_text_texture(sdl_renderer_, base_str.c_str(),
            label_h, 170, 170, 180, &tw, &th);
        if (lab) {
            // Clip the label to the field width so a long label can't bleed
            // into the neighbouring column.
            SDL_Rect d = {fx, cy, std::min(tw, fw), th};
            SDL_SetTextureAlphaMod(lab, text_alpha);
            SDL_RenderCopy(sdl_renderer_, lab, nullptr, &d);
            SDL_DestroyTexture(lab);
        }
        if (has_star) {
            int sw = 0, sh = 0;
            SDL_Texture* st = make_text_texture(sdl_renderer_, "*", label_h,
                140, 200, 140, &sw, &sh);
            if (st) {
                int sx = fx + std::min(tw, fw);
                if (sx + sw > fx + fw) sx = fx + fw - sw;
                SDL_Rect d = {sx, cy, sw, sh};
                SDL_SetTextureAlphaMod(st, text_alpha);
                SDL_RenderCopy(sdl_renderer_, st, nullptr, &d);
                SDL_DestroyTexture(st);
            }
        }
        int label_actual_h = (th > 0 ? th : label_h);
        int field_y = cy + label_actual_h + 6;

        SDL_Rect fr = {fx, field_y, fw, field_h};
        android_field_rects_[idx] = {fr.x, fr.y, fr.w, fr.h};

        bool focused = (android_focus_ == idx);
        SDL_SetRenderDrawColor(sdl_renderer_, focused ? 55 : 45,
                                              focused ? 55 : 45,
                                              focused ? 65 : 50, alpha);
        SDL_RenderFillRect(sdl_renderer_, &fr);
        SDL_SetRenderDrawColor(sdl_renderer_,
            focused ? 110 : 70, focused ? 150 : 70, focused ? 220 : 80, alpha);
        SDL_RenderDrawRect(sdl_renderer_, &fr);

        std::string display = val.empty() ? std::string(placeholder) : val;
        bool is_placeholder = val.empty();
        int vt_w=0, vt_h=0;
        int font_h = field_h - 10;
        SDL_Texture* vt = make_text_texture(sdl_renderer_, display, font_h,
            is_placeholder ? 110 : 235,
            is_placeholder ? 110 : 235,
            is_placeholder ? 120 : 240, &vt_w, &vt_h);
        if (vt) {
            SDL_Rect d = {fr.x + 8, fr.y + (fr.h - vt_h)/2, vt_w, vt_h};
            SDL_SetTextureAlphaMod(vt, text_alpha);
            SDL_RenderCopy(sdl_renderer_, vt, nullptr, &d);
            SDL_DestroyTexture(vt);
        }
        if (focused) {
            using namespace std::chrono;
            auto now = steady_clock::now();
            auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
            if ((ms / 500) % 2 == 0) {
                int cx = fr.x + 8 + (is_placeholder ? 0 : vt_w) + 1;
                SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 230, text_alpha);
                SDL_Rect cur = {cx, fr.y + 6, 2, fr.h - 12};
                SDL_RenderFillRect(sdl_renderer_, &cur);
            }
        }
        if (advance_cy) cy = field_y + field_h + gap;
    };

    // Two columns. Row 1 is "Wireless debugging screen" data (IP + Connect
    // port shown side by side on the phone). Row 2 is the one-shot
    // "Pair with device" popup data (pair port + pairing code).
    int col_w = (panel_w - pad*2 - gap) / 2;
    int col1_x = panel_x + pad;
    int col2_x = col1_x + col_w + gap;

    draw_field(0, "IP address: *",    android_ip_,           "192.168.1.42",
               col1_x, col_w, false);
    draw_field(1, "Connect port:",    android_connect_port_, "e.g. 46029",
               col2_x, col_w, true);
    draw_field(2, "Pair port: *",     android_port_,         "42379",
               col1_x, col_w, false);
    draw_field(3, "Pairing code: *",  android_pin_,          "123456",
               col2_x, col_w, true);

    // Status line — word-wrapped to the panel width so long error messages
    // remain readable instead of being squished into a single line. Lines
    // were pre-computed above so the panel height already accounts for them.
    {
        if (!status_lines.empty()) {
            uint8_t cr = status_err ? 230 : 140;
            uint8_t cg = status_err ? 110 : 200;
            uint8_t cb = status_err ? 110 : 140;
            int max_w = panel_w - pad * 2;
            for (auto& ln : status_lines) {
                int tw = 0, th = 0;
                SDL_Texture* tex = make_text_texture(sdl_renderer_, ln, label_h,
                                                     cr, cg, cb, &tw, &th);
                if (tex) {
                    int dw = std::min(tw, max_w);
                    SDL_Rect d = {panel_x + pad, cy, dw, th};
                    SDL_SetTextureAlphaMod(tex, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
                    SDL_DestroyTexture(tex);
                }
                cy += th > 0 ? th : label_h;
            }
            cy += gap;
        } else {
            cy += label_h + gap;
        }
    }

    // Buttons
    int bw = (panel_w - pad*2 - gap) / 2;
    android_connect_btn_    = {panel_x + pad,           cy, bw, btn_h};
    android_disconnect_btn_ = {panel_x + pad + bw + gap, cy, bw, btn_h};

    auto draw_btn = [&](const BtnRect& r, const char* label, bool primary) {
        SDL_SetRenderDrawColor(sdl_renderer_,
            primary ? 60 : 50, primary ? 110 : 50, primary ? 200 : 55, alpha);
        SDL_Rect br = {r.x, r.y, r.w, r.h};
        SDL_RenderFillRect(sdl_renderer_, &br);
        SDL_SetRenderDrawColor(sdl_renderer_,
            primary ? 110 : 90, primary ? 150 : 90, primary ? 230 : 100, alpha);
        SDL_RenderDrawRect(sdl_renderer_, &br);
        int tw=0, th=0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, label, btn_h - 12,
            240, 240, 245, &tw, &th);
        if (tex) {
            SDL_Rect d = {r.x + (r.w-tw)/2, r.y + (r.h-th)/2, tw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
        }
    };
    draw_btn(android_connect_btn_,    android_busy_ ? "Working..." : "Connect", true);
    draw_btn(android_disconnect_btn_, "Disconnect", false);
}

void Renderer::draw_android_help() {
    if (frame_dst_w_ == 0) return;

    // Help overlay floats on top of the connect panel. Anchored inside the
    // phone screen rectangle so it stays inside the device frame.
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    int panel_w = (int)(svw * 0.92f);
    int pad     = std::max(10, panel_w / 24);
    int title_h = std::max(18, panel_w / 18);
    int line_h  = std::max(13, panel_w / 28);

    // Help body — short, scannable, no markdown. Each entry is a line of
    // text rendered at line_h. An empty string draws a half-line gap.
    // Uses ASCII only — the bundled font has no glyphs for arrows or em
    // dashes, which would render as the missing-glyph box (often shown
    // as a question mark).
    static const char* lines[] = {
        "How to set up your Android phone",
        "",
        "1. Open Settings on the phone.",
        "2. Tap 'About phone' / 'Software information'.",
        "3. Tap 'Build number' seven times to enable Developer options.",
        "4. Back in Settings open 'Developer options' and turn on",
        "   'Wireless debugging' (the phone must be on the same Wi-Fi as the PC).",
        "",
        "Reading the values from the phone",
        "",
        "  IP address:    first part of 'IP address & Port' on the main",
        "                 Wireless debugging screen (e.g. 192.168.10.73).",
        "",
        "  Connect port:  second part of the same line (e.g. :46029).",
        "                 Stable until you toggle Wi-Fi. Recommended.",
        "",
        "  Pair port:     second part of 'IP address & Port' inside the",
        "                 'Pair device with pairing code' popup. This port",
        "                 is one-shot - reopen the popup if it expires.",
        "",
        "  Pairing code:  the 6-digit number in the same popup.",
        "",
        "Note for managed devices",
        "",
        "  Developer options can be blocked by your organisation through MDM",
        "  (Intune, Workspace ONE, etc.). If 'Build number' refuses to enable",
        "  developer mode, your work profile is locked down and Wireless",
        "  debugging cannot be used on this device.",
        "",
        "Tip",
        "",
        "  Once you have entered IP and Connect port, only Pair port and",
        "  Pairing code change between sessions.",
    };
    int n_lines = (int)(sizeof(lines) / sizeof(lines[0]));

    int total_h = pad + title_h + pad + (line_h + 2) * n_lines + pad;
    // Cap so the help always fits inside the phone screen even in portrait.
    int max_h = svh - 4;
    if (total_h > max_h) total_h = max_h;

    int panel_x = svx + (svw - panel_w) / 2;
    int panel_y = svy + (svh - total_h) / 2;

    android_help_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    uint8_t alpha = 250;
    uint8_t text_alpha = 255;

    int pr = std::max(8, pad / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, 24, 26, 32, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr*2, total_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, total_h - pr*2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, total_h - pr*2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + total_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + total_h - pr, pr);
    // Subtle accent border so it stands out from the connect panel below.
    SDL_SetRenderDrawColor(sdl_renderer_, 90, 130, 200, alpha);
    SDL_Rect outline = {panel_x + pr, panel_y, panel_w - pr*2, total_h};
    SDL_RenderDrawRect(sdl_renderer_, &outline);

    // Close button (× in the top-right corner).
    {
        int sz = title_h;
        android_help_close_btn_ = {panel_x + panel_w - sz - pad/2,
                                   panel_y + pad/2, sz, sz};
        int tw=0, th=0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, "x", sz,
                                             200, 200, 200, &tw, &th);
        if (tex) {
            SDL_Rect d = {android_help_close_btn_.x + (sz-tw)/2,
                          android_help_close_btn_.y + (sz-th)/2, tw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
        }
    }

    int cy = panel_y + pad;
    for (int i = 0; i < n_lines; ++i) {
        const char* line = lines[i];
        if (line[0] == '\0') {
            cy += line_h / 2;
            continue;
        }
        // Lines without leading whitespace that don't start with a digit-dot
        // pattern are treated as section headings (bigger + brighter).
        bool is_heading = (i == 0)
            || (line[0] != ' ' && !(line[0] >= '0' && line[0] <= '9'));
        int h = is_heading ? title_h : line_h;
        uint8_t cr = is_heading ? 230 : 195;
        uint8_t cg = is_heading ? 230 : 200;
        uint8_t cb = is_heading ? 245 : 210;
        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, line, h,
                                             cr, cg, cb, &tw, &th);
        if (tex) {
            int max_w = panel_w - pad * 2;
            int dw = std::min(tw, max_w);
            SDL_Rect d = {panel_x + pad, cy, dw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
            cy += th + 2;
        } else {
            cy += h + 2;
        }
        if (cy > panel_y + total_h - pad) break;
    }
}

void Renderer::take_screenshot() {
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        std::cout << "[Screenshot] No frame data available\n";
        return;
    }

    const bool save = settings_.screenshot_save_to_folder;
    const bool clip = settings_.screenshot_copy_to_clipboard;
    if (!save && !clip) {
        std::cout << "[Screenshot] Both save-to-folder and clipboard are disabled in settings\n";
        toast_text_ = "Screenshot disabled in Settings";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    if (save) std::filesystem::create_directories(screenshot_dir_);

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
            bool wrote = true;
            if (save) {
                wrote = stbi_write_png(filename.c_str(), out_w, out_h, 4, composite, out_w * 4) != 0;
            }
            if (wrote) {
                saved = true;
                if (clip) copy_to_clipboard(composite, out_w, out_h);
                if (save) std::cout << "[Screenshot] Saved: " << filename << " (" << out_w << "x" << out_h << ")\n";
                else      std::cout << "[Screenshot] Copied to clipboard (" << out_w << "x" << out_h << ")\n";
            }
            delete[] composite;
        }
    } else {
        bool wrote = true;
        if (save) {
            wrote = stbi_write_png(filename.c_str(), last_frame_w_, last_frame_h_, 4,
                                   last_frame_data_.data(), last_frame_stride_) != 0;
        }
        if (wrote) {
            saved = true;
            if (clip) copy_to_clipboard(last_frame_data_.data(), last_frame_w_, last_frame_h_);
            if (save) std::cout << "[Screenshot] Saved: " << filename << "\n";
            else      std::cout << "[Screenshot] Copied to clipboard\n";
        }
    }

    if (saved) {
        if      (save && clip) toast_text_ = "Saved to Pictures & copied to clipboard";
        else if (save)         toast_text_ = "Saved to Pictures";
        else                   toast_text_ = "Copied to clipboard";
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
        SetWindowRgn(info.info.win.window, nullptr, FALSE);
        window_shape_last_lp_w_ = -1;
        window_shape_last_frame_w_ = -1;
        window_shape_last_frame_x_ = -1;
        window_shape_last_frame_y_ = -1;
        return;
    }

    // Compute current drawer pixel width and skip the (expensive)
    // SetWindowRgn call when nothing the region depends on has changed.
    // Calling SetWindowRgn every frame causes Windows to re-clip the
    // non-client area which produces visible stutter during animation.
    int cur_lp_w = (log_panel_anim_ > 0.01f)
                       ? (int)(log_panel_full_w_ * log_panel_anim_)
                       : 0;
    if (cur_lp_w == window_shape_last_lp_w_ &&
        frame_dst_w_ == window_shape_last_frame_w_ &&
        frame_dst_x_ == window_shape_last_frame_x_ &&
        frame_dst_y_ == window_shape_last_frame_y_) {
        return;
    }
    window_shape_last_lp_w_ = cur_lp_w;
    window_shape_last_frame_w_ = frame_dst_w_;
    window_shape_last_frame_x_ = frame_dst_x_;
    window_shape_last_frame_y_ = frame_dst_y_;

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
        int lp_w = (int)(log_panel_full_w_ * log_panel_anim_);
        int lp_margin = std::max(4, frame_dst_h_ / 40);
        int drawer_inset = frame_dst_h_ / 16;
        int lp_cr = std::max(8, cr / 2);
        int lp_left = frame_dst_x_ + frame_dst_w_; // flush with phone
        int lp_right = frame_dst_x_ + frame_dst_w_ + lp_w - lp_margin;
        int lp_top = frame_dst_y_ + drawer_inset;
        int lp_bottom = frame_dst_y_ + frame_dst_h_ - drawer_inset + 1;
        int lp_width_px = lp_right - lp_left;
        if (lp_width_px > 0) {
            HRGN log_rgn = nullptr;
            if (lp_width_px > lp_cr * 2) {
                // Wide enough for rounded right edge
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
                    log_rgn = log_round;
                    DeleteObject(log_flat);
                } else {
                    if (log_round) DeleteObject(log_round);
                    if (log_flat) DeleteObject(log_flat);
                }
            } else {
                // Too narrow for rounded corners — use a plain rect so the
                // drawer is visible from the very first pixel of animation.
                log_rgn = CreateRectRgn(lp_left, lp_top, lp_right + 1, lp_bottom);
            }
            if (log_rgn) {
                CombineRgn(phone_rgn, phone_rgn, log_rgn, RGN_OR);
                DeleteObject(log_rgn);
            }
        }
    }

    SetWindowRgn(info.info.win.window, phone_rgn, FALSE);
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
