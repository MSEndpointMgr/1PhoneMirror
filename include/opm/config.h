#pragma once

// Build configuration
// Version macros are injected by CMake from project(... VERSION X.Y.Z) so
// CMakeLists.txt is the single source of truth for the app version.
#ifndef OPM_VERSION_MAJOR
#define OPM_VERSION_MAJOR 0
#endif
#ifndef OPM_VERSION_MINOR
#define OPM_VERSION_MINOR 0
#endif
#ifndef OPM_VERSION_PATCH
#define OPM_VERSION_PATCH 0
#endif
#define OPM_APP_NAME "1PhoneMirror by MSEndpointMgr"

// Default network settings
constexpr int AIRPLAY_PORT = 7000;
constexpr int AIRPLAY_MIRROR_PORT = 7100;
constexpr int RTSP_SETUP_PORT = 7010;
constexpr int CAST_PORT = 8009;

// Renderer settings
constexpr int DEFAULT_WINDOW_WIDTH = 1920;
constexpr int DEFAULT_WINDOW_HEIGHT = 1080;
constexpr int MAX_DECODE_QUEUE = 30;

// mDNS service names
constexpr const char* AIRPLAY_SERVICE_TYPE = "_airplay._tcp";
constexpr const char* RAOP_SERVICE_TYPE = "_raop._tcp";
