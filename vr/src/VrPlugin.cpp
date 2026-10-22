#include "PCH.h"
#include "VrEngine.h"
#include "VrHooks.h"
#include "Profiles.h"
#include "Renderer.h"
#include "NativeOptics.h"
#include "BottleFrame.h"
#include <MinHook.h>
#include <ShlObj.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace nuka::vr {
namespace {
const std::filesystem::path root=L"Data/F4SE/Plugins/DynamicBottleLiquids";
DrinkMap drinks;
Renderer renderer;
std::mutex renderMutex;
ID3D11DeviceContext* hookedContext{};
const void* registeredGeometry=nullptr;
thread_local void* active{};
thread_local void* activeContext{};
thread_local const DrinkProfile* activeDrink=nullptr;
constexpr std::uint64_t kBottleVertexDesc=0x1b00000430205ull;
constexpr std::uintptr_t kTriShapeVtable=0x02E5A0B8;
constexpr std::string_view kTag="DynamicBottleLiquids:";

enum class Rejection:unsigned {Geometry,Pass,Profile,VertexLayout,Parent,Technique};
constexpr const char* kRejectionNames[]{"not a BSTriShape","instanced or property-less pass","no profile for vertex/index counts",
    "unexpected vertex descriptor","geometry has no parent","unsupported Effect technique"};
unsigned reportedRejections=0;

double Now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
RE::BSGeometry* PassGeometry(const void* pass){return Read<RE::BSGeometry*>(pass,pass::geometry);}
bool Tagged(RE::BSGeometry* g){return Plausible(g)&&g->name.c_str()&&std::string_view(g->name.c_str()).starts_with(kTag);}
bool Under(const RE::NiAVObject* object,const RE::NiAVObject* rootNode){
    if(!rootNode)return false;
    for(unsigned depth=0;Plausible(object)&&depth<128;object=object->parent,++depth)if(object==rootNode)return true;
    return false;
}
const DrinkProfile* Profile(RE::BSGeometry* g){
    auto& tri=static_cast<RE::BSTriShape*>(g)->GetTriShapeRuntimeData();
    return FindProfile(drinks,g->name.c_str(),tri.numVertices,tri.numTriangles*3);
}
void Reject(Rejection reason,RE::BSGeometry* g,const void* p){
    const auto bit=1u<<unsigned(reason);if(reportedRejections&bit)return;reportedRejections|=bit;
    const bool tri=reason!=Rejection::Geometry;
    const auto& data=static_cast<RE::BSTriShape*>(g)->GetTriShapeRuntimeData();
    REX::WARN("Bottle pass skipped ({}): {} technique={:#x} vertexDesc={:#x} vertices={} indices={}",kRejectionNames[unsigned(reason)],g->name.c_str(),
        Read<std::uint32_t>(p,pass::technique),g->GetRuntimeData().vertexDesc.desc,tri?unsigned(data.numVertices):0u,tri?data.numTriangles*3:0u);
}
bool Match(void* p,RE::BSGeometry* g){
    if(Read<std::uintptr_t>(g,0)!=ModuleBase()+kTriShapeVtable){Reject(Rejection::Geometry,g,p);return false;}
    if(!Read<void*>(p,pass::property)||Read<void*>(p,pass::instanceGroup)){Reject(Rejection::Pass,g,p);return false;}
    if(!Profile(g)){Reject(Rejection::Profile,g,p);return false;}
    if(g->GetRuntimeData().vertexDesc.desc!=kBottleVertexDesc){Reject(Rejection::VertexLayout,g,p);return false;}
    if(!g->parent){Reject(Rejection::Parent,g,p);return false;}
    // These Effect variants have ModelView directly after WorldViewProj in VS b2 (per eye).
    const auto d=Read<std::uint32_t>(p,pass::technique);
    constexpr unsigned required=(1u<<4)|(1u<<19)|(1u<<21);
    constexpr unsigned excluded=(1u<<1)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<18)|(1u<<29);
    if(!(d&required)||(d&excluded)){Reject(Rejection::Technique,g,p);return false;}
    return true;
}
void EnsureRenderer(){
    if(hookedContext)return;
    static bool attempted=false;if(attempted)return;
    auto* device=Global<ID3D11Device*>(kD3DDevice);auto* ctx=Global<ID3D11DeviceContext*>(kD3DImmediateContext);
    if(!Plausible(device)||!Plausible(ctx))return;
    attempted=true;
    if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::runtime_error("Expected immediate context");
    renderer.Initialize(device,root/"shaders",2952);
    // VR Effect VS b2 holds one WorldViewProj and one ModelView per eye.
    renderer.SetGeometryConstantBytes(224);
    if(!HookContextDraws(ctx))throw std::runtime_error("Cannot hook DrawIndexed/DrawIndexedInstanced");
    hookedContext=ctx;
    REX::INFO("D3D11 renderer initialized; glass replacement is ready.");
}
// Both eyes of the draw being rendered: their mean origin drives distance LOD, and the
// first eye's projection drives the per-eye projected size.
bool StereoView(ID3D11DeviceContext* ctx,Vec& eye,ProjectionInputs& projection){
    auto* rootNode=Global<const void*>(kSelectedRenderRoot);if(!Plausible(rootNode,16))return false;
    float origins[8];std::memcpy(origins,static_cast<const std::byte*>(rootNode)+kRootEyeOrigins,sizeof(origins));
    eye={(origins[0]+origins[4])*.5f,(origins[1]+origins[5])*.5f,(origins[2]+origins[6])*.5f};
    if(!std::isfinite(eye.x+eye.y+eye.z))return false;
    auto* records=Read<const void*>(rootNode,kRootEyeRecords);
    if(Plausible(records,16)){
        float p00=std::abs(Read<float>(records,kEyeRecordProjection)),p11=std::abs(Read<float>(records,kEyeRecordProjection+0x14));
        D3D11_VIEWPORT viewport{};UINT n=1;ctx->RSGetViewports(&n,&viewport);
        if(n==1&&std::isfinite(p00+p11)&&p00>1e-4f&&p11>1e-4f&&viewport.Width>0)
            projection={true,2/p11,2/p00,viewport.Width*.5f,viewport.Height};
    }
    return true;
}
void Ambient(AmbientInputs& out){
    static const bool accessorMatches=[]{
        const auto* code=reinterpret_cast<const std::uint8_t*>(ModuleBase()+kAmbientTransformAccessor);
        return code[0]==0x48&&code[1]==0x8D&&code[2]==0x05&&code[7]==0xC3;
    }();
    if(!accessorMatches)return;
    auto* transform=reinterpret_cast<const RE::NiTransform*(*)()>(ModuleBase()+kAmbientTransformAccessor)();
    if(!Plausible(transform,16))return;
    out.valid=true;out.translate={transform->translate.x,transform->translate.y,transform->translate.z};out.scale=transform->scale;
    for(int axis=0;axis<3;axis++)out.axes[axis]={transform->rotate[axis].x,transform->rotate[axis].y,transform->rotate[axis].z};
}
Vec ReadVec(const void* base,std::size_t offset){return {Read<float>(base,offset),Read<float>(base,offset+4),Read<float>(base,offset+8)};}
// Returns the sun's NiLight so the local-light list can skip it, as the flat build does.
const void* Sun(const void* renderContext,SunInputs& out){
    auto* skyObject=Global<const void*>(sky::singletonCell);
    if(!Plausible(skyObject)||Read<std::uint32_t>(skyObject,sky::mode)!=sky::modeFull)return nullptr;
    auto* sun=Read<const void*>(skyObject,sky::sun);if(!Plausible(sun))return nullptr;
    auto* light=Read<const void*>(sun,sky::sunLight);if(!Plausible(light))return nullptr;
    out={true,ReadVec(light,light::directionalDirection),ReadVec(light,light::niLightDiffuse),Read<float>(light,light::niLightDimmer),1};
    auto* imageData=Plausible(renderContext)?Read<const void*>(renderContext,kContextImageData):nullptr;
    if(Plausible(imageData))out.sunlightScale=Read<float>(imageData,kImageSunlightScale);
    return light;
}
unsigned Lights(const void* p,RE::BSGeometry* g,const void* sun,std::array<LocalLightInput,64>& out){
    auto* list=Read<void* const*>(p,pass::lights);const unsigned count=Read<std::uint8_t>(p,pass::lightCount);
    if(!Plausible(list))return 0;
    unsigned n=0;
    // This is the engine-provided list length; null and shapeless entries are skipped.
    for(unsigned i=0;i<count&&n<out.size();i++){
        auto* entry=list[i];if(!Plausible(entry))continue;
        auto* l=Read<const RE::NiAVObject*>(entry,light::bsLightLight);
        if(!Plausible(l)||l==sun||Read<std::uint32_t>(entry,light::bsLightShape)==0)continue;
        const auto& c=g->worldBound.center;const auto& t=l->world.translate;
        out[n++]={{c.x-t.x,c.y-t.y,c.z-t.z},Read<float>(l,light::niLightRadius),Read<float>(l,light::niLightDimmer),
            Read<float>(entry,light::bsLightFade),ReadVec(l,light::niLightDiffuse)};
    }
    return n;
}
// Player state shared by every bottle in a frame. References stay owned only until the
// next frame replaces them.
struct PlayerFrame {
    std::uint32_t frame=~0u;
    RE::NiPointer<RE::TESObjectREFR> grabbed[2];
    const RE::NiAVObject* grabbedRoots[2]{};
    const RE::NiAVObject* firstPersonRoot{};
    const RE::NiMatrix3* viewRotation{};
    const void* cameraState{};
} playerFrame;
void UpdatePlayerFrame(std::uint32_t frame){
    if(playerFrame.frame==frame)return;
    playerFrame=PlayerFrame{};playerFrame.frame=frame;
    if(auto* player=RE::PlayerCharacter::GetSingleton();Plausible(player)){
        for(int hand=0;hand<2;hand++){
            RE::ObjectRefHandle handle;const auto raw=Read<std::uint32_t>(player,player::grabHandles[hand]);
            static_assert(sizeof(handle)==sizeof(raw));std::memcpy(&handle,&raw,sizeof(raw));
            if(raw&&(playerFrame.grabbed[hand]=handle.get()))playerFrame.grabbedRoots[hand]=playerFrame.grabbed[hand]->Get3D();
        }
        auto* first=Read<const RE::NiAVObject*>(player,player::firstPerson3D);
        playerFrame.firstPersonRoot=Plausible(first)?first:nullptr;
    }
    auto* camera=Global<const void*>(player::cameraCell);
    if(Plausible(camera)){
        auto* viewRoot=Read<const RE::NiAVObject*>(camera,player::cameraRoot);
        if(Plausible(viewRoot))playerFrame.viewRotation=&viewRoot->world.rotate;
        playerFrame.cameraState=Read<const void*>(camera,player::cameraState);
    }
}
void Material(void* p,MaterialInputs& out){
    auto* property=Read<const void*>(p,pass::property);if(!Plausible(property))return;
    const auto flags=Read<std::uint64_t>(property,property::flags);
    auto* material=Read<const void*>(property,property::material);
    if(!Plausible(material)||Read<std::uintptr_t>(material,0)!=ModuleBase()+effectMaterial::vtable||!(flags&property::envMapFlag))return;
    out.effectEnvironment=true;
    out.strength=Read<float>(material,effectMaterial::environmentScale);
    out.lightingInfluence=Read<std::uint8_t>(material,effectMaterial::lightingInfluence)/255.f;
    out.environmentTechnique=(Read<std::uint32_t>(p,pass::technique)&(1u<<10))!=0;
    out.baseAlpha=Read<float>(material,effectMaterial::baseColor+12);
    out.falloff=(flags&property::falloffFlag)!=0;
    std::memcpy(out.falloffData,static_cast<const std::byte*>(material)+effectMaterial::falloff,sizeof(out.falloffData));
}
void SunShadow(SunShadowInputs& out){
    auto* scene=Read<const void*>(reinterpret_cast<const void*>(ModuleBase()+kShaderManagerState),0);if(!Plausible(scene))return;
    auto* light=Read<const void*>(scene,shadow::sceneDirectionalLight);if(!Plausible(light))return;
    const auto count=Read<std::uint32_t>(light,shadow::count);auto* entries=Read<const std::byte*>(light,shadow::entries);
    if(count<1||count>4||!Plausible(entries))return;
    const auto target=Read<std::int32_t>(entries,shadow::depthStencilTarget);
    if(target<0||std::uint32_t(target)>=depth::logicalCount)return;
    const auto slot=Read<std::int32_t>(reinterpret_cast<const void*>(ModuleBase()+depth::renderTargetManager),depth::logicalToSlot+4*std::size_t(target));
    auto* data=Global<const void*>(depth::rendererDataCell);
    if(slot<0||slot>=depth::slotCount||!Plausible(data))return;
    auto* view=Read<ID3D11ShaderResourceView*>(data,depth::targets+depth::targetStride*std::size_t(slot)+depth::depthView);if(!Plausible(view))return;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if(!resource||FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);if(desc.SampleDesc.Count!=1)return;
    for(unsigned i=0;i<count;i++){
        const auto* entry=entries+i*shadow::entryStride;
        const auto slice=Read<std::uint32_t>(entry,shadow::arraySlice);
        if(Read<std::int32_t>(entry,shadow::depthStencilTarget)!=target||slice>=desc.ArraySize)return;
        std::memcpy(out.worldToShadow[i].data(),entry+shadow::worldToShadow,sizeof(float)*16);
        std::memcpy(out.viewport[i].data(),entry+shadow::viewport,sizeof(int)*4);
        out.slice[i]=slice;
    }
    out.view=view;out.cascades=count;out.width=desc.Width;out.height=desc.Height;
}
void ReportFirstDraw(ID3D11DeviceContext* ctx,const BottleDrawInputs& in,const DrawCall& call,bool consumed){
    static bool reported=false;if(reported)return;reported=true;
    D3D11_VIEWPORT viewport{};UINT n=1;ctx->RSGetViewports(&n,&viewport);
    REX::INFO("First bottle draw: {} indices={} instances={} viewport={}x{} eye=({:.1f},{:.1f},{:.1f}) projection={} {:.3f}x{:.3f} "
        "sun={} lights={} ambient={} held={} consumed={} context={}",in.geometry->name.c_str(),call.indexCount,call.instanceCount,viewport.Width,viewport.Height,
        in.eye.x,in.eye.y,in.eye.z,in.projection.valid,in.projection.frustumWidth,in.projection.frustumHeight,in.sun.valid,in.lightCount,in.ambient.valid,in.held,consumed,ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE?"immediate":"deferred");
}
}

