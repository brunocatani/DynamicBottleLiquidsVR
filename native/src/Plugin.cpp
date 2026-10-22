#include "PCH.h"
#include "Profiles.h"
#include "Renderer.h"
#include "NativeOptics.h"
#include "NativeOpticsHooks.h"
#include "BottleFrame.h"
#include "AssignedLights.h"
#include "CarryFrame.h"
#include <RE/B/BSShaderManager.h>
#include <RE/B/BSLight.h>
#include <RE/S/Sky.h>
#include <RE/S/Sun.h>
#include <RE/I/ImageSpaceManager.h>
#undef near
#undef far
#include <RE/N/NiCamera.h>
#include <RE/N/NiDirectionalLight.h>
#include <RE/B/BSGraphics.h>
#include <RE/B/BSEffectShaderMaterial.h>
#include <RE/B/BSShadowDirectionalLight.h>
#include <RE/S/ShadowSceneNode.h>
#include <RE/H/hkRefPtr.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include "BottleSpace.h"
#include <atomic>
#include <RE/M/Main.h>
#include <RE/S/Setting.h>
#include <MinHook.h>

namespace {
using namespace nuka;
const std::filesystem::path root=L"Data/F4SE/Plugins/DynamicBottleLiquids";
thread_local const DrinkProfile* activeDrink=nullptr;
DrinkMap drinks;
Renderer renderer;
std::mutex renderMutex;
ID3D11DeviceContext* hookedContext{};
using BatchFn=void (*)(RE::BSRenderPass*);
using DrawFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
BatchFn originalBatch{};
DrawFn originalDraw{};
const void* registeredGeometry=nullptr;
thread_local RE::BSRenderPass* active{};
bool Under(const RE::NiAVObject* object,const RE::NiAVObject* root){
    if(!root)return false;
    for(unsigned depth=0;object&&depth<128;object=object->parent,++depth)if(object==root)return true;
    return false;
}
bool Match(RE::BSRenderPass* pass){

    if(!pass||!pass->geometry||!pass->shaderProperty||pass->instanceGroupPass||pass->debrisInstanceGroupPass)return false;
    auto* g=pass->geometry;
    auto it=drinks.find(g->name.c_str()?g->name.c_str():"");if(it==drinks.end())return false;
    auto* tri=g->IsTriShape();
    if(!tri||!FindProfile(drinks,g->name.c_str(),tri->numVertices,tri->numTriangles*3)||g->vertexDesc.desc!=0x1b00000430205ull)return false;
    if(!g->parent)return false;
    const auto d=pass->passEnum;
    // These Effect variants have ModelView directly after WorldViewProj in VS b2.
    constexpr unsigned required=(1u<<4)|(1u<<19)|(1u<<21);
    constexpr unsigned excluded=(1u<<1)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<18)|(1u<<29);
    return (d&required)&&!(d&excluded);
}
double Now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void SunShadow(SunShadowInputs& out){
    auto* state=RE::BSShaderManager::State::GetSingleton();auto* scene=state?state->GetShadowSceneNode(0):nullptr;
    auto* light=scene?scene->directionalShadowLight:nullptr;
    auto* manager=RE::BSGraphics::RenderTargetManager::GetSingleton();auto* data=RE::BSGraphics::GetRendererData();
    if(!light||!light->shadowMapData||!manager||!data||light->shadowMapCount<1||light->shadowMapCount>4)return;
    auto target=light->shadowMapData[0].depthStencilTarget;if(target>=12)return;
    auto slot=manager->GetDepthStencilTargetPlatformID(target);if(slot>=std::size(data->depthStencilTargets))return;
    auto* view=reinterpret_cast<ID3D11ShaderResourceView*>(data->depthStencilTargets[slot].srViewDepth);if(!view)return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> tex;if(FAILED(resource.As(&tex)))return;
    D3D11_TEXTURE2D_DESC desc{};tex->GetDesc(&desc);if(desc.SampleDesc.Count!=1)return;
    for(unsigned i=0;i<light->shadowMapCount;i++){
        auto& entry=light->shadowMapData[i];if(entry.depthStencilTarget!=target||entry.arraySlice>=desc.ArraySize)return;
        std::memcpy(out.worldToShadow[i].data(),entry.worldToShadow,sizeof(float)*16);
        std::memcpy(out.viewport[i].data(),entry.viewport,sizeof(int)*4);
        out.slice[i]=entry.arraySlice;
    }
    out.view=view;out.cascades=light->shadowMapCount;out.width=desc.Width;out.height=desc.Height;
}
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base){
    if(NativeOptics::busy){originalDraw(ctx,count,start,base);return;}
    const auto call=DrawCall::Indexed(originalDraw,count,start,base);
    if(ctx==hookedContext&&registeredGeometry){try{Native().DrawRegistered(ctx,registeredGeometry,call);}catch(const std::exception& e){Native().Fail(e.what());}return;}

    if(ctx==hookedContext&&active){
        std::lock_guard lock(renderMutex);
        try {
            BottleDrawInputs in;in.geometry=active->geometry;in.drink=activeDrink;in.now=Now();
            auto* graphics=RE::BSGraphics::State::GetSingleton();
            in.frame=graphics->frameCount;
            auto* camera=graphics->cameraState.referenceCamera;
            static RE::NiPoint3 cameraWorld{};static unsigned cameraFrame=~0u;static bool havePosition=false;
            static CarryFrame playerCarry;
            if(cameraFrame!=graphics->frameCount){cameraFrame=graphics->frameCount;auto* playerCamera=RE::PlayerCamera::GetSingleton();
                havePosition=playerCamera&&playerCamera->GetCameraPosition(cameraWorld,true)&&std::isfinite(cameraWorld.x)&&std::isfinite(cameraWorld.y)&&std::isfinite(cameraWorld.z);
                const auto* viewRoot=playerCamera?playerCamera->cameraRoot.get():nullptr;
                playerCarry=MakeCarryFrame(viewRoot?&viewRoot->world.rotate:nullptr,{cameraWorld.x,cameraWorld.y,cameraWorld.z},
                    playerCamera?playerCamera->currentState.get():nullptr,havePosition);
            }
            Vec eye{cameraWorld.x,cameraWorld.y,cameraWorld.z};
            auto* player=RE::PlayerCharacter::GetSingleton();
            auto* firstRoot=player?player->firstPerson3D.get():nullptr;
            bool firstPerson=Under(active->geometry,firstRoot);
            Vec offset{};bool localEyeValid=havePosition;
            if(firstPerson){
                auto* anchor=player->firstPersonEye;
                localEyeValid=havePosition&&anchor;
                if(localEyeValid){auto p=anchor->world.translate;eye={p.x,p.y,p.z};localEyeValid=std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
                if(localEyeValid)offset=AnimationWorldOffset({cameraWorld.x,cameraWorld.y,cameraWorld.z},eye);
            }
            auto grabbed=player?player->grabbedObject.get():RE::NiPointer<RE::TESObjectREFR>{};
            in.held=firstPerson||(grabbed&&Under(active->geometry,grabbed->Get3D()))||std::string_view(active->geometry->name.c_str()).starts_with("DynamicBottleLiquids:iafanimobject");
            in.eye=eye;in.havePosition=havePosition;in.localEyeValid=localEyeValid;in.firstPerson=firstPerson;in.offset=offset;
            if(camera&&!camera->viewFrustum.ortho){
                D3D11_VIEWPORT viewport{};UINT n=1;ctx->RSGetViewports(&n,&viewport);auto& f=camera->viewFrustum;
                in.projection={true,f.top-f.bottom,f.right-f.left,viewport.Width,viewport.Height};
            }
            in.carry=playerCarry;in.carry.eye=eye;in.carry.valid=in.carry.valid&&localEyeValid;
            // A first-person skeleton and a world-space grabbed object must not
            // share derivative history when the coordinate origin changes.
            if(firstPerson)in.carry.originIdentity=firstRoot;
            in.cameraMeshEye=!firstPerson&&havePosition;
            if(auto* state=RE::BSShaderManager::State::GetSingleton()){
                const auto& a=state->GetDirectionalAmbientTransform();in.ambient.valid=true;
                in.ambient.translate={a.translate.x,a.translate.y,a.translate.z};in.ambient.scale=a.scale;
                for(int axis=0;axis<3;axis++)in.ambient.axes[axis]={a.rotate[axis].x,a.rotate[axis].y,a.rotate[axis].z};
            }
            RE::NiLight* sun=nullptr;
            auto* sky=RE::Sky::GetSingleton();
            if(sky&&sky->mode==RE::Sky::Mode::kFull&&sky->sun&&sky->sun->light){
                sun=reinterpret_cast<RE::NiLight*>(sky->sun->light.get());
                auto d=sky->sun->light->direction;
                in.sun={true,{d.x,d.y,d.z},{sun->diff.r,sun->diff.g,sun->diff.b},sun->dimmer,1};
                if(auto* image=RE::ImageSpaceManager::GetSingleton())in.sun.sunlightScale=image->currentEOFData.baseData.hdrData.sunlightScale;
            }
            std::array<LocalLightInput,64> lights;unsigned lightCount=0;
            ForEachAssignedLight(active->sceneLights,active->numLights,[&](RE::BSLight* entry){
                if(!entry->light||entry->light.get()==sun||entry->shape==0||lightCount>=lights.size())return;
                auto* l=entry->light.get();
                auto d=active->geometry->worldBound.center-l->world.translate;
                // Attached lights share the first-person origin; world lights do not.
                if(!Under(l,firstPerson?firstRoot:nullptr)){d.x+=offset.x;d.y+=offset.y;d.z+=offset.z;}
                lights[lightCount++]={{d.x,d.y,d.z},l->radius.x,l->dimmer,entry->currentFade,{l->diff.r,l->diff.g,l->diff.b}};
            });
            in.lights=lights.data();in.lightCount=lightCount;
            // Read the live ordinary Effect material; external BGEM and embedded
            // NIF materials share this layout. Do not change flags or patch assets.
            auto* material=active->shaderProperty->material;
            if(material&&material->GetType()==RE::BSShaderMaterial::Type::kEffect&&
               active->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kEnvMap)){
                auto* effect=static_cast<RE::BSEffectShaderMaterial*>(material);
                auto& m=in.material;m.effectEnvironment=true;m.strength=effect->environmentMaskScaleOrRefractionPower;
                m.lightingInfluence=effect->lightingInfluence/255.f;m.environmentTechnique=(active->passEnum&(1u<<10))!=0;
                m.baseAlpha=effect->baseColor.a;m.falloff=active->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFalloff);
                for(int i=0;i<4;i++)m.falloffData[i]=effect->falloffData[i];
            }
            SunShadow(in.shadow);
            if(DrawBottle(ctx,renderer,in,call))return;
        }catch(const std::exception& e){static bool reported=false;if(!reported){reported=true;REX::ERROR("Bottle draw failed: {}",e.what());}}
    }
    originalDraw(ctx,count,start,base);
}
void EnsureRenderer(){
    if(hookedContext)return;
    static bool attempted=false;if(attempted)return;
    auto* data=RE::BSGraphics::GetRendererData();if(!data||!data->device||!data->context)return;
    attempted=true;
    auto* device=reinterpret_cast<ID3D11Device*>(data->device);auto* ctx=reinterpret_cast<ID3D11DeviceContext*>(data->context);
    if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::runtime_error("Expected immediate context");
    renderer.Initialize(device,root/"shaders",2952);
    auto** table=*reinterpret_cast<void***>(ctx);
    if(MH_CreateHook(table[12],reinterpret_cast<void*>(&Draw),reinterpret_cast<void**>(&originalDraw))!=MH_OK)throw std::runtime_error("Cannot detour DrawIndexed");
    hookedContext=ctx;
    if(MH_EnableHook(table[12])!=MH_OK){hookedContext=nullptr;MH_RemoveHook(table[12]);throw std::runtime_error("Cannot enable DrawIndexed hook");}
    REX::INFO("D3D11 renderer initialized; glass replacement is ready.");

}

