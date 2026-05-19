// Windows.Media.Ocr (WinRT) wrapper. See header for contract.
//
// Implementation notes:
//   * Uses C++/WinRT headers shipped with the Windows SDK; no extra deps.
//   * Apartment is initialised lazily (per worker thread). The renderer
//     spawns a fresh std::thread per OCR job, so each call hits the
//     "not initialised yet" path once and then runs to completion.
//   * SoftwareBitmap requires BGRA8; the input is RGBA so we swap +
//     premultiply in the lock-buffer copy.
//   * IMemoryBufferByteAccess is declared inline to avoid pulling the
//     ABI-style <Windows.Foundation.h> header (which conflicts in some
//     translation units).

#include "opm/media/ocr.h"

#include <Windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

// IMemoryBufferByteAccess COM interface (lives in <windows.foundation.h>
// ABI header which we deliberately don't include). UUID from the WinRT
// metadata; signature taken from the Windows 10 SDK headers.
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d"))
__declspec(novtable)
IMemoryBufferByteAccess : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

namespace {

std::string hstring_to_utf8(winrt::hstring const& h) {
    int wlen = static_cast<int>(h.size());
    if (wlen <= 0) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, h.c_str(), wlen,
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, h.c_str(), wlen,
                          out.data(), n, nullptr, nullptr);
    return out;
}

void ensure_apartment() {
    // Try MTA — the OCR worker is a fresh thread, no UI marshalling needed.
    try {
        winrt::init_apartment();
    } catch (winrt::hresult_error const& e) {
        // Already initialised on this thread with a different mode — fine,
        // any apartment can host the OCR engine.
        if (e.code() != RPC_E_CHANGED_MODE) throw;
    }
}

} // namespace

namespace opm::media {

OcrJobResult run_ocr_rgba(const uint8_t* rgba, int w, int h) {
    OcrJobResult out;
    if (!rgba || w <= 0 || h <= 0) {
        out.error = "invalid input region";
        return out;
    }

    using namespace winrt::Windows::Globalization;
    using namespace winrt::Windows::Graphics::Imaging;
    namespace WMO = winrt::Windows::Media::Ocr;

    try {
        ensure_apartment();

        // Prefer the user's installed languages; fall back to en-US when
        // no profile language has an OCR pack on this machine.
        WMO::OcrEngine engine{ nullptr };
        try { engine = WMO::OcrEngine::TryCreateFromUserProfileLanguages(); }
        catch (...) {}
        if (!engine) {
            try { engine = WMO::OcrEngine::TryCreateFromLanguage(Language(L"en-US")); }
            catch (...) {}
        }
        if (!engine) {
            out.error = "No OCR language pack installed (Settings > Time & Language > Language)";
            return out;
        }

        // Build a Bgra8 SoftwareBitmap from the packed RGBA input. The
        // OCR engine accepts Bgra8 + Premultiplied alpha; we treat the
        // input as opaque so premultiply ≡ identity for typical screen
        // captures.
        SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, w, h,
                              BitmapAlphaMode::Premultiplied);
        {
            BitmapBuffer buf = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
            auto ref = buf.CreateReference();
            uint8_t* dst = nullptr;
            uint32_t cap = 0;
            auto byte_access = ref.as<IMemoryBufferByteAccess>();
            winrt::check_hresult(byte_access->GetBuffer(&dst, &cap));
            const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
            if (cap < need) {
                out.error = "WinRT bitmap buffer smaller than expected";
                return out;
            }
            const int npx = w * h;
            for (int i = 0; i < npx; ++i) {
                const uint8_t r = rgba[i * 4 + 0];
                const uint8_t g = rgba[i * 4 + 1];
                const uint8_t b = rgba[i * 4 + 2];
                const uint8_t a = rgba[i * 4 + 3];
                // Swap RGBA → BGRA, premultiply (a/255 * channel).
                dst[i * 4 + 0] = static_cast<uint8_t>((b * a + 127) / 255);
                dst[i * 4 + 1] = static_cast<uint8_t>((g * a + 127) / 255);
                dst[i * 4 + 2] = static_cast<uint8_t>((r * a + 127) / 255);
                dst[i * 4 + 3] = a;
            }
        }

        // Block on the async result — we're already on a worker thread.
        WMO::OcrResult res = engine.RecognizeAsync(bitmap).get();

        std::string joined;
        for (auto const& line : res.Lines()) {
            if (!joined.empty()) joined.push_back('\n');
            joined += hstring_to_utf8(line.Text());
        }
        out.ok = true;
        out.text = std::move(joined);
        return out;

    } catch (winrt::hresult_error const& e) {
        out.error = "WinRT OCR error: " + hstring_to_utf8(e.message());
        return out;
    } catch (std::exception const& e) {
        out.error = e.what();
        return out;
    } catch (...) {
        out.error = "unknown OCR error";
        return out;
    }
}

} // namespace opm::media
