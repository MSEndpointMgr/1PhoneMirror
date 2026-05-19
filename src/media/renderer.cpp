#include <opm/media/renderer.h>
#include <opm/log_buffer.h>
#include <opm/config.h>
#include <opm/network/update_check.h>
#ifdef _WIN32
#include <opm/media/ocr.h>
#endif
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

#ifdef _WIN32
// Locate TechSmith Snagit Editor on this machine. Checks the published
// App Paths registry key first (works for any installed major version),
// then falls back to scanning Program Files for "Snagit <ver>" folders.
// Returns an empty string if Snagit is not installed.
static std::string find_snagit_editor() {
    const HKEY roots[]  = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (HKEY root : roots) {
        for (REGSAM view : views) {
            HKEY hk = nullptr;
            if (RegOpenKeyExA(root,
                    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\SnagitEditor.exe",
                    0, KEY_READ | view, &hk) == ERROR_SUCCESS) {
                char buf[MAX_PATH] = {0};
                DWORD cb = sizeof(buf);
                DWORD type = 0;
                LONG r = RegQueryValueExA(hk, nullptr, nullptr, &type,
                                          (LPBYTE)buf, &cb);
                RegCloseKey(hk);
                if (r == ERROR_SUCCESS && cb > 0) {
                    std::string path(buf);
                    if (!path.empty() && std::filesystem::exists(path)) return path;
                }
            }
        }
    }
    const char* env_keys[] = { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" };
    for (const char* k : env_keys) {
        char buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableA(k, buf, sizeof(buf));
        if (n == 0 || n >= sizeof(buf)) continue;
        std::filesystem::path techsmith = std::filesystem::path(buf) / "TechSmith";
        std::error_code ec;
        if (!std::filesystem::is_directory(techsmith, ec)) continue;
        for (auto& entry : std::filesystem::directory_iterator(techsmith, ec)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.rfind("Snagit", 0) != 0) continue;
            auto candidate = entry.path() / "SnagitEditor.exe";
            if (std::filesystem::exists(candidate, ec)) {
                return candidate.string();
            }
        }
    }
    return {};
}

// Open `file_path` in Snagit Editor. Returns true if Snagit was found
// and launched. ShellExecute with the explicit editor path is the most
// reliable invocation (the editor accepts a single file argument).
static bool open_in_snagit(const std::string& file_path) {
    std::string editor = find_snagit_editor();
    if (editor.empty()) {
        std::cout << "[Snagit] SnagitEditor.exe not found on this machine\n";
        return false;
    }
    std::string params = "\"" + file_path + "\"";
    HINSTANCE r = ShellExecuteA(nullptr, "open", editor.c_str(),
                                params.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        std::cout << "[Snagit] ShellExecute failed (code "
                  << (INT_PTR)r << ") for " << editor << "\n";
        return false;
    }
    std::cout << "[Snagit] Opened " << file_path << " in " << editor << "\n";
    return true;
}
#else
static inline std::string find_snagit_editor() { return {}; }
static inline bool open_in_snagit(const std::string&) { return false; }
#endif

// ---------------------------------------------------------------------------
// GDI text helpers → SDL_Texture  (Segoe UI, anti-aliased)
// ---------------------------------------------------------------------------
#ifdef _WIN32

