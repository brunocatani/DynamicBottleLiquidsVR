#include "PCH.h"
#include "VrHooks.h"
#include "VrEngine.h"
#include "NativeOptics.h"
#include <MinHook.h>
#include <atomic>
namespace nuka::vr {
std::uintptr_t ModuleBase(){static const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));return base;}
bool EntryMatches(std::uintptr_t rva,std::span<const std::uint8_t> bytes){
    const auto* live=reinterpret_cast<const std::uint8_t*>(ModuleBase()+rva);
    return std::memcmp(live,bytes.data(),bytes.size())==0;
}
namespace {
// Effect technique selection shared with the flat build: ordinary colour variants whose
// VS b2 holds WorldViewProj then ModelView (per eye in VR). Bit 29 selects a different
// raw-buffer geometry path, so it stays excluded.
constexpr std::uint32_t kRequiredTechnique=(1u<<4)|(1u<<19)|(1u<<21);
constexpr std::uint32_t kExcludedTechnique=(1u<<1)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<18)|(1u<<29);
bool Tagged(const RE::BSGeometry* g){
    if(!Plausible(g))return false;auto* name=g->name.c_str();
    return name&&std::string_view(name).starts_with("DynamicBottleLiquids:");
}
using BuilderFn=void*(*)(void* property,RE::BSGeometry* geometry,std::uint32_t mode,void* accumulator);
using AppendFn=void*(*)(void* list,void* shader,void* property,std::uint32_t technique,std::uint8_t passType,
    void* geometry,std::uint8_t lightCount,void*,void*,void*,void*);
using HdrFn=void(*)(void*,void*,void*,void*);
using DispatchFn=void(*)(void* pass,void* context);
using PrepareFn=void(*)(void* owner,std::int32_t passIndex);
using CachedDrawFn=void(*)(void* renderer,void* record,void* command);
BuilderFn originalBuilder{};DispatchFn originalDispatch{};HdrFn originalHdr{},originalHdrCompute{};
PrepareFn originalPrepare{};CachedDrawFn originalCachedDraw{};
AppendFn appendPass{};
// One entry per vtable we patched, with the functions we chain to. Some overlay layers give
// the context a heap-copied vtable and later swap or rebuild it, so tables are re-checked
// before every bottle pass. A copy made from an already patched table holds our functions;
// it chains through the most recently patched table's targets.
struct HookedTable {void** table;DrawCall::IndexedFn indexed;DrawCall::InstancedFn instanced;};
std::array<HookedTable,16> hookedTables{};std::size_t hookedTableCount=0,latestTable=0;
const HookedTable& FindTable(ID3D11DeviceContext* c){
    auto** table=*reinterpret_cast<void***>(c);
    for(std::size_t i=0;i<hookedTableCount;i++)if(hookedTables[i].table==table)return hookedTables[i];
    return hookedTables[latestTable];
}
std::string ModuleOf(const void* address){
    HMODULE module{};wchar_t path[MAX_PATH]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(address),&module)||
        !GetModuleFileNameW(module,path,MAX_PATH))return "heap";
    return std::filesystem::path(path).filename().string();
}
// The appended pass is only safe while the hooked dispatcher intercepts it; drawn by the
// engine itself it renders the utility technique over the bottle. Stop appending if 600
// appends pass without one interception.
bool opticalDispatchSeen=false,appendDisabled=false;std::uint64_t appendsWithoutDispatch=0;