void BatchDraw(RE::BSRenderPass* pass){
    if(IsOwnedOpticalPass(pass)){auto* previous=registeredGeometry;registeredGeometry=pass->geometry;originalBatch(pass);registeredGeometry=previous;return;}
    auto* previous=active;auto* previousDrink=activeDrink;active=nullptr;activeDrink=nullptr;
    // Ignore unrelated geometry before looking up the fixed profiles.
    if(!previous&&(!pass||!pass->geometry||!pass->geometry->name.c_str()||!std::string_view(pass->geometry->name.c_str()).starts_with("DynamicBottleLiquids:"))){originalBatch(pass);active=previous;activeDrink=previousDrink;return;}

    if(Match(pass)){
        std::lock_guard lock(renderMutex);
        try {EnsureRenderer();if(hookedContext){active=pass;auto* tri=pass->geometry->IsTriShape();activeDrink=FindProfile(drinks,pass->geometry->name.c_str(),tri->numVertices,tri->numTriangles*3);}}
        catch(const std::exception& e){REX::ERROR("Renderer initialization failed; keeping source glass: {}",e.what());}

    }
    // Scope covers the actual geometry draw, independent of cached SetupGeometry/RestoreGeometry calls.
    originalBatch(pass);active=previous;activeDrink=previousDrink;
}
}
namespace {
constexpr bool SupportedRuntime(REL::Version v) {
    return v==REL::Version{1,10,163,0} || v==REL::Version{1,10,984,0} || v==REL::Version{1,11,240,0};
}
}
F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* f4se,F4SE::PluginInfo* info) {
    info->infoVersion=F4SE::PluginInfo::kVersion;info->name="DynamicBottleLiquids";info->version=0x01000000;
    return !f4se->IsEditor()&&SupportedRuntime(f4se->RuntimeVersion());
}
F4SE_PLUGIN_VERSION=[]() noexcept {
    F4SE::PluginVersionData v{};v.PluginVersion({1,0,0,0});v.PluginName("DynamicBottleLiquids");v.AuthorName("DynamicBottleLiquids contributors");
    v.UsesAddressLibrary(true);v.UsesAddressLibraryNG(true);v.IsLayoutDependent(true);v.IsLayoutDependentNG(true);v.CompatibleVersions({REL::Version{1,10,163,0},REL::Version{1,10,984,0},REL::Version{1,11,240,0}});return v;
}();
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* f4se){
    if(f4se->IsEditor()||!SupportedRuntime(f4se->RuntimeVersion()))return false;
    F4SE::Init(f4se);
    try {
        drinks=LoadDrinks();
        REX::INFO("DynamicBottleLiquids 1.0.0: runtime {}; fixed profile keys={}; basic health logging enabled",f4se->RuntimeVersion().string(),drinks.size());
        auto result=MH_Initialize();if(result!=MH_OK&&result!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook initialization failed");
        InstallOpticsHooks();
        auto* target=reinterpret_cast<void*>(RE::ID::BSBatchRenderer::Draw.address());
        if(MH_CreateHook(target,reinterpret_cast<void*>(&BatchDraw),reinterpret_cast<void**>(&originalBatch))!=MH_OK)throw std::runtime_error("Cannot hook BSBatchRenderer::Draw");
        if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);throw std::runtime_error("Cannot enable batch hook");}
        F4SE::GetMessagingInterface()->RegisterListener([](F4SE::MessagingInterface::Message* m){
            if(m->type==F4SE::MessagingInterface::kPreLoadGame||m->type==F4SE::MessagingInterface::kNewGame)ResetBottleFrames();
        });
        return true;
    }catch(const std::exception& e){REX::ERROR("DynamicBottleLiquids disabled: {}",e.what());return false;}
}
