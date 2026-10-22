#pragma once
// The Fallout 4 VR build (CMakeLists.txt) defines DBL_FALLOUT_VR and supplies its own
// CommonLibF4VR header; the flat build (xmake.lua) uses the declarations below.
#ifdef DBL_FALLOUT_VR
#include "VrPCH.h"
#else
#include <F4SE/F4SE.h>
#include <RE/B/BSEffectShader.h>
#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSTriShape.h>
#include <RE/N/NiNode.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>
#ifdef ERROR
#undef ERROR
#endif
#endif