// Measure-only helper used by word-wrap layout. Returns the same width
// that make_text_texture would produce (advance width + overhang +
// right-padding) without going through SDL.
static int measure_text_w(const std::string& text, int font_height) {
    if (text.empty()) return 0;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT font = CreateFontA(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SIZE sz = {0, 0};
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    TEXTMETRICA tm;
    int extra = 0;
    if (GetTextMetricsA(hdc, &tm)) extra = tm.tmOverhang;
    extra += (font_height + 3) / 4;
    SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
    return sz.cx + extra;
}

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

namespace opm::media {

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
    settings_ = opm::Settings::load();
    phone_frame_.set_bezel_color(settings_.bezel_r, settings_.bezel_g, settings_.bezel_b);
    SDL_SetWindowAlwaysOnTop(window_, settings_.always_on_top ? SDL_TRUE : SDL_FALSE);

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
        // Line 2: "(c) 2026 \u266B Simon Skotheimsvik, MVP \u00B7 v0.3.8"
        footer_line2_.push_back(seg(L"\u00A9 2026 ", 100, 100, 100));
        // Beamed-eighth-notes glyph — render via Segoe UI Symbol so it works
        // on Windows builds where the regular Segoe UI font lacks U+266B.
        footer_line2_.push_back(seg(L"\u266B", 100, 100, 100,
                                     "", "Tuned carefully for the community",
                                     L"Segoe UI Symbol"));
        footer_line2_.push_back(seg(L" ", 100, 100, 100));
        footer_line2_.push_back(seg(L"Simon Skotheimsvik, MVP", 100, 100, 100,
                                     "https://linktr.ee/simonskotheimsvik", "More info of Simon"));
        // Buy-me-a-coffee link — yellow coffee glyph (U+2615) sandwiched
        // between the author name and the version number. Rendered via
        // Segoe UI Symbol/Emoji so the glyph is present on stock Windows.
        footer_line2_.push_back(seg(L" ", 100, 100, 100));
        footer_line2_.push_back(seg(L"\u2615", 230, 200, 60,
                                     "https://buymeacoffee.com/simonskothn",
                                     "Buy me a coffee",
                                     L"Segoe UI Symbol"));
        // Line 3: " · v0.4.0" — broken onto its own line so the second
        // line stays a comfortable width on narrow phone aspects.
        footer_line3_.push_back(seg(L"v0.4.1", 100, 100, 100,
                                     "", "Version history (V)"));

        // Mirror the same content for the Info panel, but baked at the
        // smaller font size used by the network-requirement section so
        // the visual weight matches the surrounding text.
        auto iseg = [&](const std::wstring& text, uint8_t r, uint8_t g, uint8_t b,
                         const std::string& url = "", const std::string& tip = "",
                         const wchar_t* font = L"Segoe UI") -> FooterSeg {
            FooterSeg fs;
            fs.url = url;
            fs.tooltip = tip;
            fs.tex = make_text_texture_w(sdl_renderer_, text, 30, r, g, b, &fs.w, &fs.h, font);
            return fs;
        };
        info_footer_line1_.push_back(iseg(L"1PhoneMirror by ", 130, 130, 130));
        info_footer_line1_.push_back(iseg(L"MSEndpointMgr", 130, 130, 130,
                                           "https://msendpointmgr.com/", "Open MSEndpointMgr"));
        info_footer_line2_.push_back(iseg(L"\u00A9 2026 ", 130, 130, 130));
        info_footer_line2_.push_back(iseg(L"\u266B", 130, 130, 130,
                                           "", "Tuned carefully for the community",
                                           L"Segoe UI Symbol"));
        info_footer_line2_.push_back(iseg(L" ", 130, 130, 130));
        info_footer_line2_.push_back(iseg(L"Simon Skotheimsvik, MVP", 130, 130, 130,
                                           "https://linktr.ee/simonskotheimsvik", "More info of Simon"));
        info_footer_line2_.push_back(iseg(L" ", 130, 130, 130));
        info_footer_line2_.push_back(iseg(L"\u2615", 230, 200, 60,
                                           "https://buymeacoffee.com/simonskothn",
                                           "Buy me a coffee",
                                           L"Segoe UI Symbol"));
        info_footer_line3_.push_back(iseg(L"v0.4.1", 130, 130, 130,
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
        info_lines_.push_back(make_info(L"1PhoneMirror v0.4.1", 44, 255, 255, 255));
        info_lines_.push_back(make_info(L"AirPlay (iOS) \u00B7 scrcpy (Android)", 34, 160, 160, 160));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"(F) Fullscreen \u00B7 (M) Menu \u00B7 (L) Log \u00B7 (A) Add Android", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(I) Info \u00B7 (V) Version \u00B7 (S) Settings \u00B7 (Esc) Quit", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(Ctrl+S) Screenshot \u00B7 (Ctrl+Shift+S) Annotate", 30, 130, 130, 130));
#ifdef _WIN32
        info_lines_.push_back(make_info(L"(Ctrl+Shift+T) OCR copy text from a region", 30, 130, 130, 130));
#endif
        info_lines_.push_back(make_info(L"(Ctrl+R) Record \u00B7 (Ctrl+0) Reset window size", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(Ctrl+1\u20139) Switch to device 1\u20139 in the bottom-bezel picker", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"In log: (Ctrl+C) Copy \u00B7 (Ctrl+X) Clear", 30, 130, 130, 130));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"Network requirements", 34, 160, 160, 160));
        info_lines_.push_back(make_info(L"Same Wi-Fi / VLAN as the phone, mDNS (UDP 5353) allowed", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"AirPlay: TCP 7000/7001/7100, UDP 6000-6010", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"Android: TCP 27183 (loopback) + ADB pair port from phone", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"Installer adds Windows Firewall rules for the .exe", 30, 130, 130, 130));

        // "About" header that sits just above the MSEndpointMgr/Simon/version
        // footer block at the bottom of the Info panel. Sized to match the
        // "Network requirements" section header.
        info_about_header_ = make_info(L"About", 34, 160, 160, 160);
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
        version_lines_.push_back(make_ver(L"18.05.2026 \u2013 0.4.1", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Open screenshots directly in Snagit Editor (new Settings toggle)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"18.05.2026 \u2013 0.4.0", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Multi-device shortcuts (Ctrl+1\u20139), always-on-top, Apple naming", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"17.05.2026 \u2013 0.3.9", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Small telemetry in settings", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"16.05.2026 \u2013 0.3.8", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Tuned for larger screens from macOS", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"15.05.2026 \u2013 0.3.7", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Copy text from the phone screen (Ctrl+Shift+T)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"14.05.2026 \u2013 0.3.6", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Screenshot annotation tools (Ctrl+Shift+S)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"13.05.2026 \u2013 0.3.5", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Version check and UI tunings", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"13.05.2026 \u2013 0.3.4", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"New Android discovery routine for easy connect", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"12.05.2026 \u2013 0.3.3", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Right-click resize grip to reset to default size", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"11.05.2026 \u2013 0.3.2", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"User interface fixes", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"10.05.2026 \u2013 0.3.1", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Phone frame on recordings (rounded corners, transparent on GIF)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"10.05.2026 \u2013 0.3.0", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Screen recording: MP4/GIF, Ctrl+R, right-click for delay/timed", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"09.05.2026 \u2013 0.2.5", 34, 200, 200, 255));
        version_lines_.push_back(make_ver(L"Info panel: copy network test PowerShell script (with MDM check)", 30, 160, 160, 160));
        version_lines_.push_back({nullptr, 0, 0});
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
    stop_android_discovery();
    if (recorder_.is_recording()) {
        std::cout << "[Recorder] Finalising on shutdown\n";
        recorder_.stop();
    }
    for (auto& s : footer_line1_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : footer_line2_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : footer_line3_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    footer_line1_.clear();
    footer_line2_.clear();
    footer_line3_.clear();
    for (auto& s : info_footer_line1_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : info_footer_line2_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : info_footer_line3_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    info_footer_line1_.clear();
    info_footer_line2_.clear();
    info_footer_line3_.clear();
    if (info_about_header_.tex) {
        SDL_DestroyTexture(info_about_header_.tex);
        info_about_header_.tex = nullptr;
    }
    if (footer_tooltip_tex_) { SDL_DestroyTexture(footer_tooltip_tex_); footer_tooltip_tex_ = nullptr; }
    if (toast_tex_) { SDL_DestroyTexture(toast_tex_); toast_tex_ = nullptr; }
    if (update_line1_tex_) { SDL_DestroyTexture(update_line1_tex_); update_line1_tex_ = nullptr; }
    if (update_link_tex_) { SDL_DestroyTexture(update_link_tex_); update_link_tex_ = nullptr; }
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
    if (pending_overlay_tex_) { SDL_DestroyTexture(pending_overlay_tex_); pending_overlay_tex_ = nullptr; }
    if (protected_overlay_tex_) { SDL_DestroyTexture(protected_overlay_tex_); protected_overlay_tex_ = nullptr; }
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
            // Screenshot annotator is modal: while it's up, every event
            // gets routed through it first and most are swallowed.
            if (annotator_active_) {
                if (event.type == SDL_QUIT) { running_.store(false); return; }
                if (handle_annotator_event(event)) continue;
            }
#ifdef _WIN32
            // OCR region picker is modal too.
            if (ocr_active_) {
                if (event.type == SDL_QUIT) { running_.store(false); return; }
                if (handle_ocr_event(event)) continue;
            }
#endif
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
                if (event.key.keysym.sym == SDLK_s &&
                    (event.key.keysym.mod & KMOD_CTRL) &&
                    (event.key.keysym.mod & KMOD_SHIFT)) {
                    // Ctrl+Shift+S: open the screenshot annotator on the
                    // current composited frame. Handled before plain Ctrl+S
                    // so the shifted shortcut wins.
                    begin_annotation();
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                } else if (event.key.keysym.sym == SDLK_s &&
                           (event.key.keysym.mod & KMOD_CTRL) &&
                           !(event.key.keysym.mod & KMOD_SHIFT)) {
                    screenshot_requested_ = true;
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Ctrl+R toggles screen recording.
                if (event.key.keysym.sym == SDLK_r && (event.key.keysym.mod & KMOD_CTRL)) {
                    record_toggle_requested_ = true;
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Ctrl+0 resets the window to its default size — matches
                // the "reset zoom" idiom (Ctrl+0 in browsers / editors) and
                // mirrors the right-click action on the resize grip.
                // Explicitly exclude Alt so AltGr+0 (which types '}' on
                // Norwegian/German/etc. layouts and arrives as Ctrl+Alt on
                // Windows) does not get misread as Ctrl+0.
                if ((event.key.keysym.sym == SDLK_0 || event.key.keysym.sym == SDLK_KP_0) &&
                    (event.key.keysym.mod & KMOD_CTRL) &&
                    !(event.key.keysym.mod & KMOD_ALT) &&
                    !(event.key.keysym.mod & KMOD_SHIFT)) {
                    reset_window_to_default_size();
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Ctrl+1..Ctrl+9 — switch active source by picker order.
                // Same Alt/Shift guard as Ctrl+0 to avoid AltGr collisions.
                if ((event.key.keysym.mod & KMOD_CTRL) &&
                    !(event.key.keysym.mod & KMOD_ALT) &&
                    !(event.key.keysym.mod & KMOD_SHIFT)) {
                    int picked = -1;
                    SDL_Keycode k = event.key.keysym.sym;
                    if      (k == SDLK_1 || k == SDLK_KP_1) picked = 0;
                    else if (k == SDLK_2 || k == SDLK_KP_2) picked = 1;
                    else if (k == SDLK_3 || k == SDLK_KP_3) picked = 2;
                    else if (k == SDLK_4 || k == SDLK_KP_4) picked = 3;
                    else if (k == SDLK_5 || k == SDLK_KP_5) picked = 4;
                    else if (k == SDLK_6 || k == SDLK_KP_6) picked = 5;
                    else if (k == SDLK_7 || k == SDLK_KP_7) picked = 6;
                    else if (k == SDLK_8 || k == SDLK_KP_8) picked = 7;
                    else if (k == SDLK_9 || k == SDLK_KP_9) picked = 8;
                    if (picked >= 0 && get_sources_fn_ && set_active_source_fn_) {
                        auto sources = get_sources_fn_();
                        if (picked < (int)sources.size()) {
                            const auto& sel = sources[picked];
                            if (!sel.active) {
                                pending_source_name_ = sel.name;
                                source_just_switched_ = true;
                            }
                            set_active_source_fn_(sel.id);
                            btn_flash_ = true;
                            btn_flash_start_ = std::chrono::steady_clock::now();
                        }
                    }
                }
#ifdef _WIN32
                // Ctrl+Shift+T: OCR copy. Open the region picker over the
                // current composite; on mouse-up the cropped pixels go
                // through Windows.Media.Ocr and the recognised text lands
                // on the clipboard.
                if (event.key.keysym.sym == SDLK_t &&
                    (event.key.keysym.mod & KMOD_CTRL) &&
                    (event.key.keysym.mod & KMOD_SHIFT)) {
                    begin_ocr();
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
#endif
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
                        for (auto& ln : opm::LogBuffer::instance().get_lines()) {
                            all += ln; all += '\n';
                        }
                        SDL_SetClipboardText(all.c_str());
                        std::cout << "[Renderer] Log copied to clipboard ("
                                  << all.size() << " bytes)\n";
                    } else if (event.key.keysym.sym == SDLK_x) {
                        opm::LogBuffer::instance().clear();
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
                    // When the help overlay is showing, only clicks that
                    // actually land on it are intercepted. Clicks outside
                    // the help panel fall through so the title-bar buttons
                    // (close, menu, log toggle) keep working.
                    if (android_help_visible_) {
                        if (in(android_help_close_btn_) || in(android_help_btn_)) {
                            android_help_visible_ = false;
                            break;
                        }
                        // Scrollbar thumb drag — page-up/down on track.
                        if (android_help_thumb_rect_.w > 0 && in(android_help_thumb_rect_)) {
                            android_help_dragging_ = true;
                            android_help_drag_offset_ = my - android_help_thumb_rect_.y;
                            break;
                        }
                        if (android_help_track_rect_.w > 0 && in(android_help_track_rect_)) {
                            int page = std::max(40, frame_dst_w_ / 5);
                            if (my < android_help_thumb_rect_.y)
                                android_help_scroll_ = std::max(0, android_help_scroll_ - page);
                            else
                                android_help_scroll_ = std::min(android_help_max_scroll_,
                                                                android_help_scroll_ + page);
                            break;
                        }
                        // Click inside the help panel but on neutral area:
                        // swallow so it doesn't pick a discovered device
                        // underneath. Clicks outside the panel fall through.
                        if (in(android_help_panel_rect_)) break;
                        // Click outside the help — dismiss it and let the
                        // event continue so e.g. the title-bar close button
                        // can still receive it.
                        android_help_visible_ = false;
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
                        if (android_help_visible_) android_help_scroll_ = 0;
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
                    // Discovered-device row clicked: copy ip/ports into the
                    // form so the user only has to type the pairing code.
                    {
                        bool consumed = false;
                        std::vector<DiscoveredAndroidDevice> snap;
                        {
                            std::lock_guard lk(android_discovered_mutex_);
                            snap = android_discovered_;
                        }
                        for (size_t i = 0; i < android_discover_btns_.size() &&
                                            i < snap.size(); ++i) {
                            if (in(android_discover_btns_[i])) {
                                const auto& dev = snap[i];
                                android_ip_ = dev.ip;
                                if (!dev.connect_port.empty())
                                    android_connect_port_ = dev.connect_port;
                                if (!dev.pair_port.empty()) {
                                    android_port_ = dev.pair_port;
                                    // Pairing required: focus the PIN field.
                                    android_focus_ = 3;
                                } else {
                                    android_focus_ = 2;
                                }
                                consumed = true;
                                break;
                            }
                        }
                        if (consumed) break;
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
                    } else if (record_btn_.w > 0 && in_rect(mx, my, record_btn_.x, record_btn_.y,
                                                              record_btn_.w, record_btn_.h)) {
                        target = "record";
                    } else if (bezel_record_btn_.w > 0 &&
                               in_rect(mx, my, bezel_record_btn_.x, bezel_record_btn_.y,
                                       bezel_record_btn_.w, bezel_record_btn_.h)) {
                        target = "record";
                    } else if (screenshot_btn_.w > 0 &&
                               in_rect(mx, my, screenshot_btn_.x, screenshot_btn_.y,
                                       screenshot_btn_.w, screenshot_btn_.h)) {
                        target = "screenshot";
                    } else if (bezel_screenshot_btn_.w > 0 &&
                               in_rect(mx, my, bezel_screenshot_btn_.x, bezel_screenshot_btn_.y,
                                       bezel_screenshot_btn_.w, bezel_screenshot_btn_.h)) {
                        target = "screenshot";
                    } else if (phone_frame_enabled_ && resize_grip_.w > 0 &&
                               in_rect(mx, my, resize_grip_.x, resize_grip_.y,
                                       resize_grip_.w, resize_grip_.h)) {
                        target = "resize";
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

                    // Update banner — handle clicks before anything else
                    // so the link and close glyph win over background
                    // surfaces beneath them. Banner draws above any frame.
                    if (update_banner_active_) {
                        if (update_close_rect_.w > 0 &&
                            in_rect(mx, my, update_close_rect_.x, update_close_rect_.y,
                                    update_close_rect_.w, update_close_rect_.h)) {
                            update_banner_active_ = false;
                            break;
                        }
                        if (update_link_rect_.w > 0 &&
                            in_rect(mx, my, update_link_rect_.x, update_link_rect_.y,
                                    update_link_rect_.w, update_link_rect_.h)) {
#ifdef _WIN32
                            std::string url;
                            {
                                std::lock_guard<std::mutex> lk(update_check_mutex_);
                                url = update_release_url_.empty()
                                    ? std::string("https://github.com/MSEndpointMgr/1PhoneMirror")
                                    : update_release_url_;
                            }
                            ShellExecuteA(nullptr, "open", url.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
#endif
                            update_banner_active_ = false;
                            break;
                        }
                    }

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
                                for (auto& ln : opm::LogBuffer::instance().get_lines()) {
                                    all += ln;
                                    all += '\n';
                                }
                                SDL_SetClipboardText(all.c_str());
                                std::cout << "[Renderer] Log copied to clipboard ("
                                          << all.size() << " bytes)\n";
                            } else if (clicked_action == "clear") {
                                opm::LogBuffer::instance().clear();
                                std::cout << "[Renderer] Log cleared\n";
                            } else if (tgt == "screenshot") {
                                if (clicked_action == "shot") {
                                    screenshot_requested_ = true;
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
                                } else if (clicked_action == "annotate") {
                                    begin_annotation();
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
#ifdef _WIN32
                                } else if (clicked_action == "ocr") {
                                    begin_ocr();
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
#endif
                                } else if (clicked_action == "open_dir") {
                                    open_screenshot_folder();
                                }
                            } else if (tgt == "resize" && clicked_action == "reset_size") {
                                reset_window_to_default_size();
                            } else if (tgt == "record") {
                                if (clicked_action == "start") {
                                    pending_record_duration_sec_ = 0;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "stop" ||
                                           clicked_action == "cancel") {
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "delay5") {
                                    pending_record_duration_sec_ = 0;
                                    record_countdown_ms_ = 5000;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "timed5") {
                                    pending_record_duration_sec_ = 5;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "timed10") {
                                    pending_record_duration_sec_ = 10;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "timed15") {
                                    pending_record_duration_sec_ = 15;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                }
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
                                // If the user is switching to a *different*
                                // source, show a "waiting for <name>" overlay
                                // until the next frame arrives so the app
                                // does not appear frozen during re-negotiation.
                                if (get_sources_fn_) {
                                    auto sources = get_sources_fn_();
                                    bool is_active = false;
                                    std::string nm;
                                    for (auto& s : sources) {
                                        if (s.id == id) { is_active = s.active; nm = s.name; break; }
                                    }
                                    if (!is_active && !nm.empty()) {
                                        pending_source_name_ = nm;
                                    }
                                }
                                if (set_active_source_fn_) {
                                    // Flag so the next texture-resize event
                                    // triggers a phone-frame regenerate +
                                    // window reshape (otherwise the
                                    // canvas-change branch would keep the
                                    // outgoing source's frame and letterbox
                                    // the new device's video).
                                    source_just_switched_ = true;
                                    set_active_source_fn_(id);
                                }
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

                    // Bezel record button (only present when the menu is
                    // hidden — same effect as Ctrl+R / island record).
                    if (bezel_record_btn_.w > 0 &&
                        in_rect(mx, my, bezel_record_btn_.x, bezel_record_btn_.y,
                                bezel_record_btn_.w, bezel_record_btn_.h)) {
                        record_toggle_requested_ = true;
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
                    // Footer link clicks (waiting screen + info panel).
                    // The info panel re-publishes the same footer hits at
                    // its bottom, so the same dispatcher handles both.
                    if (!ever_received_frame_ || info_panel_visible_) {
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
                                if (hit.tooltip == "info_check_updates") {
                                    check_for_update_async(true);
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
                            if (in_rect(mx, my, settings_toggle_snagit_btn_.x, settings_toggle_snagit_btn_.y,
                                        settings_toggle_snagit_btn_.w, settings_toggle_snagit_btn_.h)) {
                                settings_.screenshot_open_in_snagit = !settings_.screenshot_open_in_snagit;
                                // If user just enabled it, warn (in toast) when
                                // Snagit isn't installed so they're not
                                // surprised by silent no-ops later.
                                if (settings_.screenshot_open_in_snagit && find_snagit_editor().empty()) {
                                    toast_text_ = "Snagit Editor not found on this PC";
                                    toast_active_ = true;
                                    toast_start_ = std::chrono::steady_clock::now();
                                }
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_compname_btn_.x, settings_toggle_compname_btn_.y,
                                        settings_toggle_compname_btn_.w, settings_toggle_compname_btn_.h)) {
                                settings_.use_computer_name = !settings_.use_computer_name;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_aot_btn_.x, settings_toggle_aot_btn_.y,
                                        settings_toggle_aot_btn_.w, settings_toggle_aot_btn_.h)) {
                                settings_.always_on_top = !settings_.always_on_top;
                                SDL_SetWindowAlwaysOnTop(window_, settings_.always_on_top ? SDL_TRUE : SDL_FALSE);
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_telemetry_btn_.x, settings_toggle_telemetry_btn_.y,
                                        settings_toggle_telemetry_btn_.w, settings_toggle_telemetry_btn_.h)) {
                                settings_.telemetry_enabled = !settings_.telemetry_enabled;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_log_btn_.x, settings_toggle_log_btn_.y,
                                        settings_toggle_log_btn_.w, settings_toggle_log_btn_.h)) {
                                log_to_file_session_ = !log_to_file_session_;
                                if (log_to_file_session_) {
                                    std::error_code ec;
                                    std::filesystem::create_directories(screenshot_dir_, ec);
                                    std::string path = screenshot_dir_ + "/1PhoneMirror.log";
                                    if (opm::LogBuffer::instance().open_file(path)) {
                                        std::cout << "[Log] File logging enabled: " << path << "\n";
                                    } else {
                                        std::cout << "[Log] Failed to open log file: " << path << "\n";
                                        log_to_file_session_ = false;
                                    }
                                } else {
                                    std::cout << "[Log] File logging disabled\n";
                                    opm::LogBuffer::instance().close_file();
                                }
                            }
                            if (settings_fmt_mp4_btn_.w > 0 &&
                                in_rect(mx, my, settings_fmt_mp4_btn_.x, settings_fmt_mp4_btn_.y,
                                        settings_fmt_mp4_btn_.w, settings_fmt_mp4_btn_.h)) {
                                settings_.record_format = 0;
                                settings_.save();
                            }
                            if (settings_fmt_gif_btn_.w > 0 &&
                                in_rect(mx, my, settings_fmt_gif_btn_.x, settings_fmt_gif_btn_.y,
                                        settings_fmt_gif_btn_.w, settings_fmt_gif_btn_.h)) {
                                settings_.record_format = 1;
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
                    // Record button (toggle / cancel countdown)
                    if (record_btn_.w > 0 &&
                        in_rect(mx, my, record_btn_.x, record_btn_.y,
                                record_btn_.w, record_btn_.h)) {
                        record_toggle_requested_ = true;
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
                        // "Copy network test script" — copies a PowerShell
                        // troubleshooting block to the clipboard.
                        if (info_copy_ps_btn_.w > 0 &&
                            in_rect(mx, my, info_copy_ps_btn_.x, info_copy_ps_btn_.y,
                                    info_copy_ps_btn_.w, info_copy_ps_btn_.h)) {
                            const char* ps_script =
                                "# 1PhoneMirror — network troubleshooting\r\n"
                                "Write-Host '== Firewall profile (LocalRulesAllowed = False means MDM/Intune blocks installer rules) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetFirewallProfile |\r\n"
                                "    Format-Table Name, Enabled, AllowLocalFirewallRules, AllowLocalIPsecRules, DefaultInboundAction -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== MDM/Intune Firewall CSP policy (AllowLocalPolicyMerge = 0 means locked) ==' -ForegroundColor Cyan\r\n"
                                "Get-ChildItem 'HKLM:\\SOFTWARE\\Microsoft\\PolicyManager\\current\\device\\Firewall' -Recurse -ErrorAction SilentlyContinue |\r\n"
                                "    Get-ItemProperty |\r\n"
                                "    Select-Object PSChildName, AllowLocalPolicyMerge, AllowLocalIpsecPolicyMerge, EnableFirewall, DefaultInboundAction |\r\n"
                                "    Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Firewall rules for 1PhoneMirror ==' -ForegroundColor Cyan\r\n"
                                "Get-NetFirewallApplicationFilter |\r\n"
                                "    Where-Object Program -like '*1PhoneMirror*' |\r\n"
                                "    ForEach-Object {\r\n"
                                "        $r = Get-NetFirewallRule -AssociatedNetFirewallApplicationFilter $_\r\n"
                                "        [pscustomobject]@{\r\n"
                                "            Name      = $r.DisplayName\r\n"
                                "            Enabled   = $r.Enabled\r\n"
                                "            Direction = $r.Direction\r\n"
                                "            Action    = $r.Action\r\n"
                                "            Profile   = $r.Profile\r\n"
                                "            Program   = $_.Program\r\n"
                                "        }\r\n"
                                "    } | Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Listening TCP ports (AirPlay 7000/7001/7100, scrcpy 27183) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetTCPConnection -State Listen |\r\n"
                                "    Where-Object LocalPort -in 7000,7001,7100,27183 |\r\n"
                                "    Select-Object LocalAddress, LocalPort,\r\n"
                                "        @{n='Process';e={(Get-Process -Id $_.OwningProcess).ProcessName}} |\r\n"
                                "    Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Listening UDP ports (mDNS 5353, AirPlay 6000-6010) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetUDPEndpoint |\r\n"
                                "    Where-Object LocalPort -in 5353,6000,6001,6002,6003,6004,6005,6006,6007,6008,6009,6010 |\r\n"
                                "    Select-Object LocalAddress, LocalPort,\r\n"
                                "        @{n='Process';e={(Get-Process -Id $_.OwningProcess).ProcessName}} |\r\n"
                                "    Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Local IPv4 addresses (share with the phone) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetIPAddress -AddressFamily IPv4 |\r\n"
                                "    Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |\r\n"
                                "    Select-Object IPAddress, InterfaceAlias, PrefixOrigin |\r\n"
                                "    Format-Table -AutoSize\r\n";
                            SDL_SetClipboardText(ps_script);
                            toast_text_ = "PowerShell test script copied to clipboard";
                            toast_active_ = true;
                            toast_start_ = std::chrono::steady_clock::now();
                            std::cout << "[Renderer] Copied network troubleshooting script\n";
                            break;
                        }
                        // "Check for updates" button — manual GitHub poll.
                        if (info_check_btn_.w > 0 &&
                            !update_check_in_progress_.load() &&
                            in_rect(mx, my, info_check_btn_.x, info_check_btn_.y,
                                    info_check_btn_.w, info_check_btn_.h)) {
                            check_for_update_async(true);
                            break;
                        }
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
                if (android_help_dragging_ && android_help_track_rect_.h > 0 &&
                    android_help_thumb_rect_.h > 0 && android_help_max_scroll_ > 0) {
                    int my = event.motion.y;
                    int track_y = android_help_track_rect_.y;
                    int track_h = android_help_track_rect_.h;
                    int thumb_h = android_help_thumb_rect_.h;
                    float frac = (float)(my - android_help_drag_offset_ - track_y) /
                                 (float)std::max(1, track_h - thumb_h);
                    frac = std::max(0.0f, std::min(1.0f, frac));
                    android_help_scroll_ = (int)(frac * android_help_max_scroll_);
                } else if (log_scrollbar_dragging_ && log_sb_track_h_ > 0 && log_sb_max_scroll_ > 0) {
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
                    if (android_help_dragging_) android_help_dragging_ = false;
                }
                break;

            case SDL_MOUSEWHEEL:
                if (android_panel_visible_ && android_help_visible_) {
                    int scroll_amount = std::max(20, frame_dst_w_ / 18);
                    android_help_scroll_ -= event.wheel.y * scroll_amount;
                    if (android_help_scroll_ < 0) android_help_scroll_ = 0;
                    if (android_help_scroll_ > android_help_max_scroll_)
                        android_help_scroll_ = android_help_max_scroll_;
                } else if (version_panel_visible_) {
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
            pending_source_name_.clear();
            // Drop any in-flight frame that the source's worker thread
            // may have submitted just before its disconnect callback
            // fired. Without this, render_frame() would recreate the
            // texture from that stale frame and the user would see the
            // last image of the just-disconnected device hanging on the
            // waiting screen.
            {
                std::lock_guard lk(frame_mutex_);
                pending_frame_ = {};
                has_new_frame_ = false;
            }
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
                // Decide whether this texture change is a real device
                // rotation (portrait <-> landscape) or just a same-orientation
                // resolution switch / weird in-app canvas (e.g. TripIt
                // reallocates a 16:9 1920x1080 AirPlay surface even when the
                // phone is still held portrait — that is NOT a rotation).
                //
                // A genuine rotation swaps the previous width and height, so
                // the new aspect ratio should be ~ 1 / prev_aspect. If the
                // new aspect differs from that by more than ~15%, treat it
                // as a same-device canvas change and keep the existing
                // phone frame.
                bool first_real_texture = (tex_width_ == 0 || tex_height_ == 0);
                bool real_rotation = false;
                if (!first_real_texture) {
                    bool prev_portrait = (tex_height_ >= tex_width_);
                    bool new_portrait  = (pending_frame_.height >= pending_frame_.width);
                    if (prev_portrait != new_portrait) {
                        float prev_ar = (float)tex_width_ / (float)tex_height_;
                        float new_ar  = (float)pending_frame_.width / (float)pending_frame_.height;
                        float expected_ar = 1.0f / prev_ar; // rotated previous
                        if (expected_ar > 0.0f) {
                            float rel = std::abs(new_ar - expected_ar) / expected_ar;
                            real_rotation = (rel < 0.15f);
                        }
                    }
                }
                // User just clicked a different source dot \u2014 force the
                // canvas-change branch to be treated as a fresh device so
                // the phone frame is regenerated and the window is reshaped
                // for the new aspect ratio (Mac landscape \u2194 iPhone portrait).
                bool source_switched = source_just_switched_;
                source_just_switched_ = false;

                if (texture_) SDL_DestroyTexture(texture_);
                texture_ = SDL_CreateTexture(sdl_renderer_,
                    SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                    pending_frame_.width, pending_frame_.height);
                tex_width_ = pending_frame_.width;
                tex_height_ = pending_frame_.height;
                std::cout << "[Renderer] Texture resized: " << tex_width_ << "x" << tex_height_
                          << (real_rotation       ? " (rotation)" :
                              first_real_texture  ? " (initial)"  :
                              source_switched     ? " (source switch)" :
                                                    " (canvas change - keeping frame)")
                          << "\n";

                if (real_rotation || first_real_texture || source_switched) {
                    window_shape_set_ = false;
                    if (phone_frame_enabled_) {
                        phone_frame_.generate(sdl_renderer_, tex_width_, tex_height_);
                        int fw = phone_frame_.frame_width();
                        int fh = phone_frame_.frame_height();
                        int cur_w, cur_h;
                        SDL_GetWindowSize(window_, &cur_w, &cur_h);
                        int cur_log_w = (int)(log_panel_full_w_ * log_panel_anim_);

                        if (first_real_texture || source_switched) {
                            // Fresh device (first connect or user picked a
                            // different source) \u2014 use the same display-
                            // relative sizing as Ctrl+0 so the window fits
                            // the new source's aspect (e.g. iPhone portrait
                            // vs Mac landscape) without being squashed.
                            SDL_DisplayMode dm;
                            if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && fw > 0 && fh > 0) {
                                float s = std::min(dm.w * 0.32f / fw,
                                                   dm.h * 0.65f / fh);
                                int new_w = std::max(1, (int)std::round(fw * s));
                                int new_h = std::max(1, (int)std::round(fh * s));
                                SDL_SetWindowSize(window_, new_w + cur_log_w, new_h);
                            }
                        } else {
                            // Same device rotated \u2014 preserve the user's
                            // window placement AND the perceived size by
                            // keeping the SHORT side the same length, then
                            // derive the long side from the new aspect.
                            // Without this, rotating a 400x800 portrait
                            // would jump to 1600x800 landscape.
                            int phone_w   = std::max(1, cur_w - cur_log_w);
                            int prev_short = std::min(phone_w, cur_h);
                            int new_short  = std::min(fw, fh);
                            if (new_short <= 0) new_short = 1;
                            float s = (float)prev_short / (float)new_short;
                            int new_w = std::max(1, (int)std::round(fw * s));
                            int new_h = std::max(1, (int)std::round(fh * s));
                            SDL_SetWindowSize(window_, new_w + cur_log_w, new_h);
                        }
                    }
                }
                // Same-orientation resolution change: keep phone_frame_ as-is.
                // The existing window stays the same shape; the new texture
                // (possibly a different aspect ratio than the phone frame's
                // inner screen rect) will be rendered with letterboxing
                // inside that rect by the existing draw code.

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
            // First frame after a device switch — clear the "waiting for
            // <name>" overlay.
            pending_source_name_.clear();
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
        record_btn_ = {};
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
        // If the texture aspect ratio differs noticeably from the phone
        // screen rect (e.g. an app like TripIt sends a landscape 1920x1080
        // canvas while the phone frame is portrait), aspect-fit the texture
        // inside vdst with black bars so the content doesn't get squished.
        if (tex_width_ > 0 && tex_height_ > 0 && vdst.w > 0 && vdst.h > 0) {
            float tex_ar  = (float)tex_width_  / (float)tex_height_;
            float rect_ar = (float)vdst.w      / (float)vdst.h;
            if (std::abs(tex_ar - rect_ar) > 0.05f * rect_ar) {
                // Fill the phone screen rect with black first.
                SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 255);
                SDL_RenderFillRect(sdl_renderer_, &vdst);
                int fit_w, fit_h;
                if (tex_ar > rect_ar) {
                    // Texture wider than rect — fit by width, letterbox.
                    fit_w = vdst.w;
                    fit_h = (int)std::round(vdst.w / tex_ar);
                } else {
                    fit_h = vdst.h;
                    fit_w = (int)std::round(vdst.h * tex_ar);
                }
                SDL_Rect fit{vdst.x + (vdst.w - fit_w) / 2,
                             vdst.y + (vdst.h - fit_h) / 2,
                             fit_w, fit_h};
                SDL_RenderCopy(sdl_renderer_, texture_, nullptr, &fit);
            } else {
                SDL_RenderCopy(sdl_renderer_, texture_, nullptr, &vdst);
            }
        } else {
            SDL_RenderCopy(sdl_renderer_, texture_, nullptr, &vdst);
        }

        // Optional "Waiting for screen updates from <name>" overlay shown
        // briefly after a source switch and cleared on the next frame.
        if (!pending_source_name_.empty()) {
            std::string want = "Waiting for screen updates from " + pending_source_name_;
            if (want != pending_overlay_text_) {
                if (pending_overlay_tex_) SDL_DestroyTexture(pending_overlay_tex_);
                pending_overlay_tex_ = make_text_texture(sdl_renderer_, want.c_str(),
                    32, 235, 235, 235, &pending_overlay_w_, &pending_overlay_h_);
                pending_overlay_text_ = want;
            }
            // Frosty translucent belt spanning the screen width, centered
            // vertically behind the text. Simulated frosted-glass look:
            // a darker translucent fill with thin highlight lines on the
            // top and bottom edges.
            if (pending_overlay_tex_) {
                float ts = std::min(1.0f, svw * 0.78f / (float)pending_overlay_w_);
                int tw = (int)(pending_overlay_w_ * ts);
                int th = (int)(pending_overlay_h_ * ts);
                int pad = std::max(12, th / 2);
                int belt_h = th + pad * 2;
                int belt_y = svy + (svh - belt_h) / 2;
                SDL_Rect belt = {svx, belt_y, svw, belt_h};

                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                // Main translucent fill (cool dark tint).
                SDL_SetRenderDrawColor(sdl_renderer_, 18, 22, 28, 165);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                // Subtle inner lift for a frosted feel.
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 16);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                // Highlight edges (top & bottom) for crispness.
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 70);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y,
                                   belt.x + belt.w - 1, belt.y);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 1,
                                   belt.x + belt.w - 1, belt.y + belt.h - 1);
                SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 90);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + 1,
                                   belt.x + belt.w - 1, belt.y + 1);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 2,
                                   belt.x + belt.w - 1, belt.y + belt.h - 2);

                SDL_Rect tr = {svx + (svw - tw) / 2, belt_y + pad, tw, th};
                SDL_RenderCopy(sdl_renderer_, pending_overlay_tex_, nullptr, &tr);
            }
        }

        // Protected/blank-content detector. Sustained near-black frames
        // most commonly mean Android FLAG_SECURE (lock screen, banking
        // app, MDM-restricted document, DRM video). The framework gives
        // us no signal, so we wait ~1.2 s of unbroken black before
        // surfacing the overlay to avoid flickering on legitimate dark
        // transitions (app switcher fade-to-black, video letterbox, etc).
        if (is_frame_near_black()) {
            auto now = std::chrono::steady_clock::now();
            if (!black_frame_active_) {
                black_frame_active_ = true;
                black_frame_since_ = now;
            }
            auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - black_frame_since_).count();
            protected_overlay_visible_ = held_ms >= 1200;
        } else {
            black_frame_active_ = false;
            protected_overlay_visible_ = false;
        }

        if (protected_overlay_visible_ && pending_source_name_.empty()) {
            // Two lines, baked at a font size chosen to fit the current
            // screen width. Re-baked only when that target size changes,
            // so glyphs are always rendered 1:1 (no scaling = crisp) but
            // still fit even on small windows / narrow phone aspects.
            const char* line1 = "Screen content is hidden by the device.";
            const char* line2 = "Lock screen or protected content.";
            // Pick a font size that, given the longer line, leaves comfortable
            // side margins. Clamp to a sane absolute range.
            int target_px = std::clamp(svw / 22, 11, 48);
            static SDL_Texture* tex1 = nullptr;
            static SDL_Texture* tex2 = nullptr;
            static int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
            static int baked_px = 0;
            if (target_px != baked_px) {
                if (tex1) { SDL_DestroyTexture(tex1); tex1 = nullptr; }
                if (tex2) { SDL_DestroyTexture(tex2); tex2 = nullptr; }
                tex1 = make_text_texture(sdl_renderer_, line1, target_px,
                                         235, 235, 235, &w1, &h1);
                tex2 = make_text_texture(sdl_renderer_, line2, target_px,
                                         235, 235, 235, &w2, &h2);
                baked_px = target_px;
            }
            if (tex1 && tex2) {
                int gap = std::max(2, h1 / 6);
                int text_h = h1 + gap + h2;
                int pad = std::max(8, h1 / 2);
                int belt_h = text_h + pad * 2;
                int belt_y = svy + (svh - belt_h) / 2;
                SDL_Rect belt = {svx, belt_y, svw, belt_h};

                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdl_renderer_, 18, 22, 28, 200);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 16);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 70);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y,
                                   belt.x + belt.w - 1, belt.y);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 1,
                                   belt.x + belt.w - 1, belt.y + belt.h - 1);
                SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 90);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + 1,
                                   belt.x + belt.w - 1, belt.y + 1);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 2,
                                   belt.x + belt.w - 1, belt.y + belt.h - 2);

                int ty = belt_y + pad;
                SDL_Rect tr1 = {svx + (svw - w1) / 2, ty, w1, h1};
                SDL_RenderCopy(sdl_renderer_, tex1, nullptr, &tr1);
                SDL_Rect tr2 = {svx + (svw - w2) / 2, ty + h1 + gap, w2, h2};
                SDL_RenderCopy(sdl_renderer_, tex2, nullptr, &tr2);
            }
        }
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

    // Recording HUD — countdown digit while waiting, "REC + elapsed" chip
    // while recording. Drawn on the screen content (under phone bezel) but
    // above the video itself.
    if (record_countdown_ms_ > 0 && !recorder_.is_recording() && tex_width_ > 0) {
        auto cd_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_countdown_start_).count();
        int remaining = std::max(0, (record_countdown_ms_ - (int)cd_elapsed + 999) / 1000);
        char buf[8]; std::snprintf(buf, sizeof(buf), "%d", remaining);
        int big = std::max(64, frame_dst_w_ / 4);
        int tw = 0, th = 0;
        SDL_Texture* t = make_text_texture(sdl_renderer_, buf, big,
                                           255, 255, 255, &tw, &th);
        if (t) {
            // Pulse pop on each second tick.
            int phase_ms = (int)(cd_elapsed % 1000);
            float pop = 1.0f + 0.15f * std::max(0.0f, 1.0f - phase_ms / 250.0f);
            int dw = (int)(tw * pop), dh = (int)(th * pop);
            int cx = frame_dst_x_ + frame_dst_w_ / 2;
            int cy = frame_dst_y_ + frame_dst_h_ / 2;
            // Frosty circular backdrop — partly transparent so the
            // mirrored content remains visible behind the digit. Soft
            // outer halo + main translucent disc + faint inner highlight.
            int radius = (int)(std::max(dw, dh) * 0.85f);
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
            for (int rr = radius + radius / 4; rr > radius; --rr) {
                uint8_t a = (uint8_t)(20 * (radius + radius / 4 - rr) / std::max(1, radius / 4));
                SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 30, a);
                fill_circle(sdl_renderer_, cx, cy, rr);
            }
            // Main frosted disc — alpha ~95/255 keeps the underlying
            // mirror legible while still floating the digit visually.
            SDL_SetRenderDrawColor(sdl_renderer_, 28, 30, 40, 95);
            fill_circle(sdl_renderer_, cx, cy, radius);
            // Subtle inner highlight ring for a glassy edge.
            int ring_r = radius - std::max(2, radius / 18);
            SDL_SetRenderDrawColor(sdl_renderer_, 200, 210, 230, 45);
            for (int rr = ring_r; rr >= ring_r - 1 && rr > 0; --rr) {
                for (int dy = -rr; dy <= rr; ++dy) {
                    for (int dx = -rr; dx <= rr; ++dx) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 <= rr * rr && d2 >= (rr - 1) * (rr - 1))
                            SDL_RenderDrawPoint(sdl_renderer_, cx + dx, cy + dy);
                    }
                }
            }
            SDL_Rect dst{cx - dw / 2, cy - dh / 2, dw, dh};
            SDL_RenderCopy(sdl_renderer_, t, nullptr, &dst);
            SDL_DestroyTexture(t);
        }
    } else if (recorder_.is_recording() && tex_width_ > 0) {
        int s = (int)recorder_.elapsed_seconds();
        char buf[64]; std::snprintf(buf, sizeof(buf), "REC  %d:%02d", s / 60, s % 60);
        int fh = std::max(14, frame_dst_w_ / 28);
        int tw = 0, th = 0;
        SDL_Texture* t = make_text_texture(sdl_renderer_, buf, fh,
                                           255, 240, 240, &tw, &th);
        if (t) {
            int pad_ = std::max(6, fh / 2);
            int chip_w = tw + pad_ * 2 + fh; // extra room for the dot
            int chip_h = th + pad_;
            int chip_x = frame_dst_x_ + std::max(8, frame_dst_w_ / 24);
            int chip_y = frame_dst_y_ + std::max(8, frame_dst_h_ / 28);
            // Pulsing red background.
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            float pulse = 0.55f + 0.45f * (float)std::abs(std::sin(ms * 0.005));
            SDL_SetRenderDrawColor(sdl_renderer_,
                                   (uint8_t)(180 * pulse + 50), 30, 30, 220);
            SDL_Rect chip{chip_x, chip_y, chip_w, chip_h};
            SDL_RenderFillRect(sdl_renderer_, &chip);
            // Filled dot to the left of the text.
            int dot_r = th / 3;
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 230);
            fill_circle(sdl_renderer_, chip_x + pad_ + dot_r,
                        chip_y + chip_h / 2, dot_r);
            SDL_Rect td{chip_x + pad_ + dot_r * 2 + pad_ / 2,
                        chip_y + (chip_h - th) / 2, tw, th};
            SDL_RenderCopy(sdl_renderer_, t, nullptr, &td);
            SDL_DestroyTexture(t);
        }
    }

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
        // Glyph size derived from the phone-equivalent reference width
        // (capped) so the menu chevron stays compact regardless of source
        // dimensions — see ui_ref_width() for the rationale.
        int dot_r = std::max(2, ui_ref_width() / 175);
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
    bezel_record_btn_ = {};
    if (island_anim_ < 0.5f) {
        float scale_b = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_top = (int)(phone_frame_.screen_y() * scale_b);
        int svx_b = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale_b);
        int svw_b = (int)(phone_frame_.screen_width() * scale_b);
        // Mirror the maths inside draw_island() so the X position is identical.
        int phone_eq_w = ui_ref_width();
        int btn_sz_full = std::max(20, phone_eq_w / 14);
        int pad_full = btn_sz_full / 3;
        int gap_full = pad_full / 2 + 2;
        int island_w = std::max(160, (int)(phone_eq_w * 0.80f));
        int island_x = svx_b + (svw_b - island_w) / 2;
        // Order on the island (right→left): close, folder, record, screenshot.
        int ss_full_x = island_x + island_w - pad_full - btn_sz_full
                       - 3 * (btn_sz_full + gap_full);
        int ss_full_cx = ss_full_x + btn_sz_full / 2;
        int rec_full_x = island_x + island_w - pad_full - btn_sz_full
                        - 2 * (btn_sz_full + gap_full);
        int rec_full_cx = rec_full_x + btn_sz_full / 2;

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
        // White filled-dot glyph — matches the island screenshot button so
        // the affordance reads identically whether the menu is up or down.
        int cr = std::max(2, mini_sz / 4);
        int ccx = mini_x + mini_sz / 2;
        int ccy = mini_y + mini_sz / 2;
        uint8_t ca = flashing_b ? 255 : (bhov ? 255 : 220);
        SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, ca);
        fill_circle(sdl_renderer_, ccx, ccy, cr);

        if (bhov) {
            bezel_hover_key = "bezel_ss";
            bezel_hover_text = "Screenshot (Ctrl+S)";
            bezel_hover_ax = ccx;
            bezel_hover_ay = mini_y + mini_sz + 4;
        }

        // Bezel record button — same horizontal position as the island
        // record button, same red filled-dot / square language.
        int rec_mini_x = rec_full_cx - mini_sz / 2;
        int rec_mini_y = mini_y;
        bezel_record_btn_ = {rec_mini_x, rec_mini_y, mini_sz, mini_sz};
        bool rhov = in_rect(bmx, bmy, rec_mini_x, rec_mini_y, mini_sz, mini_sz);
        bool rec_active_b = recorder_.is_recording();
        bool rec_count_b  = (record_countdown_ms_ > 0 && !rec_active_b);
        float rec_pulse_b = 1.0f;
        if (rec_active_b) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_b.time_since_epoch()).count();
            rec_pulse_b = 0.55f + 0.45f * (float)std::abs(std::sin(ms * 0.005));
        }
        uint8_t rbg_r = rec_active_b ? (uint8_t)(110 * rec_pulse_b + 60)
                                     : (rhov ? 80 : 40);
        uint8_t rbg_g = rhov ? 35 : 35;
        uint8_t rbg_b = rhov ? 35 : 40;
        SDL_SetRenderDrawColor(sdl_renderer_, rbg_r, rbg_g, rbg_b,
                               rhov || rec_active_b ? 200 : 130);
        fill_circle(sdl_renderer_, rec_mini_x + mini_sz / 2,
                    rec_mini_y + mini_sz / 2, mini_sz / 2);
        int rrr = std::max(2, mini_sz / 4);
        int rrcx = rec_mini_x + mini_sz / 2;
        int rrcy = rec_mini_y + mini_sz / 2;
        uint8_t ra = rec_active_b ? 255 : (rhov ? 240 : 210);
        SDL_SetRenderDrawColor(sdl_renderer_, 235, 80, 80, ra);
        if (rec_active_b) {
            SDL_Rect sqr{rrcx - rrr, rrcy - rrr, rrr * 2, rrr * 2};
            SDL_RenderFillRect(sdl_renderer_, &sqr);
        } else {
            fill_circle(sdl_renderer_, rrcx, rrcy, rrr);
            if (rec_count_b) {
                int ring = rrr + 2;
                SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, 220);
                for (int dy = -ring; dy <= ring; dy++) {
                    for (int dx = -ring; dx <= ring; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 <= ring * ring && d2 >= (ring - 1) * (ring - 1))
                            SDL_RenderDrawPoint(sdl_renderer_, rrcx + dx, rrcy + dy);
                    }
                }
            }
        }
        if (rhov) {
            bezel_hover_key = "bezel_rec";
            if (rec_active_b) {
                int s = (int)recorder_.elapsed_seconds();
                static char rb[64];
                std::snprintf(rb, sizeof(rb), "Stop recording - %d:%02d (Ctrl+R)", s / 60, s % 60);
                bezel_hover_text = rb;
            } else if (rec_count_b) {
                bezel_hover_text = "Cancel countdown";
            } else {
                bezel_hover_text = "Record (Ctrl+R)\nRight-click for delay/timed";
            }
            bezel_hover_ax = rrcx;
            bezel_hover_ay = rec_mini_y + mini_sz + 4;
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
        int dot_r_close = std::max(2, ui_ref_width() / 175);
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
            if (elapsed_t < toast_duration_ms_) {
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
                    else if (elapsed_t > toast_duration_ms_ - 300)
                        alpha = (uint8_t)((toast_duration_ms_ - elapsed_t) * 255 / 300);
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

    // Persistent "Update available" banner. Sits at the top of the phone
    // frame and is independent of the island/bezel mini-button visibility
    // (the previous placement was inside `if (island_anim_ < 0.5f)`, which
    // suppressed the banner whenever the menu was open — i.e. the default
    // first-run state). Draws below the toast pill when both are visible.
    draw_update_banner();

    // Log star — center right bezel
    {
        float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_right = frame_dst_w_ - (int)((phone_frame_.screen_x() + phone_frame_.screen_width()) * scale);
        // Glyph size derived from the phone-equivalent reference width so
        // the log chevron stays compact on tablet/desktop sources.
        int dot_r = std::max(2, ui_ref_width() / 175);
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
            // Source-dot size derived from ui_ref_width() so the picker
            // stays compact (and dots stay close together) on large sources.
            int dot_r = std::max(3, ui_ref_width() / 120);
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

                // Active source: bright white, full size. Inactive: dimmed
                // and slightly smaller. No background rim — the contrast
                // alone disambiguates which device is currently shown.
                uint8_t a;
                if (sources[i].active)         a = 255;
                else if (hover)                a = 200;
                else if (sources[i].streaming) a = 110;
                else                           a = 70;
                if (sources[i].active)
                    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 255);
                else
                    SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, a);

                int icon_r = dot_r + (sources[i].active ? 2 : 1);

                if (sources[i].paused) {
                    // Pause icon — two vertical bars
                    int bw = std::max(2, (icon_r * 2) / 3);
                    int bh = icon_r * 2 + 1;
                    int gap = std::max(2, icon_r);
                    SDL_Rect lb{cx - gap / 2 - bw, cy - bh / 2, bw, bh};
                    SDL_Rect rb{cx + gap / 2,      cy - bh / 2, bw, bh};
                    SDL_RenderFillRect(sdl_renderer_, &lb);
                    SDL_RenderFillRect(sdl_renderer_, &rb);
                } else {
                    // Play icon — right-pointing triangle
                    fill_play(cx, cy, icon_r, icon_r);
                }

                source_btns_.emplace_back(sources[i].id, r);
                if (hover) {
                    bezel_hover_key = "src:" + sources[i].id;
                    std::string state = sources[i].paused ? " (paused)"
                                       : sources[i].streaming ? " (live)"
                                       : "";
                    std::string shortcut;
                    if (i < 9) shortcut = "  (Ctrl+" + std::to_string((int)i + 1) + ")";
                    bezel_hover_text = sources[i].name + shortcut + state +
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
        // Dot size derived from ui_ref_width() so the grip stays the same
        // visual weight whether mirroring a phone or a tablet/desktop. The
        // arc itself still follows the phone frame's actual corner radius.
        int dot_r = std::max(2, ui_ref_width() / 175);
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
            bezel_hover_text = "Drag to resize\nRight-click for menu";
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
        } else if (bezel_menu_target_ == "record") {
            const bool active = recorder_.is_recording();
            const bool counting = (record_countdown_ms_ > 0 && !active);
            if (active) {
                items.push_back({"stop", "Stop recording (Ctrl+R)"});
            } else if (counting) {
                items.push_back({"cancel", "Cancel countdown"});
            } else {
                items.push_back({"start",   "Start now (Ctrl+R)"});
                items.push_back({"delay5",  "Start in 5 s"});
                items.push_back({"timed5",  "Record 5 s"});
                items.push_back({"timed10", "Record 10 s"});
                items.push_back({"timed15", "Record 15 s"});
            }
        } else if (bezel_menu_target_ == "resize") {
            items.push_back({"reset_size", "Reset to default size (Ctrl+0)"});
        } else if (bezel_menu_target_ == "screenshot") {
            items.push_back({"shot",     "Take screenshot (Ctrl+S)"});
            items.push_back({"annotate", "Annotate screenshot (Ctrl+Shift+S)"});
#ifdef _WIN32
            items.push_back({"ocr",      "OCR copy text (Ctrl+Shift+T)"});
#endif
            items.push_back({"open_dir", "Open screenshot folder"});
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

            // Use the shared phone-equivalent UI reference width so the
            // right-click popup stays the same neat size regardless of
            // whether the source is a phone, tablet or desktop.
            int eq_w = ui_ref_width();
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
            } else if (bezel_menu_target_ == "record") {
                // Record button lives in the top island — slide DOWN from
                // the cursor so the menu doesn't crash into the window top.
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ + 12;
                dy = -(int)(panel_h * 0.3f);
            } else if (bezel_menu_target_ == "screenshot") {
                // Screenshot button is also in the top island — match the
                // record menu and drop the popup below the cursor.
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ + 12;
                dy = -(int)(panel_h * 0.3f);
            } else if (bezel_menu_target_ == "resize") {
                // Resize grip lives in the bottom-right corner — open the
                // menu to the LEFT of the grip so it stays inside the
                // phone-frame area (and doesn't collide with the log panel
                // when that is open on the right).
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

    // Screenshot annotator (drawn above PIN so the modal editor wins).
    if (annotator_active_) {
        draw_annotator();
    }
#ifdef _WIN32
    if (ocr_active_) {
        draw_ocr_overlay();
    }
    process_ocr_result();
#endif

    SDL_RenderPresent(sdl_renderer_);

    if (screenshot_requested_) {
        screenshot_requested_ = false;
        take_screenshot();
    }

    // ---- Recording lifecycle (after the present so we feed encoded frames
    // immediately and any UI changes happen on the next loop) ----
    if (record_toggle_requested_) {
        record_toggle_requested_ = false;
        if (recorder_.is_recording()) {
            stop_recording();
        } else if (record_countdown_ms_ > 0) {
            record_countdown_start_ = std::chrono::steady_clock::now();
        } else {
            start_recording();
        }
    }

    // Countdown -> start
    if (record_countdown_ms_ > 0 && !recorder_.is_recording()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_countdown_start_).count();
        if (elapsed >= record_countdown_ms_) {
            record_countdown_ms_ = 0;
            start_recording();
        }
    }

    // Push the latest composited frame to the recorder.
    if (recorder_.is_recording() && !last_frame_data_.empty()) {
        // When the phone bezel is on, encode the same composite the user sees
        // (rounded corners, dynamic island, bottom bar). MP4/GIF cannot carry
        // alpha, so the area outside the rounded bezel ends up black — which
        // reads as an intentional matte around the phone.
        if (phone_frame_enabled_ && phone_frame_.is_generated()) {
            int cw = 0, ch = 0;
            uint8_t* composite = phone_frame_.composite_screenshot(
                sdl_renderer_,
                last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
                &cw, &ch);
            if (composite) {
                recorder_.push_frame(composite, cw, ch, cw * 4);
                delete[] composite;
            } else {
                recorder_.push_frame(last_frame_data_.data(), last_frame_w_,
                                     last_frame_h_, last_frame_stride_);
            }
        } else {
            recorder_.push_frame(last_frame_data_.data(), last_frame_w_,
                                 last_frame_h_, last_frame_stride_);
        }
    }

    // Recorder may have hit max-duration on the worker — finalise from the
    // renderer thread so we own the toast/UI updates.
    if (recorder_.should_finalize()) {
        stop_recording();
    }
}

