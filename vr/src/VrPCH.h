#pragma once
// Fallout 4 VR precompiled header, selected by native/src/PCH.h when DBL_FALLOUT_VR is
// defined, so every shared source compiles against CommonLibF4VR. DBL_ENGINE_FREE builds
// (ProfileTests) omit the engine: linking CommonLibF4VR runs module initialization that
// requires the game process.
#ifndef DBL_ENGINE_FREE
// CommonLibF4VR must precede every Windows API header.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <REL/Relocation.h>
#include <spdlog/spdlog.h>
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef ERROR
#undef ERROR
#endif
#ifndef DBL_ENGINE_FREE
// The shared sources log through commonlib-shared's REX::INFO/WARN/ERROR, which
// CommonLibF4VR does not provide. These forward to the plugin's spdlog logger.
namespace REX {
template<class... Args> void INFO(spdlog::format_string_t<Args...> format,Args&&... args){spdlog::info(format,std::forward<Args>(args)...);}
template<class... Args> void WARN(spdlog::format_string_t<Args...> format,Args&&... args){spdlog::warn(format,std::forward<Args>(args)...);}
template<class... Args> void ERROR(spdlog::format_string_t<Args...> format,Args&&... args){spdlog::error(format,std::forward<Args>(args)...);}
}
#endif
