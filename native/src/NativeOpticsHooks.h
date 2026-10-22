#pragma once
#include <RE/B/BSRenderPass.h>
namespace nuka {
// Flat Fallout 4 engine hooks that feed NativeOptics: the Effect pass builder
// registers the delayed optical pass, and the HDR entries trigger composition.
void InstallOpticsHooks();
bool IsOwnedOpticalPass(const RE::BSRenderPass*);
}