// Returns a "phone-equivalent" reference width for sizing overlay UI.
// Panels, popup menus, tooltips, swatches and the connect/help dialogs
// previously sized everything from `svw` (the on-screen phone screen
// width). When the user mirrors a tablet or desktop, `svw` becomes huge
// and every UI element bloats out of proportion. This helper folds the
// source down to a phone-equivalent width (taking the narrower of the
// real viewport width and `svh / 2`, which is the natural width of a
// 2:1 phone aspect) and clamps to an absolute ceiling so overlays stay
// compact even on very large windows. For phone-shaped sources this
// returns ~svw and behaviour is unchanged.
int Renderer::ui_ref_width() const {
    if (frame_dst_w_ == 0) return 0;
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svw = (int)(phone_frame_.screen_width()  * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);
    int phone_eq = (std::min)(svw, svh / 2);
    constexpr int kAbsCap = 480;
    return (std::min)(phone_eq, kAbsCap);
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

#ifdef _WIN32
// Persistent two-line "Update available" banner. Pill background mirrors
// the standalone screenshot toast, but stacks two lines and exposes a
// clickable GitHub link on line 2 (with the same hover-underline + 1 s
// tooltip styling as the footer links). A small "x" in the upper-right
// dismisses it.
void Renderer::draw_update_banner() {
    if (!update_banner_active_) return;
    if (frame_dst_w_ == 0) return;

    // Resolve the version string under the lock and rebuild line-1 if it
    // changed (or on first show).
    std::string ver;
    {
        std::lock_guard<std::mutex> lk(update_check_mutex_);
        ver = update_latest_version_;
    }
    std::string line1 = "Update available: v" + ver;
    static const std::string kLinkLabel = "github.com/MSEndpointMgr/1PhoneMirror";

    if (line1 != update_line1_cached_) {
        if (update_line1_tex_) { SDL_DestroyTexture(update_line1_tex_); update_line1_tex_ = nullptr; }
        if (update_link_tex_)  { SDL_DestroyTexture(update_link_tex_);  update_link_tex_  = nullptr; }
        update_line1_cached_ = line1;
    }

    // Render textures lazily at a font size matched to the standalone
    // toast pill so visual weight stays consistent with sibling overlays.
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int btn_sz_full = std::max(20, ui_ref_width() / 14);
    int fh_text = std::max(28, btn_sz_full * 2);

    if (!update_line1_tex_) {
        update_line1_tex_ = make_text_texture(sdl_renderer_, line1, fh_text,
                                              230, 230, 230,
                                              &update_line1_w_, &update_line1_h_);
    }
    if (!update_link_tex_) {
        // Slight blue tint so the link reads as interactive even at rest,
        // matching the convention used by the footer links.
        update_link_tex_ = make_text_texture(sdl_renderer_, kLinkLabel, fh_text,
                                             150, 190, 240,
                                             &update_link_w_, &update_link_h_);
    }
    if (!update_line1_tex_ || !update_link_tex_) return;

    // Display scale: same recipe as the standalone toast, so font size
    // matches what the user sees on the screenshot/clipboard pill.
    float t_scale = (float)std::max(11, btn_sz_full / 2)
                  / std::max(28, btn_sz_full * 2);
    int l1_w = (int)(update_line1_w_ * t_scale);
    int l1_h = (int)(update_line1_h_ * t_scale);
    int l2_w = (int)(update_link_w_  * t_scale);
    int l2_h = (int)(update_link_h_  * t_scale);

    int line_gap = std::max(2, l1_h / 6);
    int pillx_pad = std::max(8, btn_sz_full / 3);
    int pilly_pad = std::max(6, pillx_pad / 2);
    // Reserve room for a small close glyph on the right.
    int close_sz  = std::max(10, l1_h);
    int close_gap = std::max(6, pillx_pad / 2);

    int content_w = std::max(l1_w, l2_w);
    int pill_w = content_w + pillx_pad * 2 + close_sz + close_gap;
    int pill_h = l1_h + line_gap + l2_h + pilly_pad * 2;

    // Center horizontally over the screen viewport, anchor near the top
    // bezel just like the standalone toast. If the toast is also up,
    // nudge below it so they don't overlap.
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int bezel_top = svy - frame_dst_y_;
    int pill_x = svx + (svw - pill_w) / 2;
    int pill_y = frame_dst_y_ + bezel_top + std::max(6, btn_sz_full / 4);
    if (toast_active_) {
        // Drop below a typical single-line toast so they stack cleanly.
        pill_y += l1_h + pilly_pad * 2 + std::max(4, line_gap);
    }

    // Soft slide-in over the first 200 ms.
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - update_banner_start_).count();
    float in_t = std::min(1.0f, (float)elapsed_ms / 200.0f);
    float eased = 1.0f - (1.0f - in_t) * (1.0f - in_t) * (1.0f - in_t);
    int slide = (int)((1.0f - eased) * pill_h * 0.4f);
    pill_y -= slide;
    uint8_t alpha = (uint8_t)(255 * eased);

    int pr = pill_h / 2;
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 22, (uint8_t)(alpha * 220 / 255));
    SDL_Rect body = {pill_x + pr, pill_y, pill_w - pr * 2, pill_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    fill_circle(sdl_renderer_, pill_x + pr,         pill_y + pr, pr);
    fill_circle(sdl_renderer_, pill_x + pill_w - pr, pill_y + pr, pr);

    // Line 1 — plain text, centered against the content (close glyph
    // sits to the right of the content area).
    int content_x = pill_x + pillx_pad;
    int l1_x = content_x + (content_w - l1_w) / 2;
    int l1_y = pill_y + pilly_pad;
    SDL_SetTextureAlphaMod(update_line1_tex_, alpha);
    SDL_Rect l1_dst = {l1_x, l1_y, l1_w, l1_h};
    SDL_RenderCopy(sdl_renderer_, update_line1_tex_, nullptr, &l1_dst);

    // Line 2 — the GitHub link. Hit-rect a bit larger than the glyph
    // bounds so it's easy to click.
    int l2_x = content_x + (content_w - l2_w) / 2;
    int l2_y = l1_y + l1_h + line_gap;
    update_link_rect_ = {l2_x - 4, l2_y - 2, l2_w + 8, l2_h + 4};

    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    bool link_hover = in_rect(mx, my, update_link_rect_.x, update_link_rect_.y,
                              update_link_rect_.w, update_link_rect_.h);
    // Brighten on hover; baseline tint already reads as a link.
    if (link_hover) {
        SDL_SetTextureColorMod(update_link_tex_, 200, 220, 255);
    } else {
        SDL_SetTextureColorMod(update_link_tex_, 255, 255, 255);
    }
    SDL_SetTextureAlphaMod(update_link_tex_, alpha);
    SDL_Rect l2_dst = {l2_x, l2_y, l2_w, l2_h};
    SDL_RenderCopy(sdl_renderer_, update_link_tex_, nullptr, &l2_dst);

    // Hover underline — same affordance the footer links use.
    if (link_hover) {
        SDL_SetRenderDrawColor(sdl_renderer_, 200, 220, 255, alpha);
        int uy = l2_y + l2_h - 1;
        SDL_RenderDrawLine(sdl_renderer_, l2_x, uy, l2_x + l2_w, uy);
        // Hand cursor.
        static SDL_Cursor* hand = nullptr;
        if (!hand) hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
        if (hand) SDL_SetCursor(hand);
    }

    // Close glyph (small "x") in the upper-right of the pill.
    int close_x = pill_x + pill_w - pillx_pad - close_sz;
    int close_y = pill_y + pilly_pad - 1;
    update_close_rect_ = {close_x - 4, close_y - 4, close_sz + 8, close_sz + 8};
    bool close_hover = in_rect(mx, my, update_close_rect_.x, update_close_rect_.y,
                               update_close_rect_.w, update_close_rect_.h);
    uint8_t cr = close_hover ? 235 : 180;
    uint8_t cg = close_hover ? 110 : 180;
    uint8_t cb = close_hover ? 110 : 185;
    SDL_SetRenderDrawColor(sdl_renderer_, cr, cg, cb, alpha);
    int t = std::max(1, close_sz / 8);
    auto draw_x = [&](int cx0, int cy0, int half) {
        for (int dx = -t / 2; dx <= t / 2; ++dx)
            for (int dy = -t / 2; dy <= t / 2; ++dy) {
                SDL_RenderDrawLine(sdl_renderer_,
                                   cx0 - half + dx, cy0 - half + dy,
                                   cx0 + half + dx, cy0 + half + dy);
                SDL_RenderDrawLine(sdl_renderer_,
                                   cx0 - half + dx, cy0 + half + dy,
                                   cx0 + half + dx, cy0 - half + dy);
            }
    };
    draw_x(close_x + close_sz / 2, close_y + close_sz / 2, close_sz / 2 - 1);

    // Hover tooltips — drive the shared 1 s delay directly via
    // tooltip_ready() and draw_bezel_tooltip(), since this helper runs
    // outside the bezel render block where bezel_hover_* locals live.
    if (link_hover) {
        if (tooltip_ready("bezel_update_link")) {
            draw_bezel_tooltip("Open release on GitHub",
                               update_link_rect_.x + update_link_rect_.w / 2,
                               update_link_rect_.y + update_link_rect_.h + 4,
                               /*prefer_below=*/true);
        }
    } else if (close_hover) {
        if (tooltip_ready("bezel_update_close")) {
            draw_bezel_tooltip("Dismiss",
                               update_close_rect_.x + update_close_rect_.w / 2,
                               update_close_rect_.y + update_close_rect_.h + 4,
                               /*prefer_below=*/true);
        }
    }

    SDL_SetTextureAlphaMod(update_line1_tex_, 255);
    SDL_SetTextureColorMod(update_link_tex_, 255, 255, 255);
    SDL_SetTextureAlphaMod(update_link_tex_, 255);
}
#else
void Renderer::draw_update_banner() {}
#endif

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

    // Use the shared phone-equivalent UI reference width so the tooltip
    // stays the same neat size regardless of source dimensions.
    int eq_w = ui_ref_width();
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

    // Use the shared phone-equivalent reference (capped) so the island
    // stays the same neat size regardless of source device — same recipe
    // applied to the info/version/settings panels and bezel popups.
    int phone_eq_w = ui_ref_width();
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
        if (elapsed < toast_duration_ms_) {
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

    // Record button (to the right of the photo button — sits between
    // folder and screenshot in the right-to-left cluster).
    bx -= btn_sz + gap;
    record_btn_ = {bx, by, btn_sz, btn_sz};
    bool rec_hover = in_rect(mx, my, bx, by, btn_sz, btn_sz);
    bool rec_active = recorder_.is_recording();
    bool rec_count  = (record_countdown_ms_ > 0 && !rec_active);
    // Pulsing red while recording (200 ms cosine breathing).
    float rec_pulse = 1.0f;
    if (rec_active) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - btn_flash_start_).count(); // any monotonic reference
        rec_pulse = 0.55f + 0.45f * (float)std::abs(std::sin(ms * 0.005));
    }
    uint8_t rec_bg_r = rec_active ? (uint8_t)(110 * rec_pulse + 60) : (rec_hover ? 80 : 55);
    uint8_t rec_bg_g = rec_hover ? 35 : 35;
    uint8_t rec_bg_b = rec_hover ? 35 : 35;
    SDL_SetRenderDrawColor(sdl_renderer_, rec_bg_r, rec_bg_g, rec_bg_b, 180);
    fill_circle(sdl_renderer_, bx + btn_sz / 2, by + btn_sz / 2, btn_sz / 2);
    // Glyph: filled red circle when idle, red square when recording, red
    // circle with thin ring when counting down.
    int recr = btn_sz / 4;
    int rcx = bx + btn_sz / 2, rcy = by + btn_sz / 2;
    uint8_t rdot_a = rec_active ? 255 : (rec_hover ? 240 : 210);
    if (rec_active) {
        // Square indicator
        int sq = recr;
        SDL_Rect sqr = {rcx - sq, rcy - sq, sq * 2, sq * 2};
        SDL_SetRenderDrawColor(sdl_renderer_, 235, 80, 80, rdot_a);
        SDL_RenderFillRect(sdl_renderer_, &sqr);
    } else {
        SDL_SetRenderDrawColor(sdl_renderer_, 235, 80, 80, rdot_a);
        fill_circle(sdl_renderer_, rcx, rcy, recr);
        if (rec_count) {
            // Thin white ring around the dot during countdown.
            int ring = recr + 3;
            SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, 220);
            for (int dy = -ring; dy <= ring; dy++) {
                for (int dx = -ring; dx <= ring; dx++) {
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= ring * ring && d2 >= (ring - 1) * (ring - 1))
                        SDL_RenderDrawPoint(sdl_renderer_, rcx + dx, rcy + dy);
                }
            }
        }
    }

    // Screenshot button — filled white dot, mirroring the record button's
    // visual language so the two camera/record affordances read as a pair.
    bx -= btn_sz + gap;
    screenshot_btn_ = {bx, by, btn_sz, btn_sz};
    bool ss_hover = in_rect(mx, my, bx, by, btn_sz, btn_sz);
    uint8_t ss_bg = flashing ? 200 : (ss_hover ? 70 : 50);
    SDL_SetRenderDrawColor(sdl_renderer_, ss_bg, ss_bg, ss_bg + 5, 180);
    fill_circle(sdl_renderer_, bx + btn_sz / 2, by + btn_sz / 2, btn_sz / 2);
    int ssr = btn_sz / 4;
    int sscx = bx + btn_sz / 2, sscy = by + btn_sz / 2;
    uint8_t ss_dot_a = flashing ? 255 : (ss_hover ? 255 : 220);
    SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, ss_dot_a);
    fill_circle(sdl_renderer_, sscx, sscy, ssr);

    // Settings (gear) button is now drawn next to the icon on the LEFT side
    // (see block above). The right-cluster ends here with the screenshot button.

    // Hover detection (0=close, 1=screenshot, 2=folder, 3=icon)
    int new_hover = -1;
    if (close_hover) new_hover = 0;
    else if (ss_hover) new_hover = 1;
    else if (folder_hover) new_hover = 2;
    else if (icon_hover) new_hover = 3;
    else if (gear_hover) new_hover = 6;
    else if (rec_hover) new_hover = 7;

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
            case 7: {
                static std::string rec_tip;
                if (recorder_.is_recording()) {
                    int s = (int)recorder_.elapsed_seconds();
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Stop recording - %d:%02d (Ctrl+R)\nRight-click for delay/timed", s / 60, s % 60);
                    rec_tip = buf;
                } else if (record_countdown_ms_ > 0) {
                    rec_tip = "Cancel countdown";
                } else {
                    rec_tip = "Record (Ctrl+R)\nRight-click for delay/timed";
                }
                tip = rec_tip.c_str();
                break;
            }
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
            else if (elapsed > toast_duration_ms_ - 300)
                alpha = (uint8_t)((toast_duration_ms_ - elapsed) * 255 / 300);
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

    // All three footer lines render at the same (slightly smaller) size
    // so MSEndpointMgr matches Simon visually.
    float sf2 = sf * 0.85f;

    int total_w1 = line_w(footer_line1_, sf2);
    int total_w2 = line_w(footer_line2_, sf2);
    int total_w3 = line_w(footer_line3_, sf2);
    int line_h  = footer_line1_.empty() ? 0 : (int)(footer_line1_[0].h * sf2);
    int line_h2 = footer_line2_.empty() ? 0 : (int)(footer_line2_[0].h * sf2);
    int line_h3 = footer_line3_.empty() ? 0 : (int)(footer_line3_[0].h * sf2);
    int gap = std::max(2, line_h / 4);

    // Position near bottom of screen area
    int footer_y = svy + svh - line_h - line_h2
                 - (line_h3 > 0 ? (gap + line_h3) : 0)
                 - gap - std::max(6, svh / 25);

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
        int sw = (int)(s.w * sf2), sh = (int)(s.h * sf2);
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

    // Line 3 (version on its own line)
    if (!footer_line3_.empty()) {
        int y3 = y2 + line_h2 + gap;
        int x3 = svx + (svw - total_w3) / 2;
        for (auto& s : footer_line3_) {
            int sw = (int)(s.w * sf2), sh = (int)(s.h * sf2);
            SDL_Rect dst = {x3, y3, sw, sh};
            if (s.tex) SDL_RenderCopy(sdl_renderer_, s.tex, nullptr, &dst);
            if (!s.url.empty() || !s.tooltip.empty())
                footer_hits_.push_back({x3, y3, sw, sh, s.url, s.tooltip});
            x3 += sw;
        }
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

    // UI dimensions are derived from a phone-equivalent reference width so
    // the Info panel stays the same neat size whether the source is a
    // phone, an iPad, or a Mac desktop. Centering still uses svw.
    int uw = ui_ref_width();
    int pad = std::max(8, uw / 20);
    int line_gap = std::max(3, pad / 4);
    int spacer_h = std::max(4, pad / 2);

    // Scale text to fit panel, never below a readable minimum.
    float max_text_w = uw * 0.7f;
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
    // Reserve space for the "Copy network test script" button beneath the
    // text lines: spacer + button + bottom pad. Sized to match the
    // network-requirement lines so it doesn't dominate the panel.
    int btn_label_h = std::max(14, (int)(30 * text_scale + 0.5f));
    int btn_h = btn_label_h + std::max(4, pad / 5);
    // Hover caption is a smaller line that appears under the button on hover
    // — reserve its slot up-front so the panel doesn't reflow when shown.
    int hint_label_h = std::max(11, (int)(22 * text_scale + 0.5f));
    total_h += spacer_h + btn_h + line_gap + hint_label_h;

    // Reserve space for the footer block (MSEndpointMgr / Simon / coffee
    // links + version + Check-for-updates button) at the bottom of the
    // Info panel. Uses dedicated info_footer_* textures baked at the
    // same font size as the network-requirement lines, scaled by
    // text_scale so they share the visual weight of the surrounding
    // body text.
    auto info_footer_line_w = [&](const std::vector<FooterSeg>& line) {
        int total = 0;
        for (auto& s : line) total += (int)(s.w * text_scale);
        return total;
    };
    int foot_h1 = info_footer_line1_.empty() ? 0 : (int)(info_footer_line1_[0].h * text_scale);
    int foot_h2 = info_footer_line2_.empty() ? 0 : (int)(info_footer_line2_[0].h * text_scale);
    int foot_h3 = info_footer_line3_.empty() ? 0 : (int)(info_footer_line3_[0].h * text_scale);
    int foot_gap = std::max(2, foot_h1 / 4);
    int about_h = (info_about_header_.tex)
                      ? (int)(info_about_header_.h * text_scale) : 0;
    int footer_block_h = (about_h > 0 ? about_h + foot_gap : 0)
                       + (foot_h1 > 0 ? foot_h1 : 0)
                       + (foot_h2 > 0 ? (foot_gap + foot_h2) : 0)
                       + (foot_h3 > 0 ? (foot_gap + foot_h3) : 0);
    if (footer_block_h > 0) total_h += spacer_h + footer_block_h;

    // Reserve space for the "Check for updates" button — same recipe as
    // the "Copy network test script" button above (button + hover hint
    // slot) so the panel doesn't reflow on hover.
    int chk_btn_label_h = btn_label_h;
    int chk_btn_h = btn_h;
    total_h += spacer_h + chk_btn_h + line_gap + hint_label_h;

    total_h += pad - line_gap;

    int panel_w = (std::min)((int)(svw * 0.80f), (int)(uw * 1.6f));
    int panel_x = svx + (svw - panel_w) / 2;

    // Position below island bar
    int btn_sz = std::max(20, uw / 14);
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

    // "Copy network test script" button — pasted into a PowerShell window
    // to verify firewall rules and listening ports for AirPlay / scrcpy.
    // Sized to roughly match the network-requirement text rather than a
    // big call-to-action; a hover caption explains usage.
    cy += spacer_h;

    int mx, my;
    SDL_GetMouseState(&mx, &my);

#ifdef _WIN32
    // Render the label first so the button can wrap tightly around it.
    int lw = 0, lh = 0;
    SDL_Texture* lab = make_text_texture(sdl_renderer_,
        "Copy network test script", btn_label_h,
        220, 220, 230, &lw, &lh);
    int btn_w = lw + std::max(12, pad);
    if (btn_w > panel_w - pad * 2) btn_w = panel_w - pad * 2;
    int btn_x = panel_x + (panel_w - btn_w) / 2;
    int btn_y = cy;
    info_copy_ps_btn_ = {btn_x, btn_y, btn_w, btn_h};

    bool hov = info_panel_anim_ >= 1.0f &&
               in_rect(mx, my, btn_x, btn_y, btn_w, btn_h);
    uint8_t bg = hov ? 70 : 50;
    SDL_SetRenderDrawColor(sdl_renderer_, bg, bg, bg + 8, alpha);
    SDL_Rect br = {btn_x, btn_y, btn_w, btn_h};
    SDL_RenderFillRect(sdl_renderer_, &br);
    SDL_SetRenderDrawColor(sdl_renderer_, 110, 110, 130,
                           (uint8_t)(180 * info_panel_anim_));
    SDL_RenderDrawRect(sdl_renderer_, &br);

    if (lab) {
        SDL_SetTextureColorMod(lab, hov ? 255 : 220, hov ? 255 : 220, hov ? 255 : 230);
        SDL_SetTextureAlphaMod(lab, text_alpha);
        SDL_Rect ldst = {btn_x + (btn_w - lw) / 2,
                         btn_y + (btn_h - lh) / 2, lw, lh};
        SDL_RenderCopy(sdl_renderer_, lab, nullptr, &ldst);
        SDL_DestroyTexture(lab);
    }
    cy += btn_h + line_gap;

    // Hover caption — only rendered when the button is hovered. Reuses
    // the slot reserved in the height calculation so the panel doesn't
    // reflow when it appears.
    if (hov) {
        int hw = 0, hh = 0;
        SDL_Texture* hint = make_text_texture(sdl_renderer_,
            "Paste into a PowerShell window to verify firewall rules and listening ports.",
            hint_label_h, 150, 150, 160, &hw, &hh);
        if (hint) {
            // Shrink-to-fit if the line is wider than the panel.
            int max_w = panel_w - pad * 2;
            int draw_w = hw, draw_h = hh;
            if (hw > max_w) {
                float s = (float)max_w / hw;
                draw_w = max_w;
                draw_h = (int)(hh * s);
            }
            SDL_SetTextureAlphaMod(hint, text_alpha);
            SDL_Rect hdst = {panel_x + (panel_w - draw_w) / 2, cy, draw_w, draw_h};
            SDL_RenderCopy(sdl_renderer_, hint, nullptr, &hdst);
            SDL_DestroyTexture(hint);
        }
    }
#else
    info_copy_ps_btn_ = {0, 0, 0, 0};
#endif

    // Footer block at bottom of info panel — clones the waiting-screen
    // footer (MSEndpointMgr link / Simon link / Buy-me-a-coffee link +
    // version) using info_footer_* textures baked at the same font size
    // as the network-requirement lines so the visual weight matches the
    // rest of the panel. Append to footer_hits_ so the shared click
    // dispatcher (extended to fire while info_panel_visible_) handles
    // link clicks.
    if (footer_block_h > 0) {
        // Re-clear stale hits when we're streaming, since draw_footer
        // didn't run this frame and footer_hits_ would otherwise contain
        // last-frame waiting-screen rects. On the waiting screen we
        // intentionally keep the bottom-of-screen hits and APPEND.
        if (ever_received_frame_) footer_hits_.clear();

        int foot_y = panel_y + total_h - pad - chk_btn_h - line_gap
                     - hint_label_h - spacer_h - footer_block_h;
        // "About" header (small, same size as "Network requirements")
        if (about_h > 0) {
            int aw = (int)(info_about_header_.w * text_scale);
            int ax = panel_x + (panel_w - aw) / 2;
            SDL_Rect adst = {ax, foot_y, aw, about_h};
            SDL_SetTextureAlphaMod(info_about_header_.tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, info_about_header_.tex, nullptr, &adst);
            foot_y += about_h + foot_gap;
        }
        auto draw_info_footer_line = [&](const std::vector<FooterSeg>& segs,
                                          int line_y, int line_h_px) {
            if (segs.empty()) return;
            int total_w = info_footer_line_w(segs);
            int fx = panel_x + (panel_w - total_w) / 2;
            for (auto& s : segs) {
                int sw = (int)(s.w * text_scale), sh = (int)(s.h * text_scale);
                SDL_Rect dst = {fx, line_y, sw, sh};
                if (s.tex) {
                    SDL_SetTextureAlphaMod(s.tex, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, s.tex, nullptr, &dst);
                }
                if (!s.url.empty() || !s.tooltip.empty())
                    footer_hits_.push_back({fx, line_y, sw, sh, s.url, s.tooltip});
                fx += sw;
            }
            (void)line_h_px;
        };
        draw_info_footer_line(info_footer_line1_, foot_y, foot_h1);
        int y2 = foot_y + foot_h1 + foot_gap;
        draw_info_footer_line(info_footer_line2_, y2, foot_h2);
        int y3 = y2 + foot_h2 + foot_gap;
        draw_info_footer_line(info_footer_line3_, y3, foot_h3);

        // Hover tooltip for footer segments — same UX as draw_footer.
        if (info_panel_anim_ >= 1.0f) {
            int tmx, tmy;
            SDL_GetMouseState(&tmx, &tmy);
            std::string hovered;
            int hx = 0, hy = 0, hw = 0;
            for (auto& h : footer_hits_) {
                if (!h.tooltip.empty() && in_rect(tmx, tmy, h.x, h.y, h.w, h.h)) {
                    hovered = h.tooltip;
                    hx = h.x; hy = h.y; hw = h.w;
                    break;
                }
            }
            if (!hovered.empty() && tooltip_ready("infofooter:" + hovered)) {
                draw_bezel_tooltip(hovered, hx + hw / 2, hy);
            } else if (hovered.empty() &&
                       hover_key_.rfind("infofooter:", 0) == 0) {
                hover_key_.clear();
            }
        }
    }

    // "Check for updates" button — same visual recipe as the
    // "Copy network test script" button above. Sits at the very bottom
    // of the panel, with a hover hint slot reserved beneath it.
#ifdef _WIN32
    {
        bool busy = update_check_in_progress_.load();
        const char* chk_label = busy ? "Checking for updates\xE2\x80\xA6"
                                     : "Check for updates";
        int chk_lw = 0, chk_lh = 0;
        SDL_Texture* chk_lab = make_text_texture(sdl_renderer_,
            chk_label, chk_btn_label_h,
            220, 220, 230, &chk_lw, &chk_lh);
        int chk_btn_w = chk_lw + std::max(12, pad);
        if (chk_btn_w > panel_w - pad * 2) chk_btn_w = panel_w - pad * 2;
        int chk_btn_x = panel_x + (panel_w - chk_btn_w) / 2;
        // Anchor to bottom of the panel so the button is always pinned.
        int chk_btn_y = panel_y + total_h - pad - hint_label_h - line_gap - chk_btn_h;
        info_check_btn_ = {chk_btn_x, chk_btn_y, chk_btn_w, chk_btn_h};

        bool chk_hov = info_panel_anim_ >= 1.0f && !busy &&
                       in_rect(mx, my, chk_btn_x, chk_btn_y, chk_btn_w, chk_btn_h);
        uint8_t chk_bg = busy ? 40 : (chk_hov ? 70 : 50);
        SDL_SetRenderDrawColor(sdl_renderer_, chk_bg, chk_bg, chk_bg + 8, alpha);
        SDL_Rect chk_br = {chk_btn_x, chk_btn_y, chk_btn_w, chk_btn_h};
        SDL_RenderFillRect(sdl_renderer_, &chk_br);
        SDL_SetRenderDrawColor(sdl_renderer_, 110, 110, 130,
                               (uint8_t)(180 * info_panel_anim_));
        SDL_RenderDrawRect(sdl_renderer_, &chk_br);

        if (chk_lab) {
            uint8_t lr = busy ? 160 : (chk_hov ? 255 : 220);
            uint8_t lg = busy ? 160 : (chk_hov ? 255 : 220);
            uint8_t lb = busy ? 170 : (chk_hov ? 255 : 230);
            SDL_SetTextureColorMod(chk_lab, lr, lg, lb);
            SDL_SetTextureAlphaMod(chk_lab, text_alpha);
            SDL_Rect ldst = {chk_btn_x + (chk_btn_w - chk_lw) / 2,
                             chk_btn_y + (chk_btn_h - chk_lh) / 2,
                             chk_lw, chk_lh};
            SDL_RenderCopy(sdl_renderer_, chk_lab, nullptr, &ldst);
            SDL_DestroyTexture(chk_lab);
        }

        // Hover hint, mirrors the copy-ps button hint slot.
        if (chk_hov) {
            int hw = 0, hh = 0;
            SDL_Texture* hint = make_text_texture(sdl_renderer_,
                "Check GitHub for a newer release.",
                hint_label_h, 150, 150, 160, &hw, &hh);
            if (hint) {
                int max_w = panel_w - pad * 2;
                int draw_w = hw, draw_h = hh;
                if (hw > max_w) {
                    float s = (float)max_w / hw;
                    draw_w = max_w;
                    draw_h = (int)(hh * s);
                }
                SDL_SetTextureAlphaMod(hint, text_alpha);
                SDL_Rect hdst = {panel_x + (panel_w - draw_w) / 2,
                                 chk_btn_y + chk_btn_h + line_gap, draw_w, draw_h};
                SDL_RenderCopy(sdl_renderer_, hint, nullptr, &hdst);
                SDL_DestroyTexture(hint);
            }
        }
    }
#else
    info_check_btn_ = {0, 0, 0, 0};
#endif
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

    // UI dimensions are derived from a phone-equivalent reference width so
    // the Version panel stays compact on tablet/desktop sources.
    int uw = ui_ref_width();
    int pad = std::max(8, uw / 20);
    int line_gap = std::max(3, pad / 4);
    int spacer_h = std::max(4, pad / 2);

    // Scale text to fit panel, but never shrink below a readable minimum.
    // If a line is still too wide at the minimum scale it will overflow the
    // clip rect (acceptable) — the scrollbar handles vertical overflow.
    float max_text_w = uw * 0.7f;
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

    int panel_w = (std::min)((int)(svw * 0.85f), (int)(uw * 1.6f));
    int panel_x = svx + (svw - panel_w) / 2;

    // Position below island bar
    int btn_sz = std::max(20, uw / 14);
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

    // UI dimensions are derived from a phone-equivalent reference width so
    // the Settings swatches/labels stay compact on tablet/desktop sources.
    int uw = ui_ref_width();
    int pad      = std::max(10, uw / 18);
    int row_gap  = std::max(6,  pad / 2);
    int title_h  = std::max(14, uw / 22);
    int label_h  = std::max(11, uw / 30);
    int swatch   = std::max(22, uw / 14);
    int sw_gap   = std::max(6,  swatch / 4);

    int panel_w = (std::min)((int)(svw * 0.86f), (int)(uw * 1.6f));
    int panel_x = svx + (svw - panel_w) / 2;

    // Bottom of island row
    int btn_sz = std::max(20, uw / 14);
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
    int telemetry_sub_h = std::max(10, label_h * 4 / 5);
    int total_h = pad + title_h + row_gap
                + swatch_rows * (swatch + row_gap)
                + row_gap + label_h + row_gap            // toggle 1 (save)
                + label_h + row_gap                       // toggle 2 (clipboard)
                + label_h + row_gap                       // toggle 3 (Snagit)
                + label_h + row_gap                       // toggle 4 (computer name)
                + label_h + row_gap                       // toggle 5 (always on top)
                + (label_h + 2)                           // toggle 6 (telemetry)
                + 3 * (telemetry_sub_h + 1) + row_gap     // telemetry subtitle (3 lines)
                + label_h + row_gap                       // toggle 7 (file log)
                + label_h + row_gap                       // recording format row
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
    settings_toggle_snagit_btn_ = draw_toggle(
        "Open screenshots in Snagit Editor",
        settings_.screenshot_open_in_snagit, cy);
    cy += label_h + row_gap;
    settings_toggle_compname_btn_ = draw_toggle(
        "Identify as computer name (restart required)",
        settings_.use_computer_name, cy);
    cy += label_h + row_gap;
    settings_toggle_aot_btn_ = draw_toggle(
        "Always keep window on top",
        settings_.always_on_top, cy);
    cy += label_h + row_gap;
    settings_toggle_telemetry_btn_ = draw_toggle(
        "Send anonymous usage ping (recommended)",
        settings_.telemetry_enabled, cy);
    cy += label_h + 2;
    // Subtitle: explain what is collected and why. Always-visible (no hover
    // tooltips in SDL); rendered dim/smaller so it doesn't crowd the row.
    {
        int sub_h    = std::max(10, label_h * 4 / 5);
        int sub_box  = std::max(12, label_h + 2);
        int sub_x    = panel_x + pad + sub_box + std::max(6, sub_box / 3);
        const char* sub_lines[] = {
            "Random install ID, app version, Windows build.",
            "No IP, no name, no content.",
            "Inspires me to prioritize fixes and platforms."
        };
        for (const char* sl : sub_lines) {
            draw_label(sl, sub_h, sub_x, cy, 150, 150, 155, false);
            cy += sub_h + 1;
        }
    }
    cy += row_gap;
    settings_toggle_log_btn_ = draw_toggle(
        "Save log file to screenshots folder (this session)",
        log_to_file_session_, cy);
    cy += label_h + row_gap;

    // Recording format row — two pill buttons (MP4 / GIF) on the left so
    // all interactive controls line up vertically with the swatch grid and
    // checkbox column above; descriptive label sits to the right.
    {
        int row_x  = panel_x + pad;
        int box    = std::max(12, label_h + 2);
        auto draw_pill = [&](const std::string& label, bool sel,
                             int px, int py, int pw, int ph) -> BtnRect {
            if (sel) SDL_SetRenderDrawColor(sdl_renderer_, 90, 130, 200, alpha);
            else     SDL_SetRenderDrawColor(sdl_renderer_, 50, 50, 55, alpha);
            SDL_Rect br = {px, py, pw, ph};
            SDL_RenderFillRect(sdl_renderer_, &br);
            SDL_SetRenderDrawColor(sdl_renderer_, sel ? 200 : 100,
                                                  sel ? 220 : 100,
                                                  sel ? 255 : 110, text_alpha);
            SDL_RenderDrawRect(sdl_renderer_, &br);
            int tw = 0, th = 0;
            SDL_Texture* tt = make_text_texture(sdl_renderer_, label, label_h,
                                                sel ? 255 : 200,
                                                sel ? 255 : 200,
                                                sel ? 255 : 200,
                                                &tw, &th);
            if (tt) {
                SDL_SetTextureAlphaMod(tt, text_alpha);
                SDL_Rect td{px + (pw - tw) / 2, py + (ph - th) / 2, tw, th};
                SDL_RenderCopy(sdl_renderer_, tt, nullptr, &td);
                SDL_DestroyTexture(tt);
            }
            return BtnRect{px, py, pw, ph};
        };
        int pill_w = std::max(48, label_h * 4);
        int pill_gap = std::max(4, label_h / 3);
        int pill_h = box;
        int mp4_x = row_x;
        int gif_x = mp4_x + pill_w + pill_gap;
        settings_fmt_mp4_btn_ = draw_pill("MP4", settings_.record_format == 0,
                                          mp4_x, cy, pill_w, pill_h);
        settings_fmt_gif_btn_ = draw_pill("GIF", settings_.record_format == 1,
                                          gif_x, cy, pill_w, pill_h);
        // Label to the right of the pills.
        int label_x = gif_x + pill_w + std::max(6, box / 3);
        int lw = 0, lh = 0;
        SDL_Texture* lt = make_text_texture(sdl_renderer_, "Recording format",
                                            label_h, 220, 220, 225, &lw, &lh);
        if (lt) {
            SDL_SetTextureAlphaMod(lt, text_alpha);
            SDL_Rect ld{label_x, cy + (pill_h - lh) / 2, lw, lh};
            SDL_RenderCopy(sdl_renderer_, lt, nullptr, &ld);
            SDL_DestroyTexture(lt);
        }
    }
    cy += label_h + row_gap * 2;
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
    //    Uses coverage-based anti-aliasing via a signed distance from the
    //    corner centre. The interior is painted opaquely; only the ~1px
    //    outer rim and the body/border seam use partial alpha so the curve
    //    looks smooth without any stair-step ghosts.
    //       d + 0.5 < r_in            -> drawer (opaque)
    //       d in (r_in-0.5, r_in+0.5) -> blend drawer/border
    //       d in [r_in+0.5, r_out-0.5)-> border (opaque)
    //       d in [r_out-0.5, r_out+0.5]-> blend border with destination
    //       d > r_out + 0.5           -> skip (transparent)
    auto paint_corner = [&](int cx, int cy, int quad) {
        float r_out = (float)pr;
        float r_in  = (float)(pr - bw);
        for (int dy = -pr - 1; dy <= pr + 1; dy++) {
            for (int dx = -pr - 1; dx <= pr + 1; dx++) {
                if (quad == 0 && !(dx >= 0 && dy <= 0)) continue;
                if (quad == 1 && !(dx >= 0 && dy >= 0)) continue;
                float d = std::sqrt((float)(dx * dx + dy * dy));
                // Outer coverage: 1 well inside r_out, 0 well outside.
                float cov_out = std::clamp(r_out + 0.5f - d, 0.0f, 1.0f);
                if (cov_out <= 0.0f) continue;
                // Inner coverage: 1 well inside r_in, 0 well outside.
                float cov_in  = std::clamp(r_in  + 0.5f - d, 0.0f, 1.0f);
                float ring = cov_out - cov_in; // 1 only in the strict band
                int px = cx + dx;
                int py = cy + dy;
                if (cov_in >= 1.0f) {
                    // Fully inside -> opaque drawer.
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                } else if (cov_out >= 1.0f && ring <= 0.0f) {
                    // Fully drawer, fully inside outer (cov_in must be <1
                    // here so we're in the body/border seam transition).
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                } else {
                    // Blend drawer + border according to (cov_in) and the
                    // remaining outer coverage. Premix the RGB once, then
                    // emit with cov_out as the alpha so the rim feathers
                    // smoothly against whatever is behind the window.
                    float bd = cov_in;          // drawer share inside ring
                    float bb = ring;            // border share
                    float total = bd + bb;
                    uint8_t mr, mg, mb;
                    if (total <= 0.0001f) {
                        mr = er; mg = eg; mb = eb;
                    } else {
                        mr = (uint8_t)((dr * bd + er * bb) / total);
                        mg = (uint8_t)((dg * bd + eg * bb) / total);
                        mb = (uint8_t)((db * bd + eb * bb) / total);
                    }
                    uint8_t a = (uint8_t)(cov_out * 255.0f);
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(sdl_renderer_, mr, mg, mb, a);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                }
            }
        }
    };
    paint_corner(cx_r, cy_t, 0);
    paint_corner(cx_r, cy_b, 1);

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // Get log lines
    auto lines = opm::LogBuffer::instance().get_lines();
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
    uint64_t cur_ver = opm::LogBuffer::instance().version();
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
    start_android_discovery();
}