// The draw that follows goes to the renderer's selected context; make sure its vtable is
// hooked before the scope opens (one-time per distinct vtable).
void HookSelectedContext(){
    if(!hookedContext)return;
    auto* selected=Global<ID3D11DeviceContext*>(kD3DSelectedContext);
    if(!HookContextDraws(selected)){static bool reported=false;if(!reported){reported=true;REX::WARN("Selected D3D11 context {:#x} could not be hooked",reinterpret_cast<std::uintptr_t>(selected));}}
}
void OnPassDraw(void* p,void* context,const PassDraw& draw){
    if(IsOwnedOpticalPass(p)){
        {std::lock_guard lock(renderMutex);HookSelectedContext();}
        auto* previous=registeredGeometry;registeredGeometry=PassGeometry(p);draw.Invoke();registeredGeometry=previous;return;
    }
    auto* previous=active;auto* previousContext=activeContext;auto* previousDrink=activeDrink;
    active=nullptr;activeContext=nullptr;activeDrink=nullptr;
    // Ignore unrelated geometry before looking up the fixed profiles.
    auto* g=Plausible(p)?PassGeometry(p):nullptr;
    auto* shader=Plausible(p)?Read<const void*>(p,pass::shader):nullptr;
    if(!previous&&Tagged(g)&&Plausible(shader)&&Read<std::uintptr_t>(shader,0)==ModuleBase()+kEffectShaderVtable){
        if(Match(p,g)){
            std::lock_guard lock(renderMutex);
            try{EnsureRenderer();HookSelectedContext();if(hookedContext){active=p;activeContext=context;activeDrink=Profile(g);}}
            catch(const std::exception& e){REX::ERROR("Renderer initialization failed; keeping source glass: {}",e.what());}
        }
    }
    // Scope covers the actual geometry draw, independent of cached setup/restore calls.
    draw.Invoke();active=previous;activeContext=previousContext;activeDrink=previousDrink;
}

