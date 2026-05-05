#ifdef ENABLE_MIRACAST

#ifdef _WIN32
#include <windows.h>
#endif

#include <openmirror/miracast/miracast_receiver.h>
#include <iostream>
#include <string>
#include <future>
#include <mutex>

// WinRT headers for Miracast
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Miracast.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// D3D11 for frame extraction
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using Microsoft::WRL::ComPtr;
namespace WMM = winrt::Windows::Media::Miracast;
namespace WMP = winrt::Windows::Media::Playback;
namespace WMC = winrt::Windows::Media::Core;
namespace WF  = winrt::Windows::Foundation;

namespace openmirror::miracast {

struct MiracastReceiver::Impl {
    WMM::MiracastReceiver receiver{nullptr};
    WMM::MiracastReceiverSession session{nullptr};
    WMM::MiracastReceiverSettings settings{nullptr};
    WMM::MiracastReceiverConnection active_connection{nullptr};

    winrt::event_token status_token;
    winrt::event_token connection_token;
    winrt::event_token media_source_token;
    winrt::event_token disconnected_token;
    winrt::event_token frame_available_token;

    media::Decoder decoder;

    // Media playback
    WMP::MediaPlayer player{nullptr};

    // D3D11 for frame extraction
    ComPtr<ID3D11Device> d3d_device;
    ComPtr<ID3D11DeviceContext> d3d_context;
    ComPtr<ID3D11Texture2D> render_texture;
    ComPtr<ID3D11Texture2D> staging_texture;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface render_surface{nullptr};
    int frame_width = 0;
    int frame_height = 0;
    std::mutex d3d_mutex;

    bool init_d3d() {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
            D3D11_SDK_VERSION, d3d_device.GetAddressOf(), &actual,
            d3d_context.GetAddressOf());
        if (FAILED(hr)) {
            // Fallback to WARP (software renderer)
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                D3D11_SDK_VERSION, d3d_device.GetAddressOf(), &actual,
                d3d_context.GetAddressOf());
        }
        return SUCCEEDED(hr);
    }

    bool ensure_textures(int w, int h) {
        if (w == frame_width && h == frame_height && render_texture)
            return true;

        frame_width = w;
        frame_height = h;
        render_texture.Reset();
        staging_texture.Reset();
        render_surface = nullptr;

        // Render target texture (GPU-side, receives CopyFrameToVideoSurface)
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d_device->CreateTexture2D(&desc, nullptr, render_texture.GetAddressOf());
        if (FAILED(hr)) return false;

        // Staging texture (CPU-readable copy)
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = d3d_device->CreateTexture2D(&desc, nullptr, staging_texture.GetAddressOf());
        if (FAILED(hr)) return false;

        // Wrap render texture as WinRT IDirect3DSurface
        ComPtr<IDXGISurface> dxgi_surface;
        hr = render_texture.As(&dxgi_surface);
        if (FAILED(hr)) return false;

        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11SurfaceFromDXGISurface(
            dxgi_surface.Get(), inspectable.put());
        if (FAILED(hr)) return false;

        render_surface = inspectable.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>();

        return render_surface != nullptr;
    }

    void cleanup_playback() {
        if (player) {
            try {
                player.VideoFrameAvailable(frame_available_token);
                player.Pause();
                player.Source(nullptr);
                player.Close();
            } catch (...) {}
            player = nullptr;
        }
        render_texture.Reset();
        staging_texture.Reset();
        render_surface = nullptr;
        frame_width = 0;
        frame_height = 0;
        active_connection = nullptr;
    }
};

MiracastReceiver::MiracastReceiver() : impl_(new Impl) {}

MiracastReceiver::~MiracastReceiver() {
    stop();
    delete impl_;
}