void Renderer::start_android_discovery() {
    if (!android_discover_fn_) return;
    // Always restart cleanly so re-opening the panel (e.g. while another
    // device is already connected) gets a fresh poll and a fresh list,
    // rather than silently re-attaching to a stale worker.
    stop_android_discovery();
    android_discover_running_.store(true);
    {
        std::lock_guard lk(android_discovered_mutex_);
        android_discovered_.clear();
    }
    android_discover_thread_ = std::thread([this]() {
        while (android_discover_running_.load()) {
            android_discover_in_progress_.store(true);
            std::vector<DiscoveredAndroidDevice> fresh;
            try {
                fresh = android_discover_fn_();
            } catch (...) {
                // Ignore — keep polling.
            }
            android_discover_in_progress_.store(false);
            {
                std::lock_guard lk(android_discovered_mutex_);
                android_discovered_ = std::move(fresh);
            }
            // Poll roughly every 1 s so a phone that powers on Wireless
            // Debugging while the panel is already open shows up quickly.
            // Sleep in 100 ms chunks so a panel close is responsive.
            for (int i = 0; i < 10 && android_discover_running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void Renderer::stop_android_discovery() {
    if (!android_discover_running_.exchange(false)) return;
    if (!android_discover_thread_.joinable()) return;

    // The discovery worker spends most of its time inside
    // `adb mdns services` / `adb devices -l` subprocess calls. adb is
    // known to occasionally hang for many seconds (especially after a
    // phone disconnects), and `spawn_capture` blocks with
    // WaitForSingleObject(..., INFINITE). Give the worker a brief window
    // to notice the running flag and exit cleanly; if it doesn't, detach
    // so we never freeze the UI / app shutdown waiting on a stuck adb.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (android_discover_in_progress_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (android_discover_in_progress_.load()) {
        // Still inside an adb call — detaching is the safe choice. The
        // thread captures `this` (Renderer) and uses an App-owned adb
        // controller via android_discover_fn_, so the leaked thread will
        // be torn down only at process exit. That's acceptable: it's a
        // diagnostic shutdown path and the OS reclaims everything.
        android_discover_thread_.detach();
        return;
    }
    android_discover_thread_.join();
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
    if (android_panel_anim_ <= 0.0f) {
        // Panel fully hidden — stop the discovery worker if it's still
        // running from the previous open. Guard on `visible_` so that
        // the very first frame after show_android_panel() (anim still
        // at 0) doesn't immediately tear down the worker that
        // show_android_panel() just spawned.
        if (!android_panel_visible_) stop_android_discovery();
        if (!android_panel_visible_) return;
    }

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    // Cap panel width using the phone-equivalent reference so the dialog
    // (and every glyph derived from `panel_w`) stays compact when mirroring
    // a tablet/desktop source. Same recipe as the info/version/settings
    // panels — see ui_ref_width().
    int uw = ui_ref_width();
    int panel_w = std::min((int)(svw * 0.85f), (int)(uw * 1.6f));
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

    // Snapshot the discovered devices once for this draw call so the
    // height calculation and the row rendering use identical data.
    std::vector<DiscoveredAndroidDevice> discovered_snapshot;
    {
        std::lock_guard lk(android_discovered_mutex_);
        discovered_snapshot = android_discovered_;
    }
    // Cap the visible list so the panel does not bloat past the screen.
    const int max_discover_rows = 3;
    int discover_visible = std::min<int>((int)discovered_snapshot.size(),
                                         max_discover_rows);
    int discover_row_h = field_h - 4;
    // Always reserve one row's worth of space (either real entries OR the
    // "Scanning..." placeholder while the list is still empty).
    int discover_rows_height = (discover_visible > 0)
        ? discover_visible * (discover_row_h + 4) - 4
        : label_h;
    int discover_section_h = label_h + 4 + discover_rows_height + gap;

    // Extra breathing room below the buttons, on top of the normal `pad`,
    // so the Connect/Disconnect row visibly sits above the panel's bottom
    // edge instead of touching it.
    int btn_bottom_pad = std::max(10, pad);
    int total_h = pad + title_h + gap*2
                + discover_section_h
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

    // ---- Discovered devices section ----
    android_discover_btns_.clear();
    {
        bool running = android_discover_running_.load();
        std::string heading = "Discovered devices";
        if (running) {
            // Animated dot trail so the user sees the panel is actively
            // looking, even between the (~1 s) mDNS poll cycles.
            using namespace std::chrono;
            auto ms = duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count();
            int dots = (int)((ms / 350) % 4);
            heading += discovered_snapshot.empty() ? " (scanning"
                                                   : " (refreshing";
            for (int i = 0; i < dots; ++i) heading += '.';
            heading += ')';
        }
        int hw=0, hh=0;
        SDL_Texture* hd = make_text_texture(sdl_renderer_, heading,
            label_h, 170, 170, 180, &hw, &hh);
        if (hd) {
            SDL_Rect d = {col1_x, cy, hw, hh};
            SDL_SetTextureAlphaMod(hd, text_alpha);
            SDL_RenderCopy(sdl_renderer_, hd, nullptr, &d);
            SDL_DestroyTexture(hd);
        }
        cy += label_h + 4;

        if (discovered_snapshot.empty()) {
            const char* msg = "No devices found. Open Wireless debugging on the phone.";
            int tw=0, th=0;
            SDL_Texture* tex = make_text_texture(sdl_renderer_, msg,
                label_h - 1, 130, 130, 140, &tw, &th);
            if (tex) {
                SDL_Rect d = {col1_x, cy, std::min(tw, panel_w - pad*2), th};
                SDL_SetTextureAlphaMod(tex, text_alpha);
                SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
                SDL_DestroyTexture(tex);
            }
            cy += label_h;
        } else {
            for (int i = 0; i < discover_visible; ++i) {
                const auto& dev = discovered_snapshot[i];
                int row_w = panel_w - pad * 2;
                SDL_Rect rr = {col1_x, cy, row_w, discover_row_h};
                BtnRect hit{rr.x, rr.y, rr.w, rr.h};
                android_discover_btns_.push_back(hit);
                // Highlight the row that matches the values currently
                // typed in the IP / Connect port fields so the user can
                // see "this is the device I'm about to connect to".
                bool selected = !android_ip_.empty() && dev.ip == android_ip_ &&
                                (android_connect_port_.empty() ||
                                 dev.connect_port == android_connect_port_);
                SDL_SetRenderDrawColor(sdl_renderer_,
                    selected ? 45 : 38,
                    selected ? 60 : 38,
                    selected ? 90 : 44, alpha);
                SDL_RenderFillRect(sdl_renderer_, &rr);
                SDL_SetRenderDrawColor(sdl_renderer_,
                    selected ? 110 : 70,
                    selected ? 150 : 70,
                    selected ? 220 : 80, alpha);
                SDL_RenderDrawRect(sdl_renderer_, &rr);

                // Left column: device label.
                int label_pad = 8;
                int max_label_w = (int)(row_w * 0.55f);
                int tw=0, th=0;
                SDL_Texture* lt = make_text_texture(sdl_renderer_,
                    dev.label.empty() ? std::string("Android device") : dev.label,
                    label_h - 1, 230, 230, 235, &tw, &th);
                if (lt) {
                    int draw_w = std::min(tw, max_label_w);
                    SDL_Rect d = {rr.x + label_pad,
                                  rr.y + (rr.h - th) / 2,
                                  draw_w, th};
                    SDL_SetTextureAlphaMod(lt, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, lt, nullptr, &d);
                    SDL_DestroyTexture(lt);
                }

                // Right column: ip[:port] + pairing hint.
                std::string detail = dev.ip;
                if (!dev.connect_port.empty()) detail += ":" + dev.connect_port;
                if (!dev.pair_port.empty()) detail += "  (pairing)";
                int dw=0, dh=0;
                SDL_Texture* dt = make_text_texture(sdl_renderer_, detail,
                    label_h - 1, 190, 200, 220, &dw, &dh);
                if (dt) {
                    SDL_Rect d = {rr.x + rr.w - dw - label_pad,
                                  rr.y + (rr.h - dh) / 2,
                                  dw, dh};
                    SDL_SetTextureAlphaMod(dt, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, dt, nullptr, &d);
                    SDL_DestroyTexture(dt);
                }
                cy += discover_row_h + 4;
            }
        }
        cy += gap;
    }

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

    // Cap panel width using ui_ref_width() so the help overlay stays the
    // same neat size when mirroring a tablet/desktop source.
    int uw = ui_ref_width();
    int panel_w = std::min((int)(svw * 0.92f), (int)(uw * 1.6f));
    int pad     = std::max(10, panel_w / 24);
    int title_h = std::max(18, panel_w / 18);
    int line_h  = std::max(13, panel_w / 28);
    int line_gap = std::max(2, line_h / 6);   // space between wrapped lines
    int para_gap = std::max(6, line_h / 2);   // space between paragraphs
    int head_gap = std::max(8, line_h * 2 / 3); // extra above headings

    // Paragraph model: every entry is either a heading, a gap, or a text
    // paragraph with an optional inline prefix (e.g. "1." or "*"). Text
    // paragraphs are word-wrapped to body width and continuation lines
    // are hang-indented past the prefix.
    enum class Kind { HEADING, TEXT, GAP };
    struct Para { Kind kind; std::string prefix; std::string text; };
    static const Para paragraphs[] = {
        {Kind::HEADING, "", "How to set up your Android phone"},
        {Kind::TEXT,    "1.", "Open Settings on the phone."},
        {Kind::TEXT,    "2.", "Tap 'About phone' / 'Software information'."},
        {Kind::TEXT,    "3.", "Tap 'Build number' seven times to enable Developer options."},
        {Kind::TEXT,    "4.", "Back in Settings open 'Developer options' and turn on 'Wireless debugging' (the phone must be on the same Wi-Fi as the PC)."},

        {Kind::HEADING, "", "Auto-discovery (recommended)"},
        {Kind::TEXT,    "", "This panel scans the network for phones with Wireless debugging on. Discovered phones appear in the 'Discovered devices' list at the top."},
        {Kind::TEXT,    "", "Tip: if your phone is not showing up, toggle Wireless debugging off and on again on the phone. It will then re-advertise its service and pop up in the list within a second."},
        {Kind::TEXT,    "", "Click a discovered device to fill in IP address and Connect port automatically. Then open 'Pair device with pairing code' on the phone, type the Pair port and 6-digit Pairing code shown there, and press Connect."},

        {Kind::HEADING, "", "Reading the values manually"},
        {Kind::TEXT,    "*", "IP address: First part of 'IP address & Port' on the main Wireless debugging screen (e.g. 192.168.10.73)."},
        {Kind::TEXT,    "*", "Connect port: Second part of the same line (e.g. :46029). Stable until you toggle Wi-Fi. Recommended."},
        {Kind::TEXT,    "*", "Pair port: Second part of 'IP address & Port' inside the 'Pair device with pairing code' popup. This port is one-shot - reopen the popup if it expires."},
        {Kind::TEXT,    "*", "Pairing code: The 6-digit number in the same popup."},

        {Kind::HEADING, "", "Multiple phones"},
        {Kind::TEXT,    "", "You can keep several Android phones connected at once. Each gets its own dot in the bottom row - left-click to switch which phone is on screen, right-click for a per-device disconnect menu."},

        {Kind::HEADING, "", "Note for managed devices"},
        {Kind::TEXT,    "", "Developer options can be blocked by your organisation through MDM (Intune, Workspace ONE, etc.). If 'Build number' refuses to enable developer mode, your work profile is locked down and Wireless debugging cannot be used on this device."},
    };
    int n_paras = (int)(sizeof(paragraphs) / sizeof(paragraphs[0]));

    // --- layout pass: word-wrap every paragraph to the available width ---
    int sb_w   = std::max(3, pad / 4);
    int sb_gap = std::max(4, sb_w);
    int body_w = panel_w - pad * 2 - (sb_w + sb_gap);

    struct LineOut { std::string text; int x_offset; int h; bool heading; };
    std::vector<LineOut> rendered;
    std::vector<int> gap_after; // extra px after each rendered line

    int space_w = measure_text_w(" ", line_h);

    auto wrap = [&](const std::string& s, int h, int max_w) {
        std::vector<std::string> out;
        if (s.empty()) { out.push_back(""); return out; }
        // Tokenise on spaces, keeping word boundaries.
        std::vector<std::string> words;
        std::string cur;
        for (char c : s) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
        std::string line;
        for (auto& w : words) {
            std::string trial = line.empty() ? w : line + " " + w;
            if (measure_text_w(trial, h) <= max_w || line.empty()) {
                line = trial;
            } else {
                out.push_back(line);
                line = w;
            }
        }
        if (!line.empty()) out.push_back(line);
        return out;
    };

    for (int i = 0; i < n_paras; ++i) {
        const Para& p = paragraphs[i];
        if (p.kind == Kind::GAP) {
            // (no GAP paragraphs in current data, kept for completeness)
            if (!rendered.empty()) gap_after.back() += para_gap;
            continue;
        }
        // Inter-paragraph spacing: small gap before normal text, larger
        // gap before a heading. Skip the leading paragraph.
        if (!rendered.empty()) {
            gap_after.back() += (p.kind == Kind::HEADING) ? head_gap : para_gap;
        }
        if (p.kind == Kind::HEADING) {
            rendered.push_back({p.text, 0, title_h, true});
            gap_after.push_back(0);
            continue;
        }
        // TEXT
        int prefix_w = 0;
        if (!p.prefix.empty()) prefix_w = measure_text_w(p.prefix, line_h) + space_w;
        int avail = std::max(40, body_w - prefix_w);
        auto lines = wrap(p.text, line_h, avail);
        for (size_t j = 0; j < lines.size(); ++j) {
            std::string txt;
            int xoff;
            if (j == 0 && !p.prefix.empty()) {
                txt = p.prefix + " " + lines[j];
                xoff = 0;
            } else {
                txt = lines[j];
                xoff = prefix_w;
            }
            rendered.push_back({txt, xoff, line_h, false});
            gap_after.push_back(line_gap);
        }
    }

    // Total content height.
    int content_h = 0;
    for (size_t i = 0; i < rendered.size(); ++i)
        content_h += rendered[i].h + gap_after[i];

    // Breathing margin above/below the help overlay so it doesn't slam
    // into the phone's bezel curve.
    int breath = std::max(svh / 14, 24);
    int max_panel_h = svh - breath * 2;
    int wanted_h = pad * 2 + content_h;
    int total_h = std::min(wanted_h, max_panel_h);
    if (total_h < title_h * 4) total_h = std::min(max_panel_h, title_h * 4);

    int panel_x = svx + (svw - panel_w) / 2;
    int panel_y = svy + (svh - total_h) / 2;

    android_help_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    uint8_t alpha = 250;
    uint8_t text_alpha = 255;

    int pr = std::max(8, pad / 2);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect bgbody = {panel_x + pr, panel_y, panel_w - pr*2, total_h};
    SDL_RenderFillRect(sdl_renderer_, &bgbody);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, total_h - pr*2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, total_h - pr*2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + total_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + total_h - pr, pr);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

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

    int body_x = panel_x + pad;
    int body_y = panel_y + pad;
    int body_h = total_h - pad * 2;

    android_help_max_scroll_ = std::max(0, content_h - body_h);
    if (android_help_scroll_ > android_help_max_scroll_)
        android_help_scroll_ = android_help_max_scroll_;
    if (android_help_scroll_ < 0) android_help_scroll_ = 0;

    SDL_Rect clip = {body_x, body_y, body_w, body_h};
    SDL_Rect prev_clip;
    SDL_bool was_clip = SDL_RenderIsClipEnabled(sdl_renderer_);
    if (was_clip) SDL_RenderGetClipRect(sdl_renderer_, &prev_clip);
    SDL_RenderSetClipRect(sdl_renderer_, &clip);

    int cy = body_y - android_help_scroll_;
    for (size_t i = 0; i < rendered.size(); ++i) {
        const auto& r = rendered[i];
        int step = r.h + gap_after[i];
        if (cy + r.h < body_y) { cy += step; continue; }
        if (cy > body_y + body_h) break;
        if (!r.text.empty()) {
            uint8_t cr = r.heading ? 230 : 195;
            uint8_t cg = r.heading ? 230 : 200;
            uint8_t cb = r.heading ? 245 : 210;
            int tw = 0, th = 0;
            SDL_Texture* tex = make_text_texture(sdl_renderer_, r.text, r.h,
                                                 cr, cg, cb, &tw, &th);
            if (tex) {
                SDL_Rect d = {body_x + r.x_offset, cy, tw, th};
                SDL_SetTextureAlphaMod(tex, text_alpha);
                SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
                SDL_DestroyTexture(tex);
            }
        }
        cy += step;
    }

    if (was_clip) SDL_RenderSetClipRect(sdl_renderer_, &prev_clip);
    else          SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // Scrollbar — only when content actually overflows. Matches the
    // version panel: a single thumb (no track) tucked inside the rounded
    // corner radius.
    if (android_help_max_scroll_ > 0) {
        int track_x = panel_x + panel_w - sb_w - pr;
        int track_y = panel_y + pr;
        int track_h = total_h - pr * 2;
        android_help_track_rect_ = {track_x, track_y, sb_w, track_h};
        int thumb_h = std::max(pr * 2, (int)((float)track_h * body_h /
                                              (float)content_h));
        thumb_h = std::min(thumb_h, track_h);
        int max_top = track_h - thumb_h;
        int thumb_y = track_y + (max_top > 0
            ? (int)((float)android_help_scroll_ / android_help_max_scroll_ * max_top)
            : 0);
        android_help_thumb_rect_ = {track_x, thumb_y, sb_w, thumb_h};
        uint8_t a = android_help_dragging_ ? 220 : 180;
        SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, a);
        SDL_Rect thr = {track_x, thumb_y, sb_w, thumb_h};
        SDL_RenderFillRect(sdl_renderer_, &thr);
    } else {
        android_help_track_rect_ = {0, 0, 0, 0};
        android_help_thumb_rect_ = {0, 0, 0, 0};
    }
}

void Renderer::take_screenshot() {
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        std::cout << "[Screenshot] No frame data available\n";
        return;
    }

    const bool save = settings_.screenshot_save_to_folder;
    const bool clip = settings_.screenshot_copy_to_clipboard;
    const bool snag = settings_.screenshot_open_in_snagit;
    if (!save && !clip && !snag) {
        std::cout << "[Screenshot] All output options are disabled in settings\n";
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
    // Primary filename in the user's screenshots folder. If "save" is off
    // but Snagit is on we write a transient copy into %TEMP% so Snagit
    // has something to open.
    std::string filename = screenshot_dir_ + "/screenshot_" + timestamp + ".png";
    std::string snagit_path; // path passed to Snagit when save==false
    if (!save && snag) {
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (!ec) snagit_path = (tmp / ("1PhoneMirror_" + std::string(timestamp) + ".png")).string();
    }

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
            } else if (snag && !snagit_path.empty()) {
                wrote = stbi_write_png(snagit_path.c_str(), out_w, out_h, 4, composite, out_w * 4) != 0;
            }
            if (wrote) {
                saved = true;
                if (clip) copy_to_clipboard(composite, out_w, out_h);
                if (snag) open_in_snagit(save ? filename : snagit_path);
                if (save) std::cout << "[Screenshot] Saved: " << filename << " (" << out_w << "x" << out_h << ")\n";
                else if (clip) std::cout << "[Screenshot] Copied to clipboard (" << out_w << "x" << out_h << ")\n";
            }
            delete[] composite;
        }
    } else {
        bool wrote = true;
        if (save) {
            wrote = stbi_write_png(filename.c_str(), last_frame_w_, last_frame_h_, 4,
                                   last_frame_data_.data(), last_frame_stride_) != 0;
        } else if (snag && !snagit_path.empty()) {
            wrote = stbi_write_png(snagit_path.c_str(), last_frame_w_, last_frame_h_, 4,
                                   last_frame_data_.data(), last_frame_stride_) != 0;
        }
        if (wrote) {
            saved = true;
            if (clip) copy_to_clipboard(last_frame_data_.data(), last_frame_w_, last_frame_h_);
            if (snag) open_in_snagit(save ? filename : snagit_path);
            if (save) std::cout << "[Screenshot] Saved: " << filename << "\n";
            else if (clip) std::cout << "[Screenshot] Copied to clipboard\n";
        }
    }

    if (saved) {
        std::string parts;
        auto add = [&](const char* p){ if (!parts.empty()) parts += " + "; parts += p; };
        if (save) add("saved to Pictures");
        if (clip) add("copied to clipboard");
        if (snag) add("opened in Snagit");
        toast_text_ = "Screenshot: " + parts;
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

// ---- Screenshot annotation overlay (Ctrl+Shift+S) ----
//
// Snapshots the current composited frame (same buffer that take_screenshot
// would write), then enters a modal markup mode with a small floating
// toolbar: arrow / rectangle / highlight / pixelate, four colour swatches,
// undo, save and cancel. Save bakes the strokes back into the captured
// RGBA and re-uses the same write+clipboard logic as a plain screenshot.

namespace {

// Five-colour palette — kept tiny on purpose; v1 is a quick-markup tool,
// not a paint program. Order: red, yellow, green, black, white.
struct AnnoColor { uint8_t r, g, b; };
constexpr AnnoColor k_anno_palette[5] = {
    {235,  80,  80},  // red
    {245, 205,  60},  // yellow
    { 90, 200, 110},  // green
    { 20,  20,  20},  // black
    {245, 245, 245},  // white
};

// Pixelate block size in image-space pixels. 12 is coarse enough that
// faces / phone numbers / OTPs are unreadable but fine enough that the
// general layout is preserved.
constexpr int k_anno_pix_block = 12;

// Average a `block`x`block` patch of RGBA at (sx,sy) inside src into a
// packed 0xRRGGBB. Out-of-bounds reads are clamped.
inline uint32_t avg_block_rgb(const uint8_t* src, int w, int h, int stride,
                              int sx, int sy, int block) {
    uint32_t rsum = 0, gsum = 0, bsum = 0, n = 0;
    int x0 = std::max(0, sx);
    int y0 = std::max(0, sy);
    int x1 = std::min(w, sx + block);
    int y1 = std::min(h, sy + block);
    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* row = src + yy * stride;
        for (int xx = x0; xx < x1; ++xx) {
            const uint8_t* p = row + xx * 4;
            rsum += p[0]; gsum += p[1]; bsum += p[2]; ++n;
        }
    }
    if (n == 0) return 0;
    return ((rsum / n) << 16) | ((gsum / n) << 8) | (bsum / n);
}

// Bresenham-style thick line into RGBA. Used by the bake step so the
// annotated PNG matches what the user saw in the editor.
inline void plot_disc_rgba(uint8_t* rgba, int w, int h, int stride,
                           int cx, int cy, int rad,
                           uint8_t r, uint8_t g, uint8_t b) {
    int r2 = rad * rad;
    int x0 = std::max(0, cx - rad), x1 = std::min(w - 1, cx + rad);
    int y0 = std::max(0, cy - rad), y1 = std::min(h - 1, cy + rad);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                uint8_t* p = rgba + y * stride + x * 4;
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            }
        }
    }
}