void* Utility(){auto* u=Global<void*>(kUtilityShaderSingleton);return Plausible(u)?u:nullptr;}
// Registers the delayed optical pass beside the supported ordinary Effect colour pass,
// exactly as the flat builder hook does, through the VR append ABI.
void* Build(void* property,RE::BSGeometry* geometry,std::uint32_t mode,void* accumulator){
    auto* list=originalBuilder(property,geometry,mode,accumulator);
    if(!list||!Tagged(geometry))return list;
    auto* utility=Utility();auto* head=Read<void*>(list,0);if(!utility||!head)return list;
    bool colour=false;unsigned links=0;
    for(void* p=head;p&&links++<128;p=Read<void*>(p,pass::next)){
        if(!Plausible(p))return list;
        const auto technique=Read<std::uint32_t>(p,pass::technique);const auto* shader=Read<void*>(p,pass::shader);
        if(shader==utility&&technique==kOpticalTechnique)return list;
        if(Read<void*>(p,pass::geometry)==geometry&&shader!=utility&&(technique&kRequiredTechnique)&&!(technique&kExcludedTechnique))colour=true;
    }
    if(!colour||links>=128||appendDisabled)return list;
    if(!opticalDispatchSeen&&++appendsWithoutDispatch>600){
        appendDisabled=true;REX::ERROR("Optical pass never reached a hooked draw route after 600 appends; optical pass registration disabled");return list;
    }
    appendPass(list,utility,property,kOpticalTechnique,kOpticalPassType,geometry,0,nullptr,nullptr,nullptr,nullptr);
    return list;
}
void Dispatch(void* p,void* context){
    if(Plausible(p)&&Tagged(Read<RE::BSGeometry*>(p,pass::geometry))){
        if(IsOwnedOpticalPass(p))opticalDispatchSeen=true;
    }
    OnPassDraw(p,context,{[](const PassDraw& d){originalDispatch(d.a,d.b);},p,context,nullptr});
}
// Prepared record -> tagged pass. Preparation may run as queued work, so the map is
// locked; only bottle passes are stored, and each entry keeps its geometry alive until it
// is replaced, re-prepared for other geometry, or unused for 300 frames.
struct RecordEntry {void* pass;RE::NiPointer<RE::BSGeometry> geometry;std::uint32_t frame;};
std::mutex recordMutex;
std::unordered_map<const void*,RecordEntry> records;
std::atomic<std::size_t> recordCount{0};
void Prepare(void* owner,std::int32_t index){
    originalPrepare(owner,index);
    auto* passes=Plausible(owner)?Read<void* const*>(owner,kOwnerPasses):nullptr;
    auto* p=Plausible(passes)&&index>=0?passes[index]:nullptr;if(!Plausible(p))return;
    auto* record=Read<void*>(p,pass::preparedRecord);if(!record)return;
    auto* geometry=Read<RE::BSGeometry*>(p,pass::geometry);
    const bool tagged=Tagged(geometry);
    if(!tagged&&recordCount.load(std::memory_order_relaxed)==0)return;
    std::lock_guard lock(recordMutex);
    if(tagged){
        auto& entry=records[record];entry.pass=p;
        if(entry.geometry.get()!=geometry)entry.geometry=RE::NiPointer<RE::BSGeometry>(geometry);
        entry.frame=Global<std::uint32_t>(kRenderFrameCounter);
    }else records.erase(record);
    recordCount=records.size();
}
void CachedDraw(void* renderer,void* record,void* command){
    void* p=nullptr;
    if(recordCount.load(std::memory_order_relaxed)){
        std::lock_guard lock(recordMutex);
        const auto frame=Global<std::uint32_t>(kRenderFrameCounter);
        static std::uint32_t pruned=~0u;
        if(pruned!=frame){pruned=frame;std::erase_if(records,[&](const auto& e){return frame-e.second.frame>300;});recordCount=records.size();}
        if(auto i=records.find(record);i!=records.end()){
            // The pass must still own this record and draw the geometry it was mapped with.
            if(Read<void*>(i->second.pass,pass::preparedRecord)==record&&Read<RE::BSGeometry*>(i->second.pass,pass::geometry)==i->second.geometry.get()){
                p=i->second.pass;i->second.frame=frame;
            }else{records.erase(i);recordCount=records.size();}
        }
    }
    if(!p){originalCachedDraw(renderer,record,command);return;}
    if(IsOwnedOpticalPass(p))opticalDispatchSeen=true;
    OnPassDraw(p,nullptr,{[](const PassDraw& d){originalCachedDraw(d.a,d.b,d.c);},renderer,record,command});
}
void ComposeBeforeHdr(){try{Native().ComposeBeforeHDR();}catch(const std::exception& e){Native().Fail(e.what());}}
void Hdr(void* self,void* input,void* output,void* parameters){ComposeBeforeHdr();originalHdr(self,input,output,parameters);}
void HdrCompute(void* self,void* input,void* output,void* parameters){ComposeBeforeHdr();originalHdrCompute(self,input,output,parameters);}

