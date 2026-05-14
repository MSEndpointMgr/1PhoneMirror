cmake_minimum_required(VERSION 3.20)
project(1PhoneMirror VERSION 0.3.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(ENABLE_AIRPLAY "Build AirPlay receiver module" ON)
option(ENABLE_MIRACAST "Build Miracast receiver module (Windows only)" ON)
option(ENABLE_CAST "Build Google Cast receiver module" ON)
option(ENABLE_ANDROID "Build Android (scrcpy-based) screen mirror receiver" ON)

# --- Dependencies via vcpkg or find_package ---
find_package(PkgConfig QUIET)

# FFmpeg
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)
find_library(SWRESAMPLE_LIBRARY swresample)
find_library(AVFILTER_LIBRARY avfilter)

# SDL2
find_package(SDL2 CONFIG REQUIRED)

# OpenSSL (for AirPlay crypto)
find_package(OpenSSL REQUIRED)

# DNS-SD / Bonjour (for AirPlay mDNS)
# mDNS: Built-in multicast responder — no Bonjour SDK needed.
# On Windows, uses winsock2 multicast (ws2_32 + iphlpapi).
# On Linux, you can optionally link avahi-compat-libdns_sd.

# --- Source files ---
set(MEDIA_SOURCES
    src/media/decoder.cpp
    src/media/renderer.cpp
    src/media/audio_output.cpp
    src/media/phone_frame.cpp
    src/media/recorder.cpp
)

if(WIN32)
    list(APPEND MEDIA_SOURCES src/media/ocr.cpp)
endif()

set(NETWORK_SOURCES
    src/network/tcp_server.cpp
    src/network/rtsp_server.cpp
    src/network/update_check.cpp
)

set(CORE_SOURCES
    src/main.cpp
    src/app.cpp
    src/settings.cpp
    ${MEDIA_SOURCES}
    ${NETWORK_SOURCES}
    lib/stb_image_write_impl.c
    lib/stb_image_impl.c
)

if(ENABLE_AIRPLAY)
    list(APPEND CORE_SOURCES
        src/airplay/airplay_server.cpp
        src/airplay/mdns_service.cpp
        src/airplay/airplay_mirror.cpp
        src/airplay/pairing.cpp
        src/airplay/srp_pin.cpp
        src/airplay/bplist.cpp
        src/airplay/mirror_buffer.cpp
        lib/playfair/playfair.c
        lib/playfair/hand_garble.c
        lib/playfair/modified_md5.c
        lib/playfair/omg_hax.c
        lib/playfair/sap_hash.c
    )
endif()

if(ENABLE_MIRACAST AND WIN32)
    list(APPEND CORE_SOURCES
        src/miracast/miracast_receiver.cpp
    )
endif()

if(ENABLE_CAST)
    list(APPEND CORE_SOURCES
        src/cast/cast_receiver.cpp
    )
endif()

if(ENABLE_ANDROID)
    list(APPEND CORE_SOURCES
        src/android/scrcpy_receiver.cpp
        src/android/android_dialog.cpp
    )
endif()

# --- Target ---
add_executable(1PhoneMirror ${CORE_SOURCES})

target_include_directories(1PhoneMirror PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib
    ${AVCODEC_INCLUDE_DIR}
)

# Derive C++ version macros from the single source of truth (project()).
# Keeps OPENMIRROR_VERSION_* in sync with package version automatically.
target_compile_definitions(1PhoneMirror PRIVATE
    OPENMIRROR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    OPENMIRROR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    OPENMIRROR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_link_libraries(1PhoneMirror PRIVATE
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVFILTER_LIBRARY}
    SDL2::SDL2
    SDL2::SDL2main
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(ENABLE_AIRPLAY)
    target_compile_definitions(1PhoneMirror PRIVATE ENABLE_AIRPLAY=1)
    if(WIN32)
        target_link_libraries(1PhoneMirror PRIVATE iphlpapi)
    endif()
endif()

if(ENABLE_MIRACAST AND WIN32)
    target_compile_definitions(1PhoneMirror PRIVATE ENABLE_MIRACAST=1)
    # Only needs WinRT for WiFi Direct (no D3D11/DXGI — uses FFmpeg for decode)
    target_link_libraries(1PhoneMirror PRIVATE
        windowsapp.lib
        RuntimeObject.lib
    )
endif()

if(ENABLE_CAST)
    target_compile_definitions(1PhoneMirror PRIVATE ENABLE_CAST=1)
    target_include_directories(1PhoneMirror PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

if(ENABLE_ANDROID)
    target_compile_definitions(1PhoneMirror PRIVATE ENABLE_ANDROID=1)
endif()

if(WIN32)
    target_link_libraries(1PhoneMirror PRIVATE ws2_32 winhttp)
    # WindowsApp.lib provides the C++/WinRT runtime exports used by the
    # OCR copy feature (Windows.Media.Ocr). Linked unconditionally on
    # Windows so it's available even when ENABLE_MIRACAST is OFF.
    target_link_libraries(1PhoneMirror PRIVATE windowsapp.lib)
    target_compile_definitions(1PhoneMirror PRIVATE NOMINMAX)
    # Embed app manifest for DPI awareness (disable auto-generated manifest to avoid duplicate)
    target_sources(1PhoneMirror PRIVATE resources/openmirror.rc)
    set_target_properties(1PhoneMirror PROPERTIES LINK_FLAGS "/MANIFEST:NO /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

# --- Install ---
install(TARGETS 1PhoneMirror RUNTIME DESTINATION bin)

# Copy logo and icon PNGs to build output directory
add_custom_command(TARGET 1PhoneMirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/MSEndpointMGRLogoBig.PNG"
        "$<TARGET_FILE_DIR:1PhoneMirror>/MSEndpointMGRLogoBig.PNG"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/apple_icon.png"
        "$<TARGET_FILE_DIR:1PhoneMirror>/apple_icon.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/android_icon.png"
        "$<TARGET_FILE_DIR:1PhoneMirror>/android_icon.png"
)