inline void thick_line_rgba(uint8_t* rgba, int w, int h, int stride,
                            int x0, int y0, int x1, int y1, int thick,
                            uint8_t r, uint8_t g, uint8_t b) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int rad = std::max(1, thick / 2);
    while (true) {
        plot_disc_rgba(rgba, w, h, stride, x0, y0, rad, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

inline void rect_outline_rgba(uint8_t* rgba, int w, int h, int stride,
                              int rx, int ry, int rw, int rh, int thick,
                              uint8_t r, uint8_t g, uint8_t b) {
    thick_line_rgba(rgba, w, h, stride, rx, ry, rx + rw, ry, thick, r, g, b);
    thick_line_rgba(rgba, w, h, stride, rx, ry + rh, rx + rw, ry + rh, thick, r, g, b);
    thick_line_rgba(rgba, w, h, stride, rx, ry, rx, ry + rh, thick, r, g, b);
    thick_line_rgba(rgba, w, h, stride, rx + rw, ry, rx + rw, ry + rh, thick, r, g, b);
}

inline void filled_rect_blend_rgba(uint8_t* rgba, int w, int h, int stride,
                                   int rx, int ry, int rw, int rh,
                                   uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x0 = std::max(0, rx), y0 = std::max(0, ry);
    int x1 = std::min(w, rx + rw), y1 = std::min(h, ry + rh);
    float alpha = a / 255.0f, inv = 1.0f - alpha;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint8_t* p = rgba + y * stride + x * 4;
            p[0] = (uint8_t)(r * alpha + p[0] * inv);
            p[1] = (uint8_t)(g * alpha + p[1] * inv);
            p[2] = (uint8_t)(b * alpha + p[2] * inv);
            // Keep alpha at 255 — captured frame is already opaque.
        }
    }
}