void OnIndexedDraw(ID3D11DeviceContext* ctx,const DrawCall& call){
    if(NativeOptics::busy){call.Issue(ctx);return;}
    if(registeredGeometry){
        try{Native().DrawRegistered(ctx,registeredGeometry,call);}catch(const std::exception& e){Native().Fail(e.what());}return;
    }
    if(active&&activeDrink){
        std::lock_guard lock(renderMutex);
        try{
            BottleDrawInputs in;in.geometry=PassGeometry(active);in.drink=activeDrink;in.now=Now();
            in.frame=Global<std::uint32_t>(kRenderFrameCounter);
            in.havePosition=StereoView(ctx,in.eye,in.projection);in.localEyeValid=in.havePosition;
            // Fallout 4 VR renders the body and held items in world space: there is no
            // translated first-person origin, and each eye's ray comes from its native
            // ModelView (cameraMeshEye stays false).
            UpdatePlayerFrame(in.frame);
            in.firstPerson=Under(in.geometry,playerFrame.firstPersonRoot);
            in.held=in.firstPerson||Under(in.geometry,playerFrame.grabbedRoots[0])||Under(in.geometry,playerFrame.grabbedRoots[1])||
                std::string_view(in.geometry->name.c_str()).starts_with("DynamicBottleLiquids:iafanimobject");
            in.carry=MakeCarryFrame(playerFrame.viewRotation,in.eye,playerFrame.cameraState,in.havePosition);
            if(in.firstPerson)in.carry.originIdentity=playerFrame.firstPersonRoot;
            Ambient(in.ambient);
            const auto* sun=Sun(activeContext,in.sun);
            std::array<LocalLightInput,64> lights;in.lightCount=Lights(active,in.geometry,sun,lights);in.lights=lights.data();
            Material(active,in.material);
            SunShadow(in.shadow);
            const bool consumed=DrawBottle(ctx,renderer,in,call);
            ReportFirstDraw(ctx,in,call,consumed);
            if(consumed)return;
        }catch(const std::exception& e){static bool reported=false;if(!reported){reported=true;REX::ERROR("Bottle draw failed: {}",e.what());}}
    }
    call.Issue(ctx);
}
}