void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c,UINT count,UINT start,INT base){
    OnIndexedDraw(c,DrawCall::Indexed(FindTable(c).indexed,count,start,base));
}
void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c,UINT count,UINT instances,UINT start,INT base,UINT firstInstance){
    OnIndexedDraw(c,DrawCall::Instanced(FindTable(c).instanced,count,instances,start,base,firstInstance));
}
struct Target {const char* name;std::uintptr_t rva;std::span<const std::uint8_t> bytes;void* detour;void** original;};
}
bool IsOwnedOpticalPass(const void* p){
    auto* utility=Utility();
    return utility&&Plausible(p)&&Read<void*>(p,pass::shader)==utility&&
        Read<std::uint32_t>(p,pass::technique)==kOpticalTechnique&&Tagged(Read<RE::BSGeometry*>(p,pass::geometry));
}
bool InstallEngineHooks(){
    const Target targets[]{
        {"Effect pass builder",kEffectPassBuilder,kEffectPassBuilderBytes,reinterpret_cast<void*>(&Build),reinterpret_cast<void**>(&originalBuilder)},
        {"geometry draw dispatcher",kDrawDispatcher,kDrawDispatcherBytes,reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&originalDispatch)},
        {"HDR render",kHdrRender,kHdrRenderBytes,reinterpret_cast<void*>(&Hdr),reinterpret_cast<void**>(&originalHdr)},
        {"HDR compute render",kHdrComputeRender,kHdrComputeRenderBytes,reinterpret_cast<void*>(&HdrCompute),reinterpret_cast<void**>(&originalHdrCompute)},
        {"render pass preparation",kPreparePass,kPreparePassBytes,reinterpret_cast<void*>(&Prepare),reinterpret_cast<void**>(&originalPrepare)},
        {"cached record draw",kCachedDraw,kCachedDrawBytes,reinterpret_cast<void*>(&CachedDraw),reinterpret_cast<void**>(&originalCachedDraw)},
    };
    // Verify every target before touching any of them; no partial installation.
    bool ready=EntryMatches(kAppendPass,kAppendPassBytes);
    if(!ready)REX::ERROR("Render pass append at RVA {:#x} does not match Fallout4VR 1.2.72; hooks not installed",kAppendPass);
    for(const auto& t:targets)if(!EntryMatches(t.rva,t.bytes)){ready=false;REX::ERROR("{} at RVA {:#x} does not match Fallout4VR 1.2.72 (patched by another mod?); hooks not installed",t.name,t.rva);}
    if(!ready)return false;
    appendPass=reinterpret_cast<AppendFn>(ModuleBase()+kAppendPass);
    std::vector<void*> created;
    for(const auto& t:targets){
        auto* address=reinterpret_cast<void*>(ModuleBase()+t.rva);
        if(MH_CreateHook(address,t.detour,t.original)!=MH_OK){
            REX::ERROR("Cannot create {} hook",t.name);for(auto* a:created)MH_RemoveHook(a);return false;
        }
        created.push_back(address);
    }
    for(auto* a:created)if(MH_EnableHook(a)!=MH_OK){
        REX::ERROR("Cannot enable engine hooks");for(auto* b:created){MH_DisableHook(b);MH_RemoveHook(b);}return false;
    }
    return true;
}
bool HookContextDraws(ID3D11DeviceContext* context){
    if(!Plausible(context))return false;
    auto** table=*reinterpret_cast<void***>(context);if(!Plausible(table))return false;
    auto* ourIndexed=reinterpret_cast<void*>(&DrawIndexed);auto* ourInstanced=reinterpret_cast<void*>(&DrawIndexedInstanced);
    if(table[12]==ourIndexed&&table[20]==ourInstanced)return true;
    HookedTable* entry=nullptr;
    for(std::size_t i=0;i<hookedTableCount;i++)if(hookedTables[i].table==table)entry=&hookedTables[i];
    if(!entry){
        if(hookedTableCount>=hookedTables.size()){
            static bool reported=false;if(!reported){reported=true;REX::ERROR("D3D11 context vtable keeps changing; stopped re-hooking after {} tables",hookedTables.size());}
            return false;
        }
        entry=&hookedTables[hookedTableCount++];entry->table=table;
    }
    // Chain to whatever the slot holds now unless it is already ours (a rebuilt copy).
    if(table[12]!=ourIndexed)entry->indexed=reinterpret_cast<DrawCall::IndexedFn>(table[12]);
    if(table[20]!=ourInstanced)entry->instanced=reinterpret_cast<DrawCall::InstancedFn>(table[20]);
    if(!entry->indexed||!entry->instanced)return false;
    latestTable=std::size_t(entry-hookedTables.data());
    DWORD old{};
    if(!VirtualProtect(&table[12],sizeof(void*)*9,PAGE_READWRITE,&old))return false;
    table[12]=ourIndexed;table[20]=ourInstanced;
    DWORD restored{};VirtualProtect(&table[12],sizeof(void*)*9,old,&restored);
    FlushInstructionCache(GetCurrentProcess(),&table[12],sizeof(void*)*9);
    static std::size_t reported=0;
    if(hookedTableCount>reported){reported=hookedTableCount;
        REX::INFO("Hooked draw calls of a {} D3D11 context: vtable {:#x} ({}), chaining to {} / {}",
            context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE?"immediate":"deferred",reinterpret_cast<std::uintptr_t>(table),ModuleOf(table),
            ModuleOf(reinterpret_cast<void*>(entry->indexed)),ModuleOf(reinterpret_cast<void*>(entry->instanced)));
    }else{
        static bool reportedRestore=false;
        if(!reportedRestore){reportedRestore=true;REX::INFO("Another layer restored the D3D11 context draw slots; they are re-hooked before each bottle pass");}
    }
    return true;
}
}