// Draw a filled triangle (arrow head) into RGBA. Standard half-space test.
inline void fill_triangle_rgba(uint8_t* rgba, int w, int h, int stride,
                               int ax, int ay, int bx, int by, int cx, int cy,
                               uint8_t r, uint8_t g, uint8_t b) {
    int xmin = std::max(0, std::min({ax, bx, cx}));
    int xmax = std::min(w - 1, std::max({ax, bx, cx}));
    int ymin = std::max(0, std::min({ay, by, cy}));
    int ymax = std::min(h - 1, std::max({ay, by, cy}));
    auto edge = [](int x0, int y0, int x1, int y1, int x, int y) {
        return (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
    };
    for (int y = ymin; y <= ymax; ++y) {
        for (int x = xmin; x <= xmax; ++x) {
            int e0 = edge(ax, ay, bx, by, x, y);
            int e1 = edge(bx, by, cx, cy, x, y);
            int e2 = edge(cx, cy, ax, ay, x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                uint8_t* p = rgba + y * stride + x * 4;
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            }
        }
    }
}

// Render a single line of text into a freshly-allocated RGBA buffer, in
// the same colour `cr,cg,cb`. Alpha comes from the GDI ClearType
// luminance, so callers can alpha-blend the result onto another RGBA
// surface. Returns true on success and writes into `out`. Used by the
// annotator's save path to bake Text strokes into the PNG.
inline bool make_text_rgba_buffer(const std::string& text, int font_height,
                                  uint8_t cr, uint8_t cg, uint8_t cb,
                                  std::vector<uint8_t>& out, int& out_w, int& out_h) {
    out.clear();
    out_w = out_h = 0;
    if (text.empty() || font_height <= 0) return false;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    HFONT font = CreateFontA(
        -font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return false;
    }
    TEXTMETRICA tm;
    if (GetTextMetricsA(hdc, &tm)) sz.cx += tm.tmOverhang;
    sz.cx += (font_height + 3) / 4;
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
        return false;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);
    memset(bits, 0, (size_t)sz.cx * sz.cy * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();
    auto* src = static_cast<uint8_t*>(bits);
    out.resize((size_t)sz.cx * sz.cy * 4);
    for (int i = 0; i < sz.cx * sz.cy; ++i) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        out[i * 4 + 0] = cr;
        out[i * 4 + 1] = cg;
        out[i * 4 + 2] = cb;
        out[i * 4 + 3] = alpha;
    }
    out_w = sz.cx; out_h = sz.cy;
    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return true;
}

// Alpha-blend an RGBA glyph buffer onto a destination RGBA buffer at
// (dx, dy). Source alpha modulates the per-pixel blend.
inline void blit_rgba_blend(uint8_t* dst, int dw, int dh, int dstride,
                            const uint8_t* src, int sw, int sh,
                            int dx, int dy) {
    int x0 = std::max(0, dx), y0 = std::max(0, dy);
    int x1 = std::min(dw, dx + sw), y1 = std::min(dh, dy + sh);
    for (int y = y0; y < y1; ++y) {
        const uint8_t* srow = src + (y - dy) * sw * 4;
        uint8_t* drow = dst + y * dstride;
        for (int x = x0; x < x1; ++x) {
            const uint8_t* sp = srow + (x - dx) * 4;
            uint8_t a = sp[3];
            if (a == 0) continue;
            uint8_t* dp = drow + x * 4;
            int inv = 255 - a;
            dp[0] = (uint8_t)((sp[0] * a + dp[0] * inv) / 255);
            dp[1] = (uint8_t)((sp[1] * a + dp[1] * inv) / 255);
            dp[2] = (uint8_t)((sp[2] * a + dp[2] * inv) / 255);
            dp[3] = 255;
        }
    }
}

} // namespace

void Renderer::begin_annotation() {
    if (annotator_active_) return;
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        std::cout << "[Annotate] No frame to annotate yet\n";
        return;
    }

    // Capture the same composite take_screenshot() would save, so what
    // the user marks up matches what they would have got with Ctrl+S.
    int cap_w = 0, cap_h = 0;
    std::vector<uint8_t> rgba;
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        uint8_t* composite = phone_frame_.composite_screenshot(
            sdl_renderer_,
            last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
            &cap_w, &cap_h);
        if (!composite) {
            std::cout << "[Annotate] composite_screenshot returned null\n";
            return;
        }
        rgba.assign(composite, composite + (size_t)cap_w * cap_h * 4);
        delete[] composite;
    } else {
        cap_w = last_frame_w_;
        cap_h = last_frame_h_;
        rgba.resize((size_t)cap_w * cap_h * 4);
        // last_frame_stride_ may include padding; copy row-by-row.
        for (int y = 0; y < cap_h; ++y) {
            memcpy(rgba.data() + (size_t)y * cap_w * 4,
                   last_frame_data_.data() + (size_t)y * last_frame_stride_,
                   (size_t)cap_w * 4);
        }
    }

    // Build SDL texture from the captured RGBA. Kept around for the
    // duration of the editor session and re-used across frames.
    if (annotator_bg_tex_) {
        SDL_DestroyTexture(annotator_bg_tex_);
        annotator_bg_tex_ = nullptr;
    }
    annotator_bg_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ABGR8888,
                                          SDL_TEXTUREACCESS_STATIC, cap_w, cap_h);
    if (!annotator_bg_tex_) {
        std::cerr << "[Annotate] SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        return;
    }
    SDL_UpdateTexture(annotator_bg_tex_, nullptr, rgba.data(), cap_w * 4);
    SDL_SetTextureBlendMode(annotator_bg_tex_, SDL_BLENDMODE_BLEND);

    annotator_bg_orig_ = std::move(rgba);
    annotator_bg_w_ = cap_w;
    annotator_bg_h_ = cap_h;
    annotator_strokes_.clear();
    annotator_drawing_ = false;
    annotator_active_ = true;
    std::cout << "[Annotate] Editor open (" << cap_w << "x" << cap_h << ")\n";
}

void Renderer::end_annotation() {
    if (annotator_bg_tex_) {
        SDL_DestroyTexture(annotator_bg_tex_);
        annotator_bg_tex_ = nullptr;
    }
    // Free per-stroke text textures before dropping the stroke list.
    for (auto& s : annotator_strokes_) {
        if (s.text_tex) { SDL_DestroyTexture(s.text_tex); s.text_tex = nullptr; }
    }
    if (annotator_text_preview_tex_) {
        SDL_DestroyTexture(annotator_text_preview_tex_);
        annotator_text_preview_tex_ = nullptr;
    }
    annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
    annotator_text_preview_cached_.clear();
    if (annotator_text_input_active_) {
        SDL_StopTextInput();
        annotator_text_input_active_ = false;
    }
    annotator_text_buf_.clear();
    annotator_slider_drag_ = false;
    annotator_bg_orig_.clear();
    annotator_bg_orig_.shrink_to_fit();
    annotator_strokes_.clear();
    annotator_drawing_ = false;
    annotator_active_ = false;
    annotator_bg_w_ = annotator_bg_h_ = 0;
}

void Renderer::bake_pixelate_stroke(AnnoStroke& s) const {
    int rx = std::min(s.x0, s.x1);
    int ry = std::min(s.y0, s.y1);
    int rw = std::abs(s.x1 - s.x0);
    int rh = std::abs(s.y1 - s.y0);
    if (rw < k_anno_pix_block || rh < k_anno_pix_block) {
        // Too small to mosaic meaningfully — collapse to a tiny patch
        // covering at least one block so the user still gets feedback.
        rw = std::max(rw, k_anno_pix_block);
        rh = std::max(rh, k_anno_pix_block);
    }
    int cols = (rw + k_anno_pix_block - 1) / k_anno_pix_block;
    int rows = (rh + k_anno_pix_block - 1) / k_anno_pix_block;
    s.pix_origin_x = rx;
    s.pix_origin_y = ry;
    s.pix_block_size = k_anno_pix_block;
    s.pix_cols = cols;
    s.pix_rows = rows;
    s.pix_blocks.resize((size_t)cols * rows);
    for (int by = 0; by < rows; ++by) {
        for (int bx = 0; bx < cols; ++bx) {
            s.pix_blocks[(size_t)by * cols + bx] = avg_block_rgb(
                annotator_bg_orig_.data(),
                annotator_bg_w_, annotator_bg_h_, annotator_bg_w_ * 4,
                rx + bx * k_anno_pix_block, ry + by * k_anno_pix_block,
                k_anno_pix_block);
        }
    }
}

void Renderer::rebuild_text_preview() {
    // Recreate the in-progress preview texture so it tracks the typed
    // string. Uses the same Win32 helper as every other text label in
    // this file.
    if (annotator_text_preview_tex_) {
        SDL_DestroyTexture(annotator_text_preview_tex_);
        annotator_text_preview_tex_ = nullptr;
    }
    annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
    annotator_text_preview_cached_ = annotator_text_buf_;
    if (annotator_text_buf_.empty() || annotator_text_font_px_ <= 0) return;
    AnnoColor c = k_anno_palette[annotator_color_idx_];
    int tw = 0, th = 0;
    annotator_text_preview_tex_ = make_text_texture(
        sdl_renderer_, annotator_text_buf_, annotator_text_font_px_,
        c.r, c.g, c.b, &tw, &th);
    annotator_text_preview_w_ = tw;
    annotator_text_preview_h_ = th;
}

void Renderer::commit_text_stroke() {
    if (!annotator_text_input_active_) return;
    if (!annotator_text_buf_.empty()) {
        AnnoStroke s;
        s.tool = AnnoTool::Text;
        s.x0 = annotator_text_x_;
        s.y0 = annotator_text_y_;
        s.x1 = annotator_text_x_;
        s.y1 = annotator_text_y_;
        s.text = annotator_text_buf_;
        s.font_px = annotator_text_font_px_;
        s.r = k_anno_palette[annotator_color_idx_].r;
        s.g = k_anno_palette[annotator_color_idx_].g;
        s.b = k_anno_palette[annotator_color_idx_].b;
        // Bake the on-screen texture immediately so the saved PNG and the
        // editor view come from the same glyph source.
        int tw = 0, th = 0;
        s.text_tex = make_text_texture(sdl_renderer_, s.text, s.font_px,
                                       s.r, s.g, s.b, &tw, &th);
        s.text_w = tw;
        s.text_h = th;
        annotator_strokes_.push_back(std::move(s));
    }
    annotator_text_buf_.clear();
    if (annotator_text_preview_tex_) {
        SDL_DestroyTexture(annotator_text_preview_tex_);
        annotator_text_preview_tex_ = nullptr;
    }
    annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
    annotator_text_preview_cached_.clear();
    SDL_StopTextInput();
    annotator_text_input_active_ = false;
}

void Renderer::rasterize_strokes_to(uint8_t* rgba, int w, int h, int stride) const {
    for (const auto& s : annotator_strokes_) {
        int thick = std::max(2, s.thickness);
        switch (s.tool) {
        case AnnoTool::Arrow: {
            // Head — isoceles triangle pointing along the stroke. We compute
            // it first so we know where to stop the shaft.
            float dx = (float)(s.x1 - s.x0);
            float dy = (float)(s.y1 - s.y0);
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) break;
            float ux = dx / len, uy = dy / len;
            float px = -uy, py = ux; // perpendicular
            int head_len = std::max(thick * 4, 12);
            int head_w   = std::max(thick * 3, 9);
            // Shaft as a perpendicular quad so the tail cap is flat/square
            // instead of the diamond shape produced by stacked H/V lines.
            float back_x = s.x1 - ux * head_len * 0.85f;
            float back_y = s.y1 - uy * head_len * 0.85f;
            float hw = thick * 0.5f;
            int s0x = (int)std::round(s.x0  + px * hw);
            int s0y = (int)std::round(s.y0  + py * hw);
            int s1x = (int)std::round(s.x0  - px * hw);
            int s1y = (int)std::round(s.y0  - py * hw);
            int s2x = (int)std::round(back_x - px * hw);
            int s2y = (int)std::round(back_y - py * hw);
            int s3x = (int)std::round(back_x + px * hw);
            int s3y = (int)std::round(back_y + py * hw);
            fill_triangle_rgba(rgba, w, h, stride,
                               s0x, s0y, s1x, s1y, s2x, s2y,
                               s.r, s.g, s.b);
            fill_triangle_rgba(rgba, w, h, stride,
                               s0x, s0y, s2x, s2y, s3x, s3y,
                               s.r, s.g, s.b);
            int bx0 = (int)(s.x1 - ux * head_len + px * head_w);
            int by0 = (int)(s.y1 - uy * head_len + py * head_w);
            int bx1 = (int)(s.x1 - ux * head_len - px * head_w);
            int by1 = (int)(s.y1 - uy * head_len - py * head_w);
            fill_triangle_rgba(rgba, w, h, stride,
                               s.x1, s.y1, bx0, by0, bx1, by1,
                               s.r, s.g, s.b);
            break;
        }
        case AnnoTool::Rect: {
            int rx = std::min(s.x0, s.x1), ry = std::min(s.y0, s.y1);
            int rw = std::abs(s.x1 - s.x0), rh = std::abs(s.y1 - s.y0);
            rect_outline_rgba(rgba, w, h, stride, rx, ry, rw, rh, thick,
                              s.r, s.g, s.b);
            break;
        }
        case AnnoTool::Highlight: {
            int rx = std::min(s.x0, s.x1), ry = std::min(s.y0, s.y1);
            int rw = std::abs(s.x1 - s.x0), rh = std::abs(s.y1 - s.y0);
            filled_rect_blend_rgba(rgba, w, h, stride, rx, ry, rw, rh,
                                   s.r, s.g, s.b, 110);
            break;
        }
        case AnnoTool::Pixelate: {
            for (int by = 0; by < s.pix_rows; ++by) {
                for (int bx = 0; bx < s.pix_cols; ++bx) {
                    uint32_t c = s.pix_blocks[(size_t)by * s.pix_cols + bx];
                    uint8_t r = (c >> 16) & 0xFF;
                    uint8_t g = (c >> 8) & 0xFF;
                    uint8_t b = c & 0xFF;
                    int rx = s.pix_origin_x + bx * s.pix_block_size;
                    int ry = s.pix_origin_y + by * s.pix_block_size;
                    int x0 = std::max(0, rx);
                    int y0 = std::max(0, ry);
                    int x1 = std::min(w, rx + s.pix_block_size);
                    int y1 = std::min(h, ry + s.pix_block_size);
                    for (int y = y0; y < y1; ++y) {
                        uint8_t* row = rgba + y * stride;
                        for (int x = x0; x < x1; ++x) {
                            uint8_t* p = row + x * 4;
                            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                        }
                    }
                }
            }
            break;
        }
        case AnnoTool::Text: {
            if (s.text.empty() || s.font_px <= 0) break;
            std::vector<uint8_t> glyph;
            int gw = 0, gh = 0;
            if (make_text_rgba_buffer(s.text, s.font_px, s.r, s.g, s.b,
                                      glyph, gw, gh)) {
                blit_rgba_blend(rgba, w, h, stride, glyph.data(), gw, gh,
                                s.x0, s.y0);
            }
            break;
        }
        }
    }
}

