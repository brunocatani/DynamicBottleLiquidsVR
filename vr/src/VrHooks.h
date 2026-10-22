#pragma once
#include "DrawCall.h"
#include <d3d11.h>
namespace nuka::vr {
// The engine call that draws one pass, through either the legacy dispatcher or the cached
// record executor. Invoke() runs the original engine function with the original arguments.
struct PassDraw {
    void(*invoke)(const PassDraw&);void* a;void* b;void* c;
    void Invoke() const{invoke(*this);}
};
// Implemented by VrPlugin.cpp. Called on the render thread. renderContext is null on the
// cached route, which does not receive it.
void OnPassDraw(void* pass,void* renderContext,const PassDraw& draw);
void OnIndexedDraw(ID3D11DeviceContext*,const DrawCall&);
// Installs the engine detours after checking every target's live entry bytes. Installs
// nothing unless all targets match. Process lifetime: F4SE never unloads plugins.
bool InstallEngineHooks();
// Replaces DrawIndexed (12) and DrawIndexedInstanced (20) in the context's vtable and
// chains to the previous values. Immediate and deferred contexts have different vtables,
// and overlay layers may swap or rebuild a context's vtable, so callers re-check before each
// bottle pass; this is cheap when the slots are already ours. The d3d11 function bodies stay
// untouched, so detours owned by other mods there (CSAH validates its own) survive.
bool HookContextDraws(ID3D11DeviceContext*);
bool IsOwnedOpticalPass(const void* pass);
}
