#include "PCH.h"
#include "NativeOpticsHooks.h"
#include "NativeOptics.h"
#include <RE/B/BSEffectShaderProperty.h>
#include <RE/B/BSUtilityShader.h>
#include <RE/B/BSShaderAccumulator.h>
#include <MinHook.h>
namespace nuka {
namespace {
bool tagged(RE::BSGeometry* g){return g&&g->name.c_str()&&std::string_view(g->name.c_str()).starts_with("DynamicBottleLiquids:");}
using Builder=RE::BSShaderProperty::RenderPassArray*(*)(RE::BSEffectShaderProperty*,RE::BSGeometry*,std::uint32_t,RE::BSShaderAccumulator*);
Builder originalBuilder{};
using HDRRender=void(*)(void*,void*,void*);
HDRRender originalHDR{},originalHDRCS{};
void HDR(void* self,void* geometry,void* parameter){try{Native().ComposeBeforeHDR();}catch(const std::exception& e){Native().Fail(e.what());}originalHDR(self,geometry,parameter);}
void HDRCS(void* self,void* geometry,void* parameter){try{Native().ComposeBeforeHDR();}catch(const std::exception& e){Native().Fail(e.what());}originalHDRCS(self,geometry,parameter);}

RE::BSShaderProperty::RenderPassArray* Build(RE::BSEffectShaderProperty* p,RE::BSGeometry* g,std::uint32_t mode,RE::BSShaderAccumulator* a){
    auto* result=originalBuilder(p,g,mode,a);
    if(!result||!result->passList||!tagged(g))return result;
    // Only append to a list containing the supported ordinary Effect colour variant.
    bool colour=false;unsigned links=0;
    auto* utility=RE::BSUtilityShader::GetSingleton();if(!utility)return result;
    for(auto* pass=result->passList;pass&&links++<128;pass=pass->propertyNext){
        if(pass->shader==utility&&pass->passType==5&&pass->passEnum==0x61A)return result;
        if(pass->geometry==g&&pass->shader!=utility&&(pass->passEnum&((1u<<4)|(1u<<19)|(1u<<21)))&&!(pass->passEnum&((1u<<1)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<18)|(1u<<29))))colour=true;
    }
    if(!colour||links>=128)return result;
    auto* pass=result->Add(utility,p,g,0x61A,0,nullptr,nullptr,nullptr,nullptr);
    if(pass){pass->passType=5;}
    return result;
}
}
void InstallOpticsHooks(){
    auto* target=reinterpret_cast<void*>(RE::ID::BSEffectShaderProperty::GetRenderPasses.address());
    if(MH_CreateHook(target,reinterpret_cast<void*>(&Build),reinterpret_cast<void**>(&originalBuilder))!=MH_OK||MH_EnableHook(target)!=MH_OK)throw std::runtime_error("Native optics pass builder hook failed");
    auto hookHDR=[](REL::ID id,HDRRender replacement,HDRRender& original){
        REL::Relocation<std::uintptr_t> table{id};auto* entry=reinterpret_cast<void**>(table.address());
        if(MH_CreateHook(entry[1],reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(&original))!=MH_OK||MH_EnableHook(entry[1])!=MH_OK)throw std::runtime_error("Native optics HDR-entry hook failed");
    };
    hookHDR(RE::VTABLE::ImageSpaceEffectHDR[0],&HDR,originalHDR);
    hookHDR(RE::VTABLE::ImageSpaceEffectHDRCS[0],&HDRCS,originalHDRCS);

}
bool IsOwnedOpticalPass(const RE::BSRenderPass* p){return p&&p->passType==5&&p->passEnum==0x61A&&p->shader==RE::BSUtilityShader::GetSingleton()&&tagged(p->geometry);}
}