namespace {
void InitializeLog(){
    wchar_t documents[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_MYDOCUMENTS,nullptr,SHGFP_TYPE_CURRENT,documents)))throw std::runtime_error("Documents folder is unavailable");
    const auto directory=std::filesystem::path(documents)/L"My Games"/L"Fallout4VR"/L"F4SE";
    std::filesystem::create_directories(directory);
    auto sink=std::make_shared<spdlog::sinks::basic_file_sink_mt>((directory/L"DynamicBottleLiquids.log").string(),true);
    auto logger=std::make_shared<spdlog::logger>("DynamicBottleLiquids",std::move(sink));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}
void F4SEAPI OnMessage(F4SE::MessagingInterface::Message* message) noexcept{
    if(message&&(message->type==F4SE::MessagingInterface::kPreLoadGame||message->type==F4SE::MessagingInterface::kNewGame))nuka::ResetBottleFrames();
}
}

// Query rejects the editor and anything but Fallout4VR.exe 1.2.72.0 (the module version, never
// QueryInterface::RuntimeVersion(), which reports F4SEVR's flat-compatibility value).
extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* query,F4SE::PluginInfo* info) noexcept{
    try{
        if(!query||!info)return false;
        InitializeLog();
        info->infoVersion=F4SE::PluginInfo::kVersion;info->name="DynamicBottleLiquids";info->version=0x01000000;
        if(query->IsEditor()||!REL::Module::IsVR()||REL::Module::get().version()!=F4SE::RUNTIME_VR_1_2_72){
            spdlog::error("Fallout4VR.exe 1.2.72.0 is required");return false;
        }
        spdlog::info("Query complete");return true;
    }catch(...){return false;}
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* load) noexcept{
    try{
        if(!load)return false;
        F4SE::Init(load,false);
        nuka::vr::drinks=nuka::LoadDrinks();
        spdlog::info("DynamicBottleLiquids 1.0.0 (Fallout 4 VR): fixed profile keys={}; basic health logging enabled",nuka::vr::drinks.size());
        if(auto result=MH_Initialize();result!=MH_OK&&result!=MH_ERROR_ALREADY_INITIALIZED){spdlog::error("MinHook initialization failed");return false;}
        if(!nuka::vr::InstallEngineHooks())return false;
        const auto* messaging=F4SE::GetMessagingInterface();
        if(!messaging||!messaging->RegisterListener(OnMessage)){spdlog::error("F4SE message registration failed");return false;}
        spdlog::info("Load complete");return true;
    }catch(const std::exception& e){spdlog::error("DynamicBottleLiquids disabled: {}",e.what());return false;}
    catch(...){return false;}
}
