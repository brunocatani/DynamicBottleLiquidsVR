#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
// Fallout4VR.exe 1.2.72 engine contract used by the VR glue. Every value here was read
// from raw disassembly of the VR executable (two agreeing witnesses unless noted) and
// compared against the matching flat 1.10.163 code. Addresses are RVAs. Nothing here
// is derived from a flat address plus a delta. Hooks additionally check live entry bytes.
namespace nuka::vr {
// ---- Hook and call targets ------------------------------------------------------
// BSEffectShaderProperty render-pass builder (flat RVA 0x27CAE10; Effect property vtable
// 0x30A5938 slot 0x2D). ABI: RenderPassArray*(property, geometry, renderMode, accumulator).
inline constexpr std::uintptr_t kEffectPassBuilder=0x0279E950;
// Geometry draw dispatcher (flat BSBatchRenderer::Draw 0x287EDE0). VR adds a second live
// argument, the render context, which callers 0x2891AF0/0x2891BB0/0x2891C20 forward.
inline constexpr std::uintptr_t kDrawDispatcher=0x028909C0;
// ImageSpaceEffectHDR / ImageSpaceEffectHDRCS render (vtable 0x30B8EA8 / 0x30BE648 slot 1,
// RTTI-confirmed). Four arguments in VR; the flat entries take three.
inline constexpr std::uintptr_t kHdrRender=0x02883360;
inline constexpr std::uintptr_t kHdrComputeRender=0x02903700;
// Appends an engine-owned pass to a property's list:
// RenderPass*(list, shader, property, technique, passType, geometry, lightCount, l0..l3).
inline constexpr std::uintptr_t kAppendPass=0x0278E4F0;
// Cached submission. Most VR draws never reach kDrawDispatcher: prepare(owner, passIndex)
// stores a prepared record at pass+0x28 (and owner+0x420[index]); the executor 0x1D97AA0
// later draws each record through draw(renderer, record, command), which issues the D3D
// call (stereo DrawIndexedInstanced at 0x1DA5283). Submit 0x28AA8D0 picks the route.
inline constexpr std::uintptr_t kPreparePass=0x028AAE20;
inline constexpr std::uintptr_t kCachedDraw=0x01DA51E0;
inline constexpr std::size_t kOwnerPasses=0x3D0; // RenderPass** indexed by passIndex
// Returns the active global directional-ambient transform (lea rax,[rip+...]; ret).
// FO4VR moved flat BSShaderManager::State+0xC0 into this double-buffered render state;
// the VR Lighting shader reads it through this accessor (0x291E0A2).
inline constexpr std::uintptr_t kAmbientTransformAccessor=0x027AE6F0;

// ---- Globals --------------------------------------------------------------------
inline constexpr std::uintptr_t kUtilityShaderSingleton=0x0689B4F0; // BSUtilityShader*
// BSEffectShader primary vtable (singleton cell 0x689B438). Tagged geometry also gets
// Utility-shader passes (for example technique 0x100000A), which use a different VS b2.
inline constexpr std::uintptr_t kEffectShaderVtable=0x030BC738;
inline constexpr std::uintptr_t kRenderFrameCounter=0x065A2B48;     // uint32, +1 per frame
inline constexpr std::uintptr_t kD3DDevice=0x060F3CF0;              // ID3D11Device*
inline constexpr std::uintptr_t kD3DImmediateContext=0x060F3D08;    // ID3D11DeviceContext*
// Context the renderer currently records into; the cached draw 0x1DA51E0 issues its D3D
// calls through it. It may be a deferred context, whose vtable differs from the immediate one.
inline constexpr std::uintptr_t kD3DSelectedContext=0x06235AB0;     // ID3D11DeviceContext*
inline constexpr std::uintptr_t kShaderManagerState=0x06879520;     // BSShaderManager::State (shadowSceneNode[0] at +0)
// Selected native render root. It is the context currently rendering, so inside a scene
// draw it describes that draw's two eyes.
inline constexpr std::uintptr_t kSelectedRenderRoot=0x06235AC8;     // pointer
inline constexpr std::size_t kRootEyeOrigins=0x2590;   // two float4 (xyz) current eye origins
inline constexpr std::size_t kRootEyeRecords=0x25D0;   // pointer to per-eye records
inline constexpr std::size_t kEyeRecordStride=0x210;
inline constexpr std::size_t kEyeRecordProjection=0x90; // native 4x4 projection

// ---- Entry signatures (first 16 bytes, from the unpacked executable) ---------------
inline constexpr std::array<std::uint8_t,16> kEffectPassBuilderBytes{0x48,0x89,0x5c,0x24,0x08,0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x55};
inline constexpr std::array<std::uint8_t,16> kDrawDispatcherBytes{0x40,0x55,0x56,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xc1,0x48,0x81,0xec,0x88,0x00};
inline constexpr std::array<std::uint8_t,16> kHdrRenderBytes{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x4c,0x89,0x48,0x20,0x48,0x89,0x50,0x10,0x55};
inline constexpr std::array<std::uint8_t,16> kHdrComputeRenderBytes{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x48,0x89,0x50,0x10,0x55,0x53,0x56,0x48,0x8d};
inline constexpr std::array<std::uint8_t,16> kPreparePassBytes{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48};
inline constexpr std::array<std::uint8_t,16> kCachedDrawBytes{0x48,0x89,0x5c,0x24,0x18,0x56,0x48,0x83,0xec,0x30,0x80,0x79,0x01,0x00,0x48,0x8b};
inline constexpr std::array<std::uint8_t,16> kAppendPassBytes{0x4c,0x8b,0xdc,0x49,0x89,0x5b,0x08,0x49,0x89,0x6b,0x10,0x49,0x89,0x73,0x18,0x57};

// ---- Layouts --------------------------------------------------------------------
// Render pass (0x58 bytes): constructor 0x2890350, light setter 0x28904B0, builder,
// dispatcher, list clear 0x278E3E0.
namespace pass {
inline constexpr std::size_t shader=0x08,property=0x10,geometry=0x18,instanceGroup=0x20,
    preparedRecord=0x28,lights=0x38,next=0x40,technique=0x48,passType=0x4C,lightCount=0x4F;
}
// Shader property: flags 0x30 (descriptor producer 0x279F360), list head 0x38, material 0x58.
namespace property {
inline constexpr std::size_t flags=0x30,passList=0x38,material=0x58;
inline constexpr std::uint64_t envMapFlag=0x80,falloffFlag=0x40;
}
// BSEffectShaderMaterial: flat layout minus 0x10 from +0x38 onward (constructor 0x2894470,
// copy 0x2894780, upload 0x28CECB0); identical to pinned flat CommonLibF4 field order.
namespace effectMaterial {
inline constexpr std::uintptr_t vtable=0x030BA6E8;
inline constexpr std::size_t falloff=0x28,baseColor=0x38,environmentScale=0xA0,lightingInfluence=0xA5;
}
// The native refraction branch (flat 0x27CAFC8, VR 0x279EAF1) appends a BSUtilityShader
// pass with this technique; flat assigns pass type 5 afterwards, VR passes 7 to Append.
inline constexpr std::uint32_t kOpticalTechnique=0x61A;
inline constexpr std::uint8_t kOpticalPassType=7;
// Directional sun shadows. ShadowSceneNode+0x248 (flat +0x208). Entries are 0x110 bytes
// (flat 0xF0); getter 0x29110C0 matches flat 0x28CA050, setup 0x2915210 matches flat 0x28DA760.
namespace shadow {
inline constexpr std::size_t sceneDirectionalLight=0x248,count=0x190,entries=0x198,entryStride=0x110;
inline constexpr std::size_t worldToShadow=0x00,depthStencilTarget=0x60,arraySlice=0x6C,viewport=0xE8;
}

// Lights. BSLight is unchanged from flat (setter 0x286E870, luminance 0x286EA60, Effect
// upload 0x28CF8C0). NiLight data moved +0x40 because the VR NiAVObject base is 0x160
// (constructor 0x1C362A0; NiDirectionalLight constructor 0x1C434B0, update 0x1C43500).
namespace light {
inline constexpr std::size_t bsLightFade=0x10,bsLightLight=0xB8,bsLightShape=0x180;
inline constexpr std::size_t niLightDiffuse=0x16C,niLightRadius=0x178,niLightDimmer=0x184,directionalDirection=0x1B0;
}
// Sky (accessor 0x12FB50 publishes the cell; constructor 0x6393A0). Sky::mode moved to
// +0x35C (Sun update 0x647080 compares 3 for the full solar branch).
namespace sky {
inline constexpr std::uintptr_t singletonCell=0x05A3D000;
inline constexpr std::size_t sun=0x80,mode=0x35C,sunLight=0x38;
inline constexpr std::uint32_t modeFull=3;
}
// The current image-space data of a native draw: *(void**)renderContext, sunlightScale at
// +0x18 (Effect SetupGeometry 0x28D0099; DF lighting 0x291EA4E). VR's ImageSpaceManager has
// no embedded EOF data, so the dispatcher's render context is the only valid source.
inline constexpr std::size_t kContextImageData=0x00,kImageSunlightScale=0x18;
// Player. VR controller grabs store one reference handle per controller (initiation
// 0x0F19250, update 0x0F1A0E0, release 0x0F1AB90, GetPlayerGrabbedRef 0x04F0670); the flat
// grabbedObject field is unused by native VR grabs.
namespace player {
inline constexpr std::size_t grabHandles[2]{0x9E0,0xA30};
inline constexpr std::size_t firstPerson3D=0xFE8;
inline constexpr std::uintptr_t cameraCell=0x05930608; // PlayerCamera* (constructor 0x12CA800)
inline constexpr std::size_t cameraRoot=0x20,cameraState=0x28; // NiNode* (setter 0x12CAC90), TESCameraState* (update 0x81EED0)
}

// Sun shadow depth. The DFLight sampler (0x291DCA0) binds entry+0x60 (directional
// logical ID 14) as a Texture2DArray; +0x6C is the slice (constructor 0x290CC30, submit
// 0x2931940). entry+0x00 is absolute world -> shadow (producer 0x286F350). Logical depth
// IDs map through the RenderTargetManager object to RendererData depth targets
// (binders 0x1DB9E40 / 0x1DBA470 / 0x1DBA890 / 0x1DBAB10).
namespace depth {
inline constexpr std::uintptr_t renderTargetManager=0x038AC010; // object, not a pointer cell
inline constexpr std::size_t logicalToSlot=0x15FC;               // int32[17]
inline constexpr std::uint32_t logicalCount=17;
inline constexpr std::uintptr_t rendererDataCell=0x060F3CE8;    // pointer
inline constexpr std::size_t targets=0x2588,targetStride=0x98,depthView=0x88;
inline constexpr std::int32_t slotCount=18;
}

// ---- Safe access ----------------------------------------------------------------
std::uintptr_t ModuleBase();
// Plausible user-mode object pointer: non-null, aligned and inside the address space.
inline bool Plausible(const void* p,std::size_t alignment=8){
    auto v=reinterpret_cast<std::uintptr_t>(p);
    return v>=0x10000&&v<0x7FFFFFFF0000ull&&v%alignment==0;
}
template<class T> T Read(const void* base,std::size_t offset){T value;std::memcpy(&value,static_cast<const std::byte*>(base)+offset,sizeof(T));return value;}
template<class T> T Global(std::uintptr_t rva){return Read<T>(reinterpret_cast<const void*>(ModuleBase()+rva),0);}
bool EntryMatches(std::uintptr_t rva,std::span<const std::uint8_t> bytes);
}