void Renderer::save_annotated() {
    if (!annotator_active_ || annotator_bg_orig_.empty()) return;

    // Flush any in-progress text so it ends up in the saved image.
    if (annotator_text_input_active_) commit_text_stroke();

    const bool save = settings_.screenshot_save_to_folder;
    const bool clip = settings_.screenshot_copy_to_clipboard;
    const bool snag = settings_.screenshot_open_in_snagit;
    if (!save && !clip && !snag) {
        toast_text_ = "Screenshot disabled in Settings";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    // Bake strokes into a fresh copy of the original capture.
    std::vector<uint8_t> baked = annotator_bg_orig_;
    rasterize_strokes_to(baked.data(), annotator_bg_w_, annotator_bg_h_,
                         annotator_bg_w_ * 4);

    bool wrote = true;
    std::string filename;
    std::string snagit_path;
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
    if (save) {
        std::filesystem::create_directories(screenshot_dir_);
        filename = screenshot_dir_ + "/annotated_" + timestamp + ".png";
        wrote = stbi_write_png(filename.c_str(), annotator_bg_w_, annotator_bg_h_,
                               4, baked.data(), annotator_bg_w_ * 4) != 0;
    } else if (snag) {
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (!ec) {
            snagit_path = (tmp / ("1PhoneMirror_annot_" + std::string(timestamp) + ".png")).string();
            wrote = stbi_write_png(snagit_path.c_str(), annotator_bg_w_, annotator_bg_h_,
                                   4, baked.data(), annotator_bg_w_ * 4) != 0;
        }
    }
    if (wrote && clip) {
        copy_to_clipboard(baked.data(), annotator_bg_w_, annotator_bg_h_);
    }
    if (wrote && snag) {
        open_in_snagit(save ? filename : snagit_path);
    }
    if (wrote) {
        std::string parts;
        auto add = [&](const char* p){ if (!parts.empty()) parts += " + "; parts += p; };
        if (save) add("saved");
        if (clip) add("copied");
        if (snag) add("Snagit");
        toast_text_ = "Annotated: " + parts;
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        if (save) std::cout << "[Annotate] Saved: " << filename << "\n";
    }
    end_annotation();
}

void Renderer::draw_annotator() {
    if (!annotator_active_ || !annotator_bg_tex_) return;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    // Restrict the overlay to the PHONE area only (exclude the log drawer
    // if it is open) so the dim backdrop, picture and toolbar all stay
    // within the phone window and the user can still see/use the log on
    // the right. Same recipe as draw_ocr_overlay().
    int log_w = (int)(log_panel_full_w_ * log_panel_anim_);
    int phone_area_w = std::max(64, win_w - log_w);

    // Dim backdrop covering the phone area (the log drawer remains visible).
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 235);
    SDL_Rect full{0, 0, phone_area_w, win_h};
    SDL_RenderFillRect(sdl_renderer_, &full);

    // Toolbar reserves a strip at the top.
    int toolbar_h = std::max(44, win_h / 14);
    int img_top = toolbar_h + 12;
    int img_bottom_pad = 12;
    int avail_w = std::max(64, phone_area_w - 24);
    int avail_h = std::max(64, win_h - img_top - img_bottom_pad);
    float sx = (float)avail_w / annotator_bg_w_;
    float sy = (float)avail_h / annotator_bg_h_;
    float s = std::min(sx, sy);
    annotator_dst_w_ = (int)(annotator_bg_w_ * s);
    annotator_dst_h_ = (int)(annotator_bg_h_ * s);
    annotator_dst_x_ = (phone_area_w - annotator_dst_w_) / 2;
    annotator_dst_y_ = img_top + (avail_h - annotator_dst_h_) / 2;

    // Background image.
    SDL_Rect img_dst{annotator_dst_x_, annotator_dst_y_,
                     annotator_dst_w_, annotator_dst_h_};
    SDL_RenderCopy(sdl_renderer_, annotator_bg_tex_, nullptr, &img_dst);

    // Map image-space pixel coords onto the displayed rect.
    auto img_to_screen = [&](int ix, int iy, int& ox, int& oy) {
        ox = annotator_dst_x_ + (int)(ix * s);
        oy = annotator_dst_y_ + (int)(iy * s);
    };
    // Convert an image-space line width into screen-space pixels.
    auto img_thick_to_screen = [&](int img_thick) {
        return std::max(2, (int)std::round(img_thick * s));
    };

    auto draw_arrow_screen = [&](int x0, int y0, int x1, int y1, int thick,
                                 uint8_t r, uint8_t g, uint8_t b) {
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) return;
        float ux = dx / len, uy = dy / len;
        float px = -uy, py = ux;
        int head_len = std::max(thick * 4, 12);
        int head_w   = std::max(thick * 3, 9);
        // Stop the shaft at the base of the head so the line never pokes
        // through the tip. Pull back by ~80% of head_len which leaves a
        // small overlap that hides any rounding gap.
        float back_x = x1 - ux * head_len * 0.85f;
        float back_y = y1 - uy * head_len * 0.85f;

        // Generic scanline fill for an arbitrary triangle. Used both for
        // the shaft (split into two tris) and the arrow head.
        auto edge = [](float Ax, float Ay, float Bx, float By, float Px, float Py) {
            return (Bx - Ax) * (Py - Ay) - (By - Ay) * (Px - Ax);
        };
        auto fill_tri = [&](float ax, float ay, float bx, float by,
                            float cx2, float cy2) {
            int min_x = (int)std::floor(std::min({ax, bx, cx2}));
            int max_x = (int)std::ceil (std::max({ax, bx, cx2}));
            int min_y = (int)std::floor(std::min({ay, by, cy2}));
            int max_y = (int)std::ceil (std::max({ay, by, cy2}));
            for (int yy = min_y; yy <= max_y; ++yy) {
                for (int xx = min_x; xx <= max_x; ++xx) {
                    float w0 = edge(bx, by, cx2, cy2, (float)xx, (float)yy);
                    float w1 = edge(cx2, cy2, ax, ay,  (float)xx, (float)yy);
                    float w2 = edge(ax, ay, bx, by,    (float)xx, (float)yy);
                    if ((w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                        (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                        SDL_RenderDrawPoint(sdl_renderer_, xx, yy);
                    }
                }
            }
        };

        // Shaft as a perpendicular quad, giving a flat (square) tail cap.
        float hw = thick * 0.5f;
        float s0x = x0  + px * hw, s0y = y0  + py * hw;
        float s1x = x0  - px * hw, s1y = y0  - py * hw;
        float s2x = back_x - px * hw, s2y = back_y - py * hw;
        float s3x = back_x + px * hw, s3y = back_y + py * hw;
        fill_tri(s0x, s0y, s1x, s1y, s2x, s2y);
        fill_tri(s0x, s0y, s2x, s2y, s3x, s3y);

        // Solid arrow head: filled triangle with apex at the tip and base
        // perpendicular to the shaft `head_len` behind it.
        float ax = (float)x1, ay = (float)y1;
        float bx = x1 - ux * head_len + px * head_w;
        float by = y1 - uy * head_len + py * head_w;
        float cx2 = x1 - ux * head_len - px * head_w;
        float cy2 = y1 - uy * head_len - py * head_w;
        fill_tri(ax, ay, bx, by, cx2, cy2);
    };

    auto draw_rect_screen = [&](int x0, int y0, int x1, int y1, int thick,
                                uint8_t r, uint8_t g, uint8_t b) {
        int rx = std::min(x0, x1), ry = std::min(y0, y1);
        int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        for (int t = 0; t < thick; ++t) {
            SDL_Rect rr{rx - t / 2, ry - t / 2, rw + t, rh + t};
            SDL_RenderDrawRect(sdl_renderer_, &rr);
        }
    };

    auto draw_highlight_screen = [&](int x0, int y0, int x1, int y1,
                                     uint8_t r, uint8_t g, uint8_t b) {
        int rx = std::min(x0, x1), ry = std::min(y0, y1);
        int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 110);
        SDL_Rect rr{rx, ry, rw, rh};
        SDL_RenderFillRect(sdl_renderer_, &rr);
    };

    auto draw_pixelate_screen = [&](const AnnoStroke& st) {
        for (int by = 0; by < st.pix_rows; ++by) {
            for (int bx = 0; bx < st.pix_cols; ++bx) {
                uint32_t c = st.pix_blocks[(size_t)by * st.pix_cols + bx];
                uint8_t r = (c >> 16) & 0xFF;
                uint8_t g = (c >> 8) & 0xFF;
                uint8_t b = c & 0xFF;
                int ix = st.pix_origin_x + bx * st.pix_block_size;
                int iy = st.pix_origin_y + by * st.pix_block_size;
                int sxp, syp;
                img_to_screen(ix, iy, sxp, syp);
                int sw = std::max(1, (int)(st.pix_block_size * s + 1));
                int sh = std::max(1, (int)(st.pix_block_size * s + 1));
                SDL_Rect rr{sxp, syp, sw, sh};
                SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
                SDL_RenderFillRect(sdl_renderer_, &rr);
            }
        }
    };

    // Replay committed strokes on top of the background.
    SDL_RenderSetClipRect(sdl_renderer_, &img_dst);
    for (const auto& st : annotator_strokes_) {
        int sx0, sy0, sx1, sy1;
        img_to_screen(st.x0, st.y0, sx0, sy0);
        img_to_screen(st.x1, st.y1, sx1, sy1);
        int thk = img_thick_to_screen(st.thickness);
        switch (st.tool) {
        case AnnoTool::Arrow:     draw_arrow_screen(sx0, sy0, sx1, sy1, thk, st.r, st.g, st.b); break;
        case AnnoTool::Rect:      draw_rect_screen(sx0, sy0, sx1, sy1, thk, st.r, st.g, st.b); break;
        case AnnoTool::Highlight: draw_highlight_screen(sx0, sy0, sx1, sy1, st.r, st.g, st.b); break;
        case AnnoTool::Pixelate:  draw_pixelate_screen(st); break;
        case AnnoTool::Text: {
            if (!st.text_tex) break;
            // Scale the glyph texture so it grows/shrinks with the editor.
            int dw = std::max(1, (int)std::round(st.text_w * s));
            int dh = std::max(1, (int)std::round(st.text_h * s));
            SDL_Rect td{sx0, sy0, dw, dh};
            SDL_RenderCopy(sdl_renderer_, st.text_tex, nullptr, &td);
            break;
        }
        }
    }

    // Live preview of the in-progress stroke.
    if (annotator_drawing_) {
        AnnoColor c = k_anno_palette[annotator_color_idx_];
        int sx0, sy0, sx1, sy1;
        img_to_screen(annotator_drag_x0_, annotator_drag_y0_, sx0, sy0);
        img_to_screen(annotator_drag_x1_, annotator_drag_y1_, sx1, sy1);
        int thk = img_thick_to_screen(annotator_line_width_);
        switch (annotator_tool_) {
        case AnnoTool::Arrow:     draw_arrow_screen(sx0, sy0, sx1, sy1, thk, c.r, c.g, c.b); break;
        case AnnoTool::Rect:      draw_rect_screen(sx0, sy0, sx1, sy1, thk, c.r, c.g, c.b); break;
        case AnnoTool::Highlight: draw_highlight_screen(sx0, sy0, sx1, sy1, c.r, c.g, c.b); break;
        case AnnoTool::Pixelate: {
            // Show a translucent marker rect while dragging — actual mosaic
            // is computed on commit.
            int rx = std::min(sx0, sx1), ry = std::min(sy0, sy1);
            int rw = std::abs(sx1 - sx0), rh = std::abs(sy1 - sy0);
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 120);
            SDL_Rect rr{rx, ry, rw, rh};
            SDL_RenderFillRect(sdl_renderer_, &rr);
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 220);
            SDL_RenderDrawRect(sdl_renderer_, &rr);
            break;
        }
        case AnnoTool::Text: break; // Text uses its own preview path below.
        }
    }

    // Live preview of the in-progress text stroke (caret + glyphs).
    if (annotator_text_input_active_) {
        int sx0, sy0;
        img_to_screen(annotator_text_x_, annotator_text_y_, sx0, sy0);
        if (annotator_text_preview_tex_) {
            int dw = std::max(1, (int)std::round(annotator_text_preview_w_ * s));
            int dh = std::max(1, (int)std::round(annotator_text_preview_h_ * s));
            SDL_Rect td{sx0, sy0, dw, dh};
            SDL_RenderCopy(sdl_renderer_, annotator_text_preview_tex_, nullptr, &td);
            // Blinking caret after the last glyph.
            int caret_x = sx0 + dw + 1;
            int caret_h = dh;
            uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((ms / 500) % 2 == 0) {
                AnnoColor c = k_anno_palette[annotator_color_idx_];
                SDL_SetRenderDrawColor(sdl_renderer_, c.r, c.g, c.b, 255);
                SDL_Rect cr{caret_x, sy0, std::max(1, (int)std::round(s)), caret_h};
                SDL_RenderFillRect(sdl_renderer_, &cr);
            }
        } else {
            // Empty buffer — just show a caret so the user knows where they are.
            int caret_h = std::max(8, (int)std::round(annotator_text_font_px_ * s));
            uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((ms / 500) % 2 == 0) {
                AnnoColor c = k_anno_palette[annotator_color_idx_];
                SDL_SetRenderDrawColor(sdl_renderer_, c.r, c.g, c.b, 255);
                SDL_Rect cr{sx0, sy0, std::max(1, (int)std::round(s)), caret_h};
                SDL_RenderFillRect(sdl_renderer_, &cr);
            }
        }
    }
    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // ---- Toolbar (centered, top of window) ----
    // Lay out the toolbar with explicit slot counts and shrink everything
    // proportionally if it would overflow the window. This keeps every
    // button reachable even on a narrow phone-shaped window.
    const int n_tools = 5, n_swatches = 5, n_actions = 3;
    int btn_sz = std::max(24, toolbar_h - 8);
    int gap = std::max(3, btn_sz / 6);
    int swatch_sz = std::max(18, btn_sz - 8);
    int slider_w = btn_sz * 5;
    auto compute_total = [&]() {
        return n_tools * btn_sz + (n_tools - 1) * gap + gap * 3
             + slider_w + gap * 3
             + n_swatches * swatch_sz + (n_swatches - 1) * gap + gap * 3
             + n_actions * btn_sz + (n_actions - 1) * gap;
    };
    int avail = std::max(64, phone_area_w - 16);
    int total_w = compute_total();
    if (total_w > avail) {
        // Scale all slot sizes down to fit. Recompute gaps from the new
        // btn_sz so visual proportions stay consistent.
        float s2 = (float)avail / total_w;
        btn_sz   = std::max(20, (int)(btn_sz * s2));
        swatch_sz = std::max(16, (int)(swatch_sz * s2));
        gap      = std::max(2, (int)(gap * s2));
        slider_w = std::max(60, (int)(slider_w * s2));
        total_w = compute_total();
        // After flooring, may still overflow by a couple px — just clamp.
        if (total_w > avail) total_w = avail;
    }
    int tx = (phone_area_w - total_w) / 2;
    if (tx < 8) tx = 8;
    int ty = (toolbar_h - btn_sz) / 2;

    int mx, my;
    SDL_GetMouseState(&mx, &my);
    auto in = [&](int x, int y, int w, int h) {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    };

    auto draw_tool_btn = [&](BtnRect& slot, AnnoTool tool, const char* glyph_kind) {
        slot = {tx, ty, btn_sz, btn_sz};
        bool selected = (annotator_tool_ == tool);
        bool hov = in(tx, ty, btn_sz, btn_sz);
        uint8_t bg = selected ? 90 : (hov ? 70 : 45);
        SDL_SetRenderDrawColor(sdl_renderer_, bg, bg, bg + 8, 235);
        SDL_Rect br{tx, ty, btn_sz, btn_sz};
        SDL_RenderFillRect(sdl_renderer_, &br);
        if (selected) {
            SDL_SetRenderDrawColor(sdl_renderer_, 200, 200, 220, 255);
            SDL_RenderDrawRect(sdl_renderer_, &br);
        }
        // Tiny glyph for each tool — kept primitive-only so the toolbar
        // stays readable without bringing in icon assets.
        int cx = tx + btn_sz / 2, cy = ty + btn_sz / 2;
        int g = btn_sz / 3;
        SDL_SetRenderDrawColor(sdl_renderer_, 235, 235, 240, 255);
        if (std::string(glyph_kind) == "arrow") {
            SDL_RenderDrawLine(sdl_renderer_, cx - g, cy + g, cx + g, cy - g);
            SDL_RenderDrawLine(sdl_renderer_, cx + g, cy - g, cx + g - g/2, cy - g);
            SDL_RenderDrawLine(sdl_renderer_, cx + g, cy - g, cx + g, cy - g + g/2);
        } else if (std::string(glyph_kind) == "rect") {
            SDL_Rect r{cx - g, cy - g/2 - 1, g * 2, g + 2};
            SDL_RenderDrawRect(sdl_renderer_, &r);
        } else if (std::string(glyph_kind) == "highlight") {
            SDL_SetRenderDrawColor(sdl_renderer_, 245, 205, 60, 160);
            SDL_Rect r{cx - g, cy - g/2 - 1, g * 2, g + 2};
            SDL_RenderFillRect(sdl_renderer_, &r);
        } else if (std::string(glyph_kind) == "pixelate") {
            // 2x2 mini mosaic.
            int q = g;
            SDL_SetRenderDrawColor(sdl_renderer_, 200, 200, 200, 255);
            SDL_Rect r0{cx - q, cy - q, q, q};
            SDL_RenderFillRect(sdl_renderer_, &r0);
            SDL_SetRenderDrawColor(sdl_renderer_, 130, 130, 130, 255);
            SDL_Rect r1{cx,     cy - q, q, q};
            SDL_RenderFillRect(sdl_renderer_, &r1);
            SDL_SetRenderDrawColor(sdl_renderer_, 160, 160, 160, 255);
            SDL_Rect r2{cx - q, cy,     q, q};
            SDL_RenderFillRect(sdl_renderer_, &r2);
            SDL_SetRenderDrawColor(sdl_renderer_, 100, 100, 100, 255);
            SDL_Rect r3{cx,     cy,     q, q};
            SDL_RenderFillRect(sdl_renderer_, &r3);
        } else if (std::string(glyph_kind) == "text") {
            // Stylised "T" — top bar + vertical stem.
            SDL_Rect bar{cx - g, cy - g, g * 2, std::max(2, g / 3)};
            SDL_RenderFillRect(sdl_renderer_, &bar);
            SDL_Rect stem{cx - std::max(1, g / 4), cy - g,
                          std::max(2, g / 2), g * 2};
            SDL_RenderFillRect(sdl_renderer_, &stem);
        }
        tx += btn_sz + gap;
    };
    draw_tool_btn(anno_btn_arrow_,     AnnoTool::Arrow,     "arrow");
    draw_tool_btn(anno_btn_rect_,      AnnoTool::Rect,      "rect");
    draw_tool_btn(anno_btn_highlight_, AnnoTool::Highlight, "highlight");
    draw_tool_btn(anno_btn_pixelate_,  AnnoTool::Pixelate,  "pixelate");
    draw_tool_btn(anno_btn_text_,      AnnoTool::Text,      "text");

    tx += gap * 3;

    // ---- Line-width slider ----
    // Track range maps to image-space pixels [k_min..k_max]. Affects
    // Arrow + Rect strokes (and the in-progress preview); Text uses it
    // as the glyph height.
    constexpr int k_lw_min = 1, k_lw_max = 32;
    int track_y = ty + btn_sz / 2;
    int track_h = std::max(3, btn_sz / 8);
    SDL_SetRenderDrawColor(sdl_renderer_, 60, 60, 70, 235);
    SDL_Rect track{tx, track_y - track_h / 2, slider_w, track_h};
    SDL_RenderFillRect(sdl_renderer_, &track);
    // Filled portion up to handle.
    float lw_t = (float)(annotator_line_width_ - k_lw_min) / (k_lw_max - k_lw_min);
    lw_t = std::clamp(lw_t, 0.0f, 1.0f);
    int handle_x = tx + (int)(lw_t * slider_w);
    SDL_SetRenderDrawColor(sdl_renderer_, 120, 160, 220, 255);
    SDL_Rect filled{tx, track_y - track_h / 2, handle_x - tx, track_h};
    SDL_RenderFillRect(sdl_renderer_, &filled);
    // Handle (circle approximated by filled rect — keeps it primitive).
    int handle_r = std::max(5, btn_sz / 4);
    SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 230, 255);
    SDL_Rect handle{handle_x - handle_r, track_y - handle_r,
                    handle_r * 2, handle_r * 2};
    SDL_RenderFillRect(sdl_renderer_, &handle);
    SDL_SetRenderDrawColor(sdl_renderer_, 40, 40, 50, 255);
    SDL_RenderDrawRect(sdl_renderer_, &handle);
    // Numeric value sits to the right of the track for quick feedback.
    {
        char val_buf[12];
        std::snprintf(val_buf, sizeof(val_buf), "%d", annotator_line_width_);
        int vw = 0, vh = 0;
        SDL_Texture* vt = make_text_texture(sdl_renderer_, val_buf,
                                            std::max(10, btn_sz / 2),
                                            220, 220, 230, &vw, &vh);
        if (vt) {
            SDL_Rect vd{tx + slider_w - vw - 4, track_y - vh / 2, vw, vh};
            SDL_RenderCopy(sdl_renderer_, vt, nullptr, &vd);
            SDL_DestroyTexture(vt);
        }
    }
    anno_slider_rect_ = {tx, ty, slider_w, btn_sz};
    tx += slider_w + gap * 3;

    // Colour swatches.
    int sw_y = ty + (btn_sz - swatch_sz) / 2;
    for (int i = 0; i < 5; ++i) {
        anno_swatch_btns_[i] = {tx, sw_y, swatch_sz, swatch_sz};
        bool selected = (annotator_color_idx_ == i);
        SDL_SetRenderDrawColor(sdl_renderer_,
                               k_anno_palette[i].r, k_anno_palette[i].g,
                               k_anno_palette[i].b, 255);
        SDL_Rect r{tx, sw_y, swatch_sz, swatch_sz};
        SDL_RenderFillRect(sdl_renderer_, &r);
        SDL_SetRenderDrawColor(sdl_renderer_, selected ? 255 : 60,
                               selected ? 255 : 60, selected ? 255 : 60, 255);
        SDL_Rect rr{tx - 1, sw_y - 1, swatch_sz + 2, swatch_sz + 2};
        SDL_RenderDrawRect(sdl_renderer_, &rr);
        tx += swatch_sz + gap;
    }
    tx += gap * 3;

    auto draw_action_btn = [&](BtnRect& slot, const char* glyph_kind,
                               uint8_t r, uint8_t g, uint8_t b) {
        slot = {tx, ty, btn_sz, btn_sz};
        bool hov = in(tx, ty, btn_sz, btn_sz);
        SDL_SetRenderDrawColor(sdl_renderer_, hov ? 80 : 50, hov ? 80 : 50,
                               hov ? 90 : 58, 235);
        SDL_Rect br{tx, ty, btn_sz, btn_sz};
        SDL_RenderFillRect(sdl_renderer_, &br);
        int cx = tx + btn_sz / 2, cy = ty + btn_sz / 2;
        int gg = btn_sz / 3;
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        if (std::string(glyph_kind) == "undo") {
            // Left-pointing curved-back arrow: head on the left, tail loops
            // up and over to the right. Mirrored from the previous version
            // because "undo" reads more naturally as a leftward motion.
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy - gg/2, cx + gg/2, cy - gg/2);
            SDL_RenderDrawLine(sdl_renderer_, cx + gg/2, cy - gg/2, cx + gg, cy);
            SDL_RenderDrawLine(sdl_renderer_, cx + gg, cy, cx + gg/2, cy + gg/2);
            // Arrow head wedge at the left tip.
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy - gg/2, cx - gg + gg/3, cy - gg);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy - gg/2, cx - gg + gg/3, cy);
        } else if (std::string(glyph_kind) == "save") {
            // Checkmark.
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy, cx - gg/3, cy + gg);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg/3, cy + gg, cx + gg, cy - gg);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy + 1, cx - gg/3, cy + gg + 1);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg/3, cy + gg + 1, cx + gg, cy - gg + 1);
        } else if (std::string(glyph_kind) == "cancel") {
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy - gg, cx + gg, cy + gg);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy + gg, cx + gg, cy - gg);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy - gg + 1, cx + gg, cy + gg + 1);
            SDL_RenderDrawLine(sdl_renderer_, cx - gg, cy + gg - 1, cx + gg, cy - gg - 1);
        }
        tx += btn_sz + gap;
    };
    draw_action_btn(anno_btn_undo_,   "undo",   220, 220, 220);
    draw_action_btn(anno_btn_save_,   "save",    90, 220, 110);
    draw_action_btn(anno_btn_cancel_, "cancel", 235, 110, 110);
}

bool Renderer::handle_annotator_event(const SDL_Event& ev) {
    if (!annotator_active_) return false;

    auto screen_to_image = [&](int sx, int sy, int& ix, int& iy) {
        if (annotator_dst_w_ <= 0 || annotator_dst_h_ <= 0) { ix = iy = 0; return false; }
        float fx = (float)(sx - annotator_dst_x_) * annotator_bg_w_ / annotator_dst_w_;
        float fy = (float)(sy - annotator_dst_y_) * annotator_bg_h_ / annotator_dst_h_;
        ix = (int)std::round(fx);
        iy = (int)std::round(fy);
        return ix >= 0 && iy >= 0 && ix < annotator_bg_w_ && iy < annotator_bg_h_;
    };

    auto in = [](int mx, int my, const BtnRect& r) {
        return r.w > 0 && mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
    };

    constexpr int k_lw_min = 1, k_lw_max = 32;
    auto slider_value_from_x = [&](int sx) {
        int rel = std::clamp(sx - anno_slider_rect_.x, 0, anno_slider_rect_.w);
        float t = (float)rel / std::max(1, anno_slider_rect_.w);
        return std::clamp((int)std::round(k_lw_min + t * (k_lw_max - k_lw_min)),
                          k_lw_min, k_lw_max);
    };

    // ---- Text input mode ----
    // While the user is typing into a text stroke, most keyboard events
    // belong to the input rather than to tool shortcuts.
    if (annotator_text_input_active_) {
        if (ev.type == SDL_TEXTINPUT) {
            annotator_text_buf_ += ev.text.text;
            rebuild_text_preview();
            return true;
        }
        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode k = ev.key.keysym.sym;
            if (k == SDLK_BACKSPACE) {
                if (!annotator_text_buf_.empty()) {
                    // Strip one byte; safe for ASCII typing. Multi-byte
                    // UTF-8 sequences can leave a partial codepoint, but
                    // the next render call just re-bakes the buffer.
                    annotator_text_buf_.pop_back();
                    // Remove any trailing UTF-8 continuation bytes.
                    while (!annotator_text_buf_.empty() &&
                           ((uint8_t)annotator_text_buf_.back() & 0xC0) == 0x80) {
                        annotator_text_buf_.pop_back();
                    }
                    rebuild_text_preview();
                }
                return true;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                commit_text_stroke();
                return true;
            }
            if (k == SDLK_ESCAPE) {
                // Cancel the in-progress text without leaving the editor.
                annotator_text_buf_.clear();
                if (annotator_text_preview_tex_) {
                    SDL_DestroyTexture(annotator_text_preview_tex_);
                    annotator_text_preview_tex_ = nullptr;
                }
                annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
                annotator_text_preview_cached_.clear();
                SDL_StopTextInput();
                annotator_text_input_active_ = false;
                return true;
            }
            // Swallow other keys (no tool shortcuts while typing).
            return true;
        }
        // Mouse handling continues below — clicking the toolbar or
        // outside the current text commits and starts a new operation.
    }

    if (ev.type == SDL_KEYDOWN) {
        SDL_Keymod m = SDL_GetModState();
        if (ev.key.keysym.sym == SDLK_ESCAPE) {
            std::cout << "[Annotate] Cancelled\n";
            end_annotation();
            return true;
        }
        if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) {
            save_annotated();
            return true;
        }
        if ((m & KMOD_CTRL) && ev.key.keysym.sym == SDLK_z) {
            if (!annotator_strokes_.empty()) {
                if (annotator_strokes_.back().text_tex) {
                    SDL_DestroyTexture(annotator_strokes_.back().text_tex);
                }
                annotator_strokes_.pop_back();
            }
            return true;
        }
        // Tool shortcuts.
        if (ev.key.keysym.sym == SDLK_a) { annotator_tool_ = AnnoTool::Arrow;     return true; }
        if (ev.key.keysym.sym == SDLK_r) { annotator_tool_ = AnnoTool::Rect;      return true; }
        if (ev.key.keysym.sym == SDLK_h) { annotator_tool_ = AnnoTool::Highlight; return true; }
        if (ev.key.keysym.sym == SDLK_p) { annotator_tool_ = AnnoTool::Pixelate;  return true; }
        if (ev.key.keysym.sym == SDLK_t) { annotator_tool_ = AnnoTool::Text;      return true; }
        // [/] adjust line width without touching the slider.
        if (ev.key.keysym.sym == SDLK_LEFTBRACKET) {
            annotator_line_width_ = std::max(k_lw_min, annotator_line_width_ - 1);
            return true;
        }
        if (ev.key.keysym.sym == SDLK_RIGHTBRACKET) {
            annotator_line_width_ = std::min(k_lw_max, annotator_line_width_ + 1);
            return true;
        }
        return true; // swallow everything else while modal
    }

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        int mx = ev.button.x, my = ev.button.y;
        // Toolbar hits first.
        if (in(mx, my, anno_btn_arrow_))     { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Arrow;     return true; }
        if (in(mx, my, anno_btn_rect_))      { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Rect;      return true; }
        if (in(mx, my, anno_btn_highlight_)) { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Highlight; return true; }
        if (in(mx, my, anno_btn_pixelate_))  { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Pixelate;  return true; }
        if (in(mx, my, anno_btn_text_))      { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Text;      return true; }
        // Slider — start a drag and snap the value to the cursor.
        if (in(mx, my, anno_slider_rect_)) {
            annotator_slider_drag_ = true;
            annotator_line_width_ = slider_value_from_x(mx);
            // Live preview gets re-baked at new size on next render.
            return true;
        }
        for (int i = 0; i < 5; ++i) {
            if (in(mx, my, anno_swatch_btns_[i])) {
                annotator_color_idx_ = i;
                if (annotator_text_input_active_) rebuild_text_preview();
                return true;
            }
        }
        if (in(mx, my, anno_btn_undo_)) {
            if (!annotator_strokes_.empty()) {
                if (annotator_strokes_.back().text_tex) {
                    SDL_DestroyTexture(annotator_strokes_.back().text_tex);
                }
                annotator_strokes_.pop_back();
            }
            return true;
        }
        if (in(mx, my, anno_btn_save_))   { save_annotated();   return true; }
        if (in(mx, my, anno_btn_cancel_)) { end_annotation();   return true; }

        // Otherwise click is inside the image area.
        int ix, iy;
        if (screen_to_image(mx, my, ix, iy)) {
            if (annotator_tool_ == AnnoTool::Text) {
                // Commit any prior in-progress text and start a new one
                // anchored at the cursor.
                if (annotator_text_input_active_) commit_text_stroke();
                annotator_text_input_active_ = true;
                annotator_text_x_ = ix;
                annotator_text_y_ = iy;
                // Map the line-width slider to a glyph height, with a
                // minimum so even thickness=1 produces legible text.
                annotator_text_font_px_ = std::max(14, annotator_line_width_ * 6);
                annotator_text_buf_.clear();
                rebuild_text_preview();
                SDL_StartTextInput();
            } else {
                if (annotator_text_input_active_) commit_text_stroke();
                annotator_drawing_ = true;
                annotator_drag_x0_ = annotator_drag_x1_ = ix;
                annotator_drag_y0_ = annotator_drag_y1_ = iy;
            }
        } else if (annotator_text_input_active_) {
            // Click outside the image while typing also commits.
            commit_text_stroke();
        }
        return true;
    }

    if (ev.type == SDL_MOUSEMOTION) {
        if (annotator_slider_drag_) {
            annotator_line_width_ = slider_value_from_x(ev.motion.x);
            return true;
        }
        if (annotator_drawing_) {
            int ix, iy;
            screen_to_image(ev.motion.x, ev.motion.y, ix, iy);
            // Allow dragging slightly outside the image — clamp to bounds.
            ix = std::clamp(ix, 0, annotator_bg_w_ - 1);
            iy = std::clamp(iy, 0, annotator_bg_h_ - 1);
            annotator_drag_x1_ = ix;
            annotator_drag_y1_ = iy;
            return true;
        }
    }

    if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
        if (annotator_slider_drag_) {
            annotator_slider_drag_ = false;
            return true;
        }
        if (annotator_drawing_) {
            annotator_drawing_ = false;
            // Discard zero-length drags so a stray click doesn't litter the
            // stroke list with invisible entries.
            if (std::abs(annotator_drag_x1_ - annotator_drag_x0_) < 3 &&
                std::abs(annotator_drag_y1_ - annotator_drag_y0_) < 3) {
                return true;
            }
            AnnoStroke s;
            s.tool = annotator_tool_;
            s.x0 = annotator_drag_x0_; s.y0 = annotator_drag_y0_;
            s.x1 = annotator_drag_x1_; s.y1 = annotator_drag_y1_;
            s.r = k_anno_palette[annotator_color_idx_].r;
            s.g = k_anno_palette[annotator_color_idx_].g;
            s.b = k_anno_palette[annotator_color_idx_].b;
            s.thickness = annotator_line_width_;
            if (s.tool == AnnoTool::Pixelate) bake_pixelate_stroke(s);
            annotator_strokes_.push_back(std::move(s));
            return true;
        }
    }

    // Keep modal: swallow all other events (mouse wheel, right-click etc).
    if (ev.type == SDL_MOUSEWHEEL || ev.type == SDL_MOUSEBUTTONDOWN ||
        ev.type == SDL_MOUSEBUTTONUP) {
        return true;
    }
    return false;
}