bool MiracastReceiver::start(const Config& config) {
    // WinRT Miracast APIs require a Single-Threaded Apartment (STA).
    // The main thread may already have COM initialized differently (e.g. by SDL2),
    // so we run Miracast on a dedicated STA thread.
    std::promise<bool> result_promise;
    auto result_future = result_promise.get_future();

    DWORD sta_thread_id = 0;
    sta_thread_ = std::thread([this, config, &result_promise, &sta_thread_id]() {
        sta_thread_id = GetCurrentThreadId();
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);

            // Create the Miracast receiver
            impl_->receiver = WMM::MiracastReceiver();

            // Configure settings
            impl_->settings = impl_->receiver.GetDefaultSettings();
            impl_->settings.FriendlyName(winrt::to_hstring(config.display_name));
            impl_->settings.AuthorizationMethod(
                config.require_pin
                    ? WMM::MiracastReceiverAuthorizationMethod::PinDisplayIfRequested
                    : WMM::MiracastReceiverAuthorizationMethod::None
            );
            impl_->settings.RequireAuthorizationFromKnownTransmitters(false);

            auto apply_result = impl_->receiver.DisconnectAllAndApplySettings(impl_->settings);
            if (apply_result.Status() != WMM::MiracastReceiverApplySettingsStatus::Success) {
                std::cerr << "[Miracast] Failed to apply settings\n";
                result_promise.set_value(false);
                return;
            }

            // Initialize D3D11 for frame extraction
            if (!impl_->init_d3d()) {
                std::cerr << "[Miracast] Failed to initialize D3D11\n";
                result_promise.set_value(false);
                return;
            }

            // Check current status before proceeding
            {
                auto cur_status = impl_->receiver.GetStatus();
                auto listen = cur_status.ListeningStatus();
                std::cout << "[Miracast] Current status: listening="
                          << static_cast<int>(listen)
                          << " wifi-direct=" << static_cast<int>(cur_status.WiFiStatus())
                          << "\n";
                if (listen == WMM::MiracastReceiverListeningStatus::NotListening) {
                    std::cerr << "[Miracast] Not listening — check that 'Projecting to this PC' is enabled in Windows Settings\n";
                    std::cerr << "[Miracast]   Settings > System > Projecting to this PC > set to 'Available everywhere'\n";
                }
            }

            // Handle status changes
            impl_->status_token = impl_->receiver.StatusChanged(
                [this](const auto& sender, const auto& args) {
                    auto status = sender.GetStatus();
                    std::cout << "[Miracast] Status changed: listening="
                              << static_cast<int>(status.ListeningStatus()) << "\n";
                }
            );

            // Create and start a session
            auto session_result = impl_->receiver.CreateSessionAsync(nullptr).get();
            impl_->session = session_result;
            if (!impl_->session) {
                std::cerr << "[Miracast] Failed to create session\n";
                result_promise.set_value(false);
                return;
            }

            // Handle incoming connections — keep the connection alive
            impl_->connection_token = impl_->session.ConnectionCreated(
                [this](const auto& sender, const WMM::MiracastReceiverConnectionCreatedEventArgs& args) {
                    auto connection = args.Connection();
                    auto transmitter = connection.Transmitter();

                    std::wcout << L"[Miracast] Device connected: "
                               << transmitter.Name().c_str() << L"\n";

                    impl_->active_connection = connection;
                }
            );

            // Handle media source creation (after WFD negotiation completes)
            impl_->media_source_token = impl_->session.MediaSourceCreated(
                [this](const auto& sender, const WMM::MiracastReceiverMediaSourceCreatedEventArgs& args) {
                    auto media_source = args.MediaSource();
                    auto connection = args.Connection();

                    std::wcout << L"[Miracast] Media stream ready from: "
                               << connection.Transmitter().Name().c_str() << L"\n";

                    // Create MediaPlayer in frame-server mode (no rendering, just decode)
                    impl_->player = WMP::MediaPlayer();
                    impl_->player.IsVideoFrameServerEnabled(true);
                    impl_->player.Source(
                        WMP::MediaPlaybackItem(media_source));

                    // Handle each decoded video frame
                    impl_->frame_available_token = impl_->player.VideoFrameAvailable(
                        [this](const WMP::MediaPlayer& mp, const auto&) {
                            try {
                                auto session = mp.PlaybackSession();
                                int w = static_cast<int>(session.NaturalVideoWidth());
                                int h = static_cast<int>(session.NaturalVideoHeight());
                                if (w <= 0 || h <= 0) return;

                                std::lock_guard<std::mutex> lock(impl_->d3d_mutex);

                                if (!impl_->ensure_textures(w, h)) return;

                                // Copy decoded frame to our D3D surface
                                mp.CopyFrameToVideoSurface(impl_->render_surface);

                                // GPU copy: render texture → staging texture
                                impl_->d3d_context->CopyResource(
                                    impl_->staging_texture.Get(),
                                    impl_->render_texture.Get());

                                // Map staging texture to read pixels on CPU
                                D3D11_MAPPED_SUBRESOURCE mapped = {};
                                HRESULT hr = impl_->d3d_context->Map(
                                    impl_->staging_texture.Get(), 0,
                                    D3D11_MAP_READ, 0, &mapped);
                                if (FAILED(hr)) return;

                                // Convert BGRA (D3D) → RGBA (our renderer)
                                media::VideoFrame frame;
                                frame.width = w;
                                frame.height = h;
                                frame.stride = w * 4;
                                frame.data = new uint8_t[frame.stride * h];

                                for (int y = 0; y < h; y++) {
                                    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                                    uint8_t* dst = frame.data + y * frame.stride;
                                    for (int x = 0; x < w; x++) {
                                        dst[0] = src[2]; // R
                                        dst[1] = src[1]; // G
                                        dst[2] = src[0]; // B
                                        dst[3] = 255;    // A
                                        src += 4;
                                        dst += 4;
                                    }
                                }

                                impl_->d3d_context->Unmap(impl_->staging_texture.Get(), 0);

                                if (on_video_) {
                                    on_video_(std::move(frame));
                                }
                            } catch (const std::exception& e) {
                                std::cerr << "[Miracast] Frame error: " << e.what() << "\n";
                            } catch (...) {}
                        }
                    );

                    impl_->player.Play();
                    std::cout << "[Miracast] Streaming started\n";
                }
            );

            // Handle disconnection
            impl_->disconnected_token = impl_->session.Disconnected(
                [this](const auto& sender, const WMM::MiracastReceiverDisconnectedEventArgs& args) {
                    std::cout << "[Miracast] Device disconnected\n";
                    impl_->cleanup_playback();
                    if (on_disconnect_) on_disconnect_();
                }
            );

            // Start listening for connections
            auto start_result = impl_->session.StartAsync().get();
            if (start_result.Status() != WMM::MiracastReceiverSessionStartStatus::Success) {
                std::cerr << "[Miracast] Failed to start session (status="
                          << static_cast<int>(start_result.Status()) << ")\n";
                std::cerr << "[Miracast] Ensure 'Projecting to this PC' is set to 'Available everywhere' in Settings\n";
                result_promise.set_value(false);
                return;
            }

            running_.store(true);
            std::cout << "[Miracast] Receiver started as '"
                      << config.display_name << "'\n";
            std::cout << "[Miracast] Android devices can now cast to this PC\n";

            result_promise.set_value(true);

            // Run the STA message loop to keep WinRT event handlers alive
            MSG msg;
            while (running_.load() && GetMessage(&msg, nullptr, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            impl_->cleanup_playback();
            winrt::uninit_apartment();

        } catch (const winrt::hresult_error& ex) {
            std::wcerr << L"[Miracast] WinRT error: " << ex.message().c_str()
                       << L" (0x" << std::hex << static_cast<uint32_t>(ex.code()) << L")\n";
            try { result_promise.set_value(false); } catch (...) {}
        } catch (...) {
            std::cerr << "[Miracast] Unknown error during initialization\n";
            try { result_promise.set_value(false); } catch (...) {}
        }
    });

    bool ok = result_future.get();
    sta_thread_id_ = sta_thread_id;
    return ok;
}

void MiracastReceiver::stop() {
    running_.store(false);

    // Post WM_QUIT to break the STA message loop
    if (sta_thread_.joinable() && sta_thread_id_ != 0) {
        PostThreadMessageW(sta_thread_id_, WM_QUIT, 0, 0);
    }

    try {
        if (impl_->session) {
            impl_->session.ConnectionCreated(impl_->connection_token);
            impl_->session.MediaSourceCreated(impl_->media_source_token);
            impl_->session.Disconnected(impl_->disconnected_token);
            impl_->session = nullptr;
        }

        if (impl_->receiver) {
            impl_->receiver.StatusChanged(impl_->status_token);
            impl_->receiver = nullptr;
        }
    } catch (...) {
        // Swallow cleanup errors
    }

    if (sta_thread_.joinable()) {
        sta_thread_.join();
    }

    std::cout << "[Miracast] Receiver stopped\n";
}

} // namespace openmirror::miracast

#endif // ENABLE_MIRACAST