void Renderer::show_toast(const std::string& text, int duration_ms) {
    toast_text_ = text;
    toast_duration_ms_ = std::max(600, duration_ms);
    toast_active_ = true;
    toast_start_ = std::chrono::steady_clock::now();
}

// Sample a sparse grid of pixels in last_frame_data_ and report whether
// every sample is "near black". Used to detect FLAG_SECURE / lock-screen
// content: Android's MediaProjection capture delivers an all-black surface
// for protected windows and the framework deliberately hides the cause.
// We sample only ~16x16 = 256 pixels regardless of resolution (effectively
// free at 1080p+) and require an extremely dark threshold so genuine
// dark-mode UI won't trigger the overlay.
bool Renderer::is_frame_near_black() const {
    if (last_frame_data_.empty() || last_frame_w_ <= 0 || last_frame_h_ <= 0) {
        return false;
    }
    constexpr int kGrid = 16;
    constexpr int kThreshold = 12; // 0..255 per channel
    // Restrict sampling to the central 60% of the frame so the top status
    // bar (clock, battery, signal icons) and bottom nav bar/keyboard
    // suggestion strip — which remain bright even when an app uses
    // FLAG_SECURE on its own window — don't disqualify an otherwise
    // blacked-out content area.
    const int x0 = last_frame_w_ * 20 / 100;
    const int x1 = last_frame_w_ * 80 / 100;
    const int y0 = last_frame_h_ * 20 / 100;
    const int y1 = last_frame_h_ * 80 / 100;
    const int rw = std::max(1, x1 - x0);
    const int rh = std::max(1, y1 - y0);
    const uint8_t* p = last_frame_data_.data();
    for (int gy = 0; gy < kGrid; ++gy) {
        int y = y0 + (gy * 2 + 1) * rh / (kGrid * 2);
        if (y >= last_frame_h_) y = last_frame_h_ - 1;
        const uint8_t* row = p + (size_t)y * last_frame_stride_;
        for (int gx = 0; gx < kGrid; ++gx) {
            int x = x0 + (gx * 2 + 1) * rw / (kGrid * 2);
            if (x >= last_frame_w_) x = last_frame_w_ - 1;
            const uint8_t* px = row + x * 4;
            if (px[0] > kThreshold || px[1] > kThreshold || px[2] > kThreshold) {
                return false;
            }
        }
    }
    return true;
}

void Renderer::check_for_update_async(bool show_when_up_to_date) {
    bool expected = false;
    if (!update_check_in_progress_.compare_exchange_strong(expected, true)) {
        // Already running — silently coalesce.
        return;
    }
    // Format the running version once on the calling thread; the worker
    // thread only touches its own locals plus the renderer fields it owns.
    char ver[32];
    std::snprintf(ver, sizeof(ver), "%d.%d.%d",
                  OPM_VERSION_MAJOR,
                  OPM_VERSION_MINOR,
                  OPM_VERSION_PATCH);
    std::string current = ver;

    std::thread([this, current, show_when_up_to_date]() {
        auto result = opm::network::check_for_update(current);
        {
            std::lock_guard<std::mutex> lk(update_check_mutex_);
            update_latest_version_ = result.latest_version;
            update_release_url_    = result.release_url;
        }
        // Surface the outcome via the existing toast banner. The renderer
        // event loop polls toast state, so just setting these strings from
        // a worker thread is safe enough for our purposes (single writer
        // because update_check_in_progress_ is the latch).
        if (!result.ok) {
            if (show_when_up_to_date) {
                show_toast("Update check failed (no internet?)", 3000);
            }
            // Silent on launch when the network is unreachable.
        } else if (result.update_available) {
            // Persistent two-line banner with a clickable GitHub link
            // (replaces the previous single-line toast). The renderer
            // event loop will pick up the new state next frame.
            update_banner_active_ = true;
            update_banner_start_ = std::chrono::steady_clock::now();
            // Force texture re-render in draw_update_banner() so the
            // version string in the cached texture matches the new value.
            update_line1_cached_.clear();
            std::cout << "[Update] New version available: " << result.latest_version
                      << " (current " << current << ") " << result.release_url << "\n";
        } else if (show_when_up_to_date) {
            show_toast("You're up to date (v" + current + ")", 3000);
        }
        update_check_in_progress_.store(false);
    }).detach();
}

std::string Renderer::make_recording_path() const {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &local_tm);
    const char* ext = (settings_.record_format == 1) ? ".gif" : ".mp4";
    return screenshot_dir_ + "/recording_" + ts + ext;
}

void Renderer::start_recording() {
    if (recorder_.is_recording()) return;
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        toast_text_ = "No video to record yet";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }
    std::filesystem::create_directories(screenshot_dir_);

    media::RecordConfig cfg;
    cfg.format           = (settings_.record_format == 1) ? media::RecordFormat::GIF
                                                          : media::RecordFormat::MP4;
    cfg.output_path      = make_recording_path();

    // Match the on-screen composite when the phone bezel is on, so the
    // recording shows the same rounded-corner phone frame the user sees.
    int src_w = last_frame_w_;
    int src_h = last_frame_h_;
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        src_w = phone_frame_.frame_width();
        src_h = phone_frame_.frame_height();
    }
    cfg.width            = src_w & ~1;
    cfg.height           = src_h & ~1;
    // Downscale GIFs aggressively — RGB8 already loses colour; smaller
    // dimensions keep the file under a few MB for 10-15 s clips.
    if (cfg.format == media::RecordFormat::GIF && cfg.width > 480) {
        int new_w = 480 & ~1;
        int new_h = (src_h * new_w / src_w) & ~1;
        cfg.width  = new_w;
        cfg.height = new_h;
    }
    cfg.target_fps       = (cfg.format == media::RecordFormat::GIF)
                            ? settings_.record_fps_gif
                            : settings_.record_fps_mp4;
    cfg.bitrate_kbps     = settings_.record_bitrate_kbps;
    cfg.max_duration_sec = (pending_record_duration_sec_ > 0)
                            ? pending_record_duration_sec_
                            : settings_.record_max_duration_sec;
    pending_record_duration_sec_ = 0;

    if (!recorder_.start(cfg)) {
        toast_text_ = std::string("Recording failed: ") + recorder_.last_error();
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }
    std::cout << "[Recorder] Started: " << cfg.output_path
              << " (" << cfg.width << "x" << cfg.height
              << " @ " << cfg.target_fps << " fps)\n";
    toast_text_ = (cfg.format == media::RecordFormat::GIF)
        ? "Recording GIF..."
        : "Recording MP4...";
    toast_active_ = true;
    toast_start_  = std::chrono::steady_clock::now();
}

void Renderer::stop_recording() {
    if (!recorder_.is_recording() && record_countdown_ms_ == 0) return;
    if (record_countdown_ms_ > 0 && !recorder_.is_recording()) {
        // Cancel countdown
        record_countdown_ms_ = 0;
        toast_text_ = "Recording cancelled";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }
    std::string out = recorder_.stop();
    std::string err = recorder_.last_error();
    if (!err.empty()) {
        toast_text_ = std::string("Recording failed: ") + err;
    } else if (!out.empty()) {
        std::filesystem::path p(out);
        toast_text_ = std::string("Saved ") + p.filename().string();
        std::cout << "[Recorder] Saved: " << out << "\n";
    } else {
        toast_text_ = "Recording stopped";
    }
    toast_active_ = true;
    toast_start_  = std::chrono::steady_clock::now();
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

void Renderer::reset_window_to_default_size() {
    int fw = phone_frame_.frame_width();
    int fh = phone_frame_.frame_height();
    if (fw <= 0 || fh <= 0) return;
    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    float s = std::min(dm.w * 0.32f / fw, dm.h * 0.65f / fh);
    SDL_SetWindowSize(window_, (int)(fw * s), (int)(fh * s));
    window_shape_set_ = false;
    toast_text_ = "Window reset to default size";
    toast_start_ = std::chrono::steady_clock::now();
    toast_active_ = true;
    std::cout << "[Renderer] Reset window to default size\n";
}

#ifdef _WIN32
// =====================================================================
// OCR copy (Ctrl+Shift+T)
// =====================================================================
//
// Reuses the annotator's "freeze a composite, dim it, draw on top" idiom
// but stripped to a single tool: rectangle pick. On mouse-up the cropped
// RGBA region is shipped to a worker thread (see media/ocr.cpp), which
// runs Windows.Media.Ocr and writes the joined text back through a
// mutex-guarded result slot. The render loop polls
// `process_ocr_result()` each frame to publish to the clipboard.

void Renderer::begin_ocr() {
    if (ocr_active_ || annotator_active_) return;
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        toast_text_ = "OCR: no frame yet";
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    // Capture the same composite the annotator does so the user picks a
    // region of exactly what they see (phone frame included when on).
    int cap_w = 0, cap_h = 0;
    std::vector<uint8_t> rgba;
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        uint8_t* composite = phone_frame_.composite_screenshot(
            sdl_renderer_,
            last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
            &cap_w, &cap_h);
        if (!composite) {
            toast_text_ = "OCR: composite failed";
            toast_start_ = std::chrono::steady_clock::now();
            return;
        }
        rgba.assign(composite, composite + (size_t)cap_w * cap_h * 4);
        delete[] composite;
    } else {
        cap_w = last_frame_w_;
        cap_h = last_frame_h_;
        rgba.resize((size_t)cap_w * cap_h * 4);
        for (int y = 0; y < cap_h; ++y) {
            memcpy(rgba.data() + (size_t)y * cap_w * 4,
                   last_frame_data_.data() + (size_t)y * last_frame_stride_,
                   (size_t)cap_w * 4);
        }
    }

    if (ocr_bg_tex_) { SDL_DestroyTexture(ocr_bg_tex_); ocr_bg_tex_ = nullptr; }
    ocr_bg_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ABGR8888,
                                    SDL_TEXTUREACCESS_STATIC, cap_w, cap_h);
    if (!ocr_bg_tex_) {
        std::cerr << "[OCR] SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        return;
    }
    SDL_UpdateTexture(ocr_bg_tex_, nullptr, rgba.data(), cap_w * 4);

    ocr_bg_rgba_ = std::move(rgba);
    ocr_bg_w_ = cap_w;
    ocr_bg_h_ = cap_h;
    ocr_drawing_ = false;
    ocr_drag_x0_ = ocr_drag_y0_ = ocr_drag_x1_ = ocr_drag_y1_ = 0;
    ocr_active_ = true;
    toast_text_ = "OCR: drag a region (Esc to cancel)";
    toast_start_ = std::chrono::steady_clock::now();
    toast_active_ = true;
}

void Renderer::end_ocr() {
    if (ocr_bg_tex_) { SDL_DestroyTexture(ocr_bg_tex_); ocr_bg_tex_ = nullptr; }
    ocr_bg_rgba_.clear();
    ocr_bg_w_ = ocr_bg_h_ = 0;
    ocr_drawing_ = false;
    ocr_active_ = false;
}

void Renderer::draw_ocr_overlay() {
    if (!ocr_active_ || !ocr_bg_tex_) return;

    int win_w = 0, win_h = 0;
    SDL_GetRendererOutputSize(sdl_renderer_, &win_w, &win_h);

    // Restrict the overlay to the PHONE area only (exclude the log
    // drawer if it is open) so the dim backdrop, picture, hint and
    // selection rect all stay within the phone window and the user can
    // still see/use the log on the right.
    int log_w = (int)(log_panel_full_w_ * log_panel_anim_);
    int phone_area_w = std::max(64, win_w - log_w);

    // Dim backdrop — only over the phone area.
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 200);
    SDL_Rect full{0, 0, phone_area_w, win_h};
    SDL_RenderFillRect(sdl_renderer_, &full);

    // Fit the captured composite into the phone area with a small margin.
    int margin = 16;
    int avail_w = std::max(64, phone_area_w - 2 * margin);
    int avail_h = std::max(64, win_h - 2 * margin);
    float sx = (float)avail_w / ocr_bg_w_;
    float sy = (float)avail_h / ocr_bg_h_;
    float s = std::min(sx, sy);
    ocr_dst_w_ = (int)(ocr_bg_w_ * s);
    ocr_dst_h_ = (int)(ocr_bg_h_ * s);
    ocr_dst_x_ = (phone_area_w - ocr_dst_w_) / 2;
    ocr_dst_y_ = (win_h - ocr_dst_h_) / 2;

    SDL_Rect img_dst{ocr_dst_x_, ocr_dst_y_, ocr_dst_w_, ocr_dst_h_};
    SDL_RenderCopy(sdl_renderer_, ocr_bg_tex_, nullptr, &img_dst);

    auto img_to_screen = [&](int ix, int iy, int& ox, int& oy) {
        ox = ocr_dst_x_ + (int)(ix * s);
        oy = ocr_dst_y_ + (int)(iy * s);
    };

    // Selection rect.
    if (ocr_drawing_) {
        int x0s, y0s, x1s, y1s;
        img_to_screen(ocr_drag_x0_, ocr_drag_y0_, x0s, y0s);
        img_to_screen(ocr_drag_x1_, ocr_drag_y1_, x1s, y1s);
        int rx = std::min(x0s, x1s), ry = std::min(y0s, y1s);
        int rw = std::abs(x1s - x0s), rh = std::abs(y1s - y0s);

        // Cut the dimmer out of the selection so the user sees the source
        // pixels unobstructed inside the rect.
        SDL_Rect sel{rx, ry, rw, rh};
        SDL_RenderCopy(sdl_renderer_, ocr_bg_tex_, nullptr, &img_dst);
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 0);
        // We can't punch a hole; redraw selected region of the composite
        // on top of the dimmer instead.
        // Compute matching source rect in image-space.
        auto screen_to_img = [&](int xs, int ys, int& ix, int& iy) {
            ix = (int)((xs - ocr_dst_x_) / s);
            iy = (int)((ys - ocr_dst_y_) / s);
        };
        int isx0, isy0, isx1, isy1;
        screen_to_img(rx, ry, isx0, isy0);
        screen_to_img(rx + rw, ry + rh, isx1, isy1);
        isx0 = std::clamp(isx0, 0, ocr_bg_w_);
        isy0 = std::clamp(isy0, 0, ocr_bg_h_);
        isx1 = std::clamp(isx1, 0, ocr_bg_w_);
        isy1 = std::clamp(isy1, 0, ocr_bg_h_);
        SDL_Rect src{isx0, isy0, isx1 - isx0, isy1 - isy0};
        SDL_RenderCopy(sdl_renderer_, ocr_bg_tex_, &src, &sel);

        // Marching-rectangle border (solid 2-px green).
        SDL_SetRenderDrawColor(sdl_renderer_, 80, 220, 120, 255);
        for (int t = 0; t < 2; ++t) {
            SDL_Rect r{rx - t, ry - t, rw + 2 * t, rh + 2 * t};
            SDL_RenderDrawRect(sdl_renderer_, &r);
        }
    }

    // Status hint at the top — phone-shaped windows are too narrow at the
    // bottom for one-line text, so anchor at the top and auto-shrink the
    // font until it fits the window width.
    int fh = std::max(14, win_h / 42);
    std::string hint;
    if (ocr_running_.load()) {
        hint = "OCR: recognizing\u2026";
    } else if (ocr_drawing_) {
        hint = "Release to recognize \u00B7 Esc to cancel";
    } else {
        hint = "Drag a region \u00B7 Esc to cancel";
    }
    int tw = 0, th = 0;
    SDL_Texture* tex = nullptr;
    // Try shrinking the font until it fits with a 24-px side margin.
    for (int attempt = 0; attempt < 6 && fh >= 10; ++attempt) {
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        tex = make_text_texture(sdl_renderer_, hint, fh, 230, 230, 230, &tw, &th);
        if (tex && tw + 24 <= phone_area_w) break;
        fh = std::max(10, (int)(fh * 0.85f));
    }
    if (tex) {
        int bg_y = 12;
        SDL_Rect bg{(phone_area_w - tw) / 2 - 12, bg_y, tw + 24, th + 12};
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(sdl_renderer_, &bg);
        SDL_Rect d{(phone_area_w - tw) / 2, bg_y + 6, tw, th};
        SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
        SDL_DestroyTexture(tex);
    }
}

bool Renderer::handle_ocr_event(const SDL_Event& ev) {
    if (!ocr_active_) return false;

    auto screen_to_img = [&](int xs, int ys, int& ix, int& iy) -> bool {
        if (ocr_dst_w_ <= 0 || ocr_dst_h_ <= 0) return false;
        float s = (float)ocr_dst_w_ / ocr_bg_w_;
        ix = (int)((xs - ocr_dst_x_) / s);
        iy = (int)((ys - ocr_dst_y_) / s);
        ix = std::clamp(ix, 0, ocr_bg_w_ - 1);
        iy = std::clamp(iy, 0, ocr_bg_h_ - 1);
        // Inside the displayed image rect?
        return xs >= ocr_dst_x_ && xs < ocr_dst_x_ + ocr_dst_w_ &&
               ys >= ocr_dst_y_ && ys < ocr_dst_y_ + ocr_dst_h_;
    };

    switch (ev.type) {
    case SDL_KEYDOWN:
        if (ev.key.keysym.sym == SDLK_ESCAPE) {
            // If a job is running we still drop the modal; the worker
            // thread will publish its result silently or be ignored
            // depending on whether the user cares. Simpler: just close.
            end_ocr();
            return true;
        }
        return true; // swallow keys while OCR modal is up

    case SDL_MOUSEBUTTONDOWN:
        if (ev.button.button == SDL_BUTTON_LEFT && !ocr_running_.load()) {
            int ix, iy;
            if (screen_to_img(ev.button.x, ev.button.y, ix, iy)) {
                ocr_drawing_ = true;
                ocr_drag_x0_ = ocr_drag_x1_ = ix;
                ocr_drag_y0_ = ocr_drag_y1_ = iy;
            }
        }
        return true;

    case SDL_MOUSEMOTION:
        if (ocr_drawing_) {
            int ix, iy;
            screen_to_img(ev.motion.x, ev.motion.y, ix, iy);
            ocr_drag_x1_ = ix;
            ocr_drag_y1_ = iy;
        }
        return true;

    case SDL_MOUSEBUTTONUP:
        if (ev.button.button == SDL_BUTTON_LEFT && ocr_drawing_) {
            ocr_drawing_ = false;
            int ix0 = std::min(ocr_drag_x0_, ocr_drag_x1_);
            int iy0 = std::min(ocr_drag_y0_, ocr_drag_y1_);
            int ix1 = std::max(ocr_drag_x0_, ocr_drag_x1_);
            int iy1 = std::max(ocr_drag_y0_, ocr_drag_y1_);
            int iw = ix1 - ix0;
            int ih = iy1 - iy0;
            if (iw < 8 || ih < 8) {
                toast_text_ = "OCR: selection too small";
                toast_start_ = std::chrono::steady_clock::now();
            } else {
                launch_ocr_job(ix0, iy0, iw, ih);
            }
        }
        return true;

    case SDL_WINDOWEVENT:
        return false; // don't swallow window events
    }
    return false;
}

void Renderer::launch_ocr_job(int ix, int iy, int iw, int ih) {
    if (ocr_running_.exchange(true)) return;

    // Crop into a packed RGBA buffer that the worker owns.
    std::vector<uint8_t> crop((size_t)iw * ih * 4);
    for (int y = 0; y < ih; ++y) {
        memcpy(crop.data() + (size_t)y * iw * 4,
               ocr_bg_rgba_.data() + ((size_t)(iy + y) * ocr_bg_w_ + ix) * 4,
               (size_t)iw * 4);
    }

    int w = iw, h = ih;
    std::thread([this, buf = std::move(crop), w, h]() mutable {
        OcrJobResult r = run_ocr_rgba(buf.data(), w, h);
        {
            std::lock_guard<std::mutex> lk(ocr_result_mu_);
            ocr_result_ok_ = r.ok;
            ocr_result_text_ = std::move(r.text);
            ocr_result_error_ = std::move(r.error);
            ocr_result_pending_ = true;
        }
        ocr_running_.store(false);
    }).detach();
}

void Renderer::process_ocr_result() {
    bool pending = false;
    bool ok = false;
    std::string text, err;
    {
        std::lock_guard<std::mutex> lk(ocr_result_mu_);
        if (!ocr_result_pending_) return;
        pending = true;
        ok = ocr_result_ok_;
        text = std::move(ocr_result_text_);
        err = std::move(ocr_result_error_);
        ocr_result_pending_ = false;
    }
    if (!pending) return;

    if (ok) {
        if (text.empty()) {
            toast_text_ = "OCR: no text detected";
        } else {
            SDL_SetClipboardText(text.c_str());
            // Keep the toast short so it renders at the same font size as
            // the screenshot/clipboard confirmations (the pill auto-shrinks
            // when the text gets long). The recognised text is on the
            // clipboard — no need to echo it back here.
            toast_text_ = "Text copied to clipboard \u2014 ready to paste";
        }
    } else {
        toast_text_ = "OCR failed: " + err;
        std::cerr << "[OCR] " << err << "\n";
    }
    toast_start_ = std::chrono::steady_clock::now();
    toast_active_ = true;
    end_ocr();
}
#endif // _WIN32

} // namespace opm::media
