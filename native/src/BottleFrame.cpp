#include "SunShadowBounds.h"
#include "PCH.h"
#include "BottleFrame.h"
#include "Renderer.h"
#include "NativeOptics.h"
#include "WorldDirection.h"
#include "AdaptiveQuality.h"
#include "ShakeMotion.h"
#include <atomic>
namespace nuka {
namespace {
std::atomic_bool resetRequested{};
struct Instance {
    RE::NiPointer<RE::BSGeometry> owner;
    Motion motion;
    Constants constants,previous,output;
    int raySteps=96;bool detail=true;float interval=0;
    Vec position{},velocity{},shakeAxis{};
    EmissionTracker emissionTracker;
    double time{};
    std::uint32_t frame=~0u;
};
std::unordered_map<RE::BSGeometry*,Instance> instances;
struct LodEntry {RE::NiPointer<RE::BSGeometry> owner;DistanceLod state;};
std::unordered_map<RE::BSGeometry*,LodEntry> distanceLods;
void CleanInstances(double now){
    static double next=0;if(now<next)return;next=now+.5;
    std::erase_if(instances,[&](const auto& item){return now-item.second.time>2;});
    std::erase_if(distanceLods,[&](const auto& item){return now-item.second.state.time>2;});
}
Vec Canonical(const Settings& s,Vec v){const auto* m=s.base.matrix;return {m[0]*v.x+m[1]*v.y+m[2]*v.z,m[4]*v.x+m[5]*v.y+m[6]*v.z,m[8]*v.x+m[9]*v.y+m[10]*v.z};}
const Constants& Update(const DrinkProfile& drink,RE::BSGeometry* g,std::uint32_t frame,bool detail,float pixels,bool held,const CarryFrame& carry){
    const auto& settings=drink.settings;
    const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(resetRequested.exchange(false)){instances.clear();distanceLods.clear();}
    CleanInstances(now);
    if(!instances.contains(g)&&instances.size()>=128)instances.erase(instances.begin());
    auto& i=instances[g];
    if(i.owner&&i.frame==frame)return i.output;
    const auto& world=g->world;Vec position{world.translate.x,world.translate.y,world.translate.z};
    auto up=Normalize(Canonical(settings,WorldToMeshDirection(world.rotate,{0,0,1})));float dt=float(now-i.time);
    auto shakeAxis=Normalize(Canonical(settings,WorldToMeshDirection(world.rotate,{1,0,0})));
    bool reset=!i.owner||dt>.25f||Length(position-i.position)>256;
    bool moved=reset||Length(position-i.position)>.25f||Length(up-i.motion.lastUp)>.015f||
        (settings.shakingBubbles>0&&Length(shakeAxis-i.shakeAxis)>.015f);
    float interval=SimulationInterval(pixels,moved);
    i.raySteps=RayQuality(pixels,i.raySteps);
    if(reset){i.motion=Motion{};i.motion.lastUp=up;i.velocity={};i.shakeAxis=shakeAxis;i.emissionTracker={};dt=0;
        auto seed=reinterpret_cast<std::uintptr_t>(g);i.motion.shakeRandom.seed(unsigned(seed^(seed>>32)));}
    if(reset||detail!=i.detail||interval==0||dt>=interval){
        Vec acceleration{};
        i.motion.shakeOmega=!reset?ShakeRotation(i.shakeAxis,i.motion.lastUp,shakeAxis,up,dt):Vec{};
        i.motion.shakeAngularSpeed=Length(i.motion.shakeOmega);
        // Only emission uses the carrying frame. World-space acceleration,
        // physical slosh and corrected rendering transforms stay untouched.
        Vec emissionX=shakeAxis,emissionUp=up,emissionPosition=position,emissionVertical{0,0,1};bool emissionFrame=true;
        if(held){
            emissionFrame=carry.valid;
            if(carry.valid){
                const auto& r=carry.rotation;
                emissionX=Normalize(Canonical(settings,WorldToMeshDirection(world.rotate,{r[0].x,r[0].y,r[0].z})));
                emissionUp=Normalize(Canonical(settings,WorldToMeshDirection(world.rotate,{r[2].x,r[2].y,r[2].z})));
                emissionPosition=CarryPosition(carry,position);
                emissionVertical=WorldToMeshDirection(r,{0,0,1});
            }
        }
        emissionFrame=emissionFrame&&std::isfinite(emissionX.x+emissionX.y+emissionX.z+
            emissionUp.x+emissionUp.y+emissionUp.z+emissionPosition.x+emissionPosition.y+emissionPosition.z);
        float metricScale=Length(Canonical(settings,WorldToMeshDirection(world.rotate,{0,0,1})))/std::max(std::abs(world.scale),.0001f);
        i.emissionTracker.Sample(i.motion,emissionX,emissionUp,emissionPosition,emissionVertical,
            metricScale,dt,held,emissionFrame&&!reset,held?carry.identity:nullptr,held?carry.originIdentity:nullptr);
        if(!reset&&dt>.00001f){Vec velocity=(position-i.position)*(1/dt);acceleration=Canonical(settings,WorldToMeshDirection(world.rotate,(velocity-i.velocity)*(1/dt)))*(1/std::max(std::abs(world.scale),.0001f));i.velocity=velocity;}
        i.previous=i.constants;
        // Account for the complete elapsed interval despite Simulate's 50ms clamp.
        int steps=std::max(1,int(std::ceil(dt/.05f)));
        for(int step=0;step<steps;step++){
            i.constants=Simulate(settings,i.motion,acceleration,up,dt/steps,detail,held);
        }
        if(reset||detail!=i.detail||interval==0)i.previous=i.constants;
        i.position=position;i.shakeAxis=shakeAxis;i.time=now;i.detail=detail;
    }
    i.interval=interval;i.output=interval>0?InterpolateSimulation(i.previous,i.constants,float(now-i.time)/interval):i.constants;
    i.output.wave.w=float(i.raySteps);i.frame=frame;i.owner=RE::NiPointer<RE::BSGeometry>(g);return i.output;
}
// Evaluate actual engine illumination, never the Effect material's PropertyColor.
V4 IncidentLight(const BottleDrawInputs& in,Constants& c){
    for(int i=0;i<4;i++){c.surfaceDirection[i]={};c.surfaceLight[i]={};}
    auto surface=[&](Vec direction,Vec rgb,bool isSun=false){
        float power=std::max({rgb.x,rgb.y,rgb.z});
        if(!std::isfinite(power)||power<=0||Length(direction)<1e-5f)return;
        direction=Normalize(direction);
        for(int i=0;i<4;i++)if(power>c.surfaceLight[i].w){
            for(int j=3;j>i;j--){c.surfaceLight[j]=c.surfaceLight[j-1];c.surfaceDirection[j]=c.surfaceDirection[j-1];}
            c.surfaceDirection[i]={direction.x,direction.y,direction.z,isSun?1.f:0.f};
            c.surfaceLight[i]={rgb.x,rgb.y,rgb.z,power};break;
        }
    };
    Vec sum{},ambient{},sunContribution{};
    auto linear=[](float x){return std::isfinite(x)?std::pow(std::max(x,0.f),2.2f):0.f;};
    if(in.ambient.valid){
        const auto& a=in.ambient;
        // Six-direction average approximates illumination of the liquid volume.
        for(int axis=0;axis<3;axis++)for(float sign:{-1.f,1.f}){
            const auto& r=a.axes[axis];
            sum=sum+Vec{linear(a.translate.x+sign*r.x*a.scale),linear(a.translate.y+sign*r.y*a.scale),linear(a.translate.z+sign*r.z*a.scale)}*(1.f/6);
        }
    }
    ambient=sum;
    if(in.sun.valid){
        // Volume-average sunlight, separate from surface/cubemap highlights.
        float scale=std::max(in.sun.dimmer,0.f)*.25f*std::max(in.sun.sunlightScale,0.f);
        sum=sum+Vec{linear(in.sun.diffuse.x),linear(in.sun.diffuse.y),linear(in.sun.diffuse.z)}*scale;
    }
    sunContribution=sum-ambient;
    c.sunVolume={sunContribution.x,sunContribution.y,sunContribution.z,0};
    if(in.sun.valid){auto d=in.sun.direction;surface({-d.x,-d.y,-d.z},sunContribution*4.f,true);}
    for(unsigned n=0;n<in.lightCount;n++){
        const auto& l=in.lights[n];float radius=l.radius;if(!std::isfinite(radius)||radius<=0)continue;
        auto d=l.toBottle;
        float distance=Length(d)/radius;
        float fade=std::pow(std::max(0.f,1-distance*distance),2.2f)*std::max(l.dimmer,0.f)*std::max(l.fade,0.f);
        auto rgb=Vec{linear(l.diffuse.x),linear(l.diffuse.y),linear(l.diffuse.z)}*fade;
        sum=sum+rgb;surface({-d.x,-d.y,-d.z},rgb);
    }
    auto safe=[](float v){return std::isfinite(v)?std::clamp(v,0.f,10000.f):0.f;};
    return {safe(sum.x),safe(sum.y),safe(sum.z),0};
}
void PrepareSunShadow(Renderer& renderer,Constants& c,const BottleDrawInputs& in){
    renderer.SetSunShadow(nullptr);c.sunInfo={};
    auto pos=in.geometry->world.translate;pos.x+=in.offset.x;pos.y+=in.offset.y;pos.z+=in.offset.z;c.worldOrigin={pos.x,pos.y,pos.z,in.geometry->world.scale};
    const auto& s=in.shadow;
    if(!s.view||s.cascades<1||s.cascades>4||!s.width||!s.height)return;
    Constants staged=c;float* slices=&staged.sunSlices.x;
    for(unsigned i=0;i<s.cascades;i++){
        for(unsigned j=0;j<16;j++){float v=s.worldToShadow[i][j];if(!std::isfinite(v))return;staged.sunMatrices[i][j]=v;}
        staged.sunBounds[i]=SunShadowBounds(s.viewport[i].data(),s.width,s.height);
        slices[i]=float(s.slice[i]);
    }
    staged.sunInfo={float(s.cascades),.0001f,float(s.width),float(s.height)};
    c=staged;renderer.SetSunShadow(s.view);
}
}
void ResetBottleFrames(){resetRequested=true;}
bool DrawBottle(ID3D11DeviceContext* ctx,Renderer& renderer,const BottleDrawInputs& in,const DrawCall& call){
    auto* geometry=in.geometry;const auto& drink=*in.drink;
    const double cpuStart=in.now;
    const auto& bottleWorld=geometry->world.translate;
    Vec bottle{bottleWorld.x,bottleWorld.y,bottleWorld.z};
    CleanInstances(cpuStart);
    if(!distanceLods.contains(geometry)&&distanceLods.size()>=256)distanceLods.erase(distanceLods.begin());
    auto& entry=distanceLods[geometry];entry.owner=RE::NiPointer<RE::BSGeometry>(geometry);
    entry.state.Update(in.localEyeValid?Length(bottle-in.eye):-1.f,cpuStart);
    auto lod=entry.state;bool detail=lod.detailWeight>0,effects=lod.effectWeight>0;

    // Shared static fallback: no per-instance simulation beyond 768 units.
    const auto& distantBase=drink.distant;
    float pixels=1e6f;
    if(effects&&!drink.empty&&in.projection.valid&&in.localEyeValid){
        const auto& bound=geometry->worldBound;const auto& p=in.projection;
        pixels=ProjectedDiameter(bound.fRadius,Length(Vec{bound.center.x,bound.center.y,bound.center.z}-in.eye),p.frustumHeight,p.frustumWidth,p.viewportWidth,p.viewportHeight);
    }
    // Empty glass uses the near profile, not the distant constants whose
    // refraction has already been disabled by ApplyDetailDistance.
    auto constants=effects?(drink.empty?drink.settings.base:Update(drink,geometry,in.frame,detail,pixels,in.held,in.carry)):distantBase;
    if(effects)ApplyDetailDistance(constants,detail);
    if(!effects||drink.empty)instances.erase(geometry); // Empty glass needs no motion/volume solver.

    constants.trace.z*=lod.detailWeight;
    constants.padding[1]=1-lod.detailWeight;
    constants.scatter.w*=lod.detailWeight;
    FadeBubbles(constants,pixels,lod.detailWeight,in.held);
    PrepareBubbleGroups(constants);
    constants.padding[0]=1-lod.effectWeight;
    // Fade slosh into the static upright fill before switching shaders.
    if(effects){float t=1-lod.effectWeight;
        constants.plane={std::lerp(constants.plane.x,distantBase.plane.x,t),std::lerp(constants.plane.y,distantBase.plane.y,t),std::lerp(constants.plane.z,distantBase.plane.z,t),std::lerp(constants.plane.w,distantBase.plane.w,t)};
        constants.wave.x*=lod.effectWeight;constants.foam*=lod.effectWeight;
    }
    constants.eye=IncidentLight(in,constants);
    // Read the live ordinary Effect material; external BGEM and embedded
    // NIF materials share this layout. Do not change flags or patch assets.
    constants.surfaceEnvironment={0,1,0,0};
    if(in.material.effectEnvironment){
        const auto& m=in.material;
        constants.surfaceEnvironment={std::isfinite(m.strength)?std::max(m.strength,0.f):0.f,m.lightingInfluence,m.environmentTechnique?1.f:0.f,0};
        // Converted BGSM glass carries original response metadata loaded
        // once with profiles. Authored BGEM strength remains live.
        if(constants.surfaceResponse.z>.5f)constants.surfaceEnvironment.x=constants.surfaceResponse.w;
        constants.surfaceAlpha={std::isfinite(m.baseAlpha)?std::clamp(m.baseAlpha,0.f,1.f):1.f,m.falloff?1.f:0.f,0,0};
        auto* falloff=&constants.surfaceFalloff.x;
        for(int i=0;i<4;i++)falloff[i]=std::isfinite(m.falloffData[i])?m.falloffData[i]:(i<2?float(i):1.f);
    }
    PrepareSunShadow(renderer,constants,in);
    if(in.firstPerson&&!in.localEyeValid){constants.sunInfo={};renderer.SetSunShadow(nullptr);}
    const auto& rotation=geometry->world.rotate;
    constants.meshToWorld[0]={rotation[0].x,rotation[1].x,rotation[2].x,0};
    constants.meshToWorld[1]={rotation[0].y,rotation[1].y,rotation[2].y,0};
    constants.meshToWorld[2]={rotation[0].z,rotation[1].z,rotation[2].z,0};
    // Do not infer the viewing ray from variant-dependent Effect b2.
    // Camera draws pass the true camera. Other draws derive the exact mesh-space
    // eye from their native ModelView in VSMain (padding[2]=0).
    float scale=geometry->world.scale;
    if(in.cameraMeshEye&&std::isfinite(scale)&&std::abs(scale)>.00001f){
        auto meshEye=WorldToMeshDirection(rotation,in.eye-bottle)*(1.f/scale);
        constants.meshToWorld[0].w=meshEye.x;constants.meshToWorld[1].w=meshEye.y;constants.meshToWorld[2].w=meshEye.z;
        constants.padding[2]=1;
    }else constants.padding[2]=0;
    renderer.SetExpectedIndices(drink.indices);

    Native().BeginFrame(in.frame);Native().activeGeometry=geometry;
    bool replaced=renderer.Draw(ctx,constants,call);
    Native().activeGeometry=nullptr;
    if(replaced)return true;
    static bool firstFailure=true;if(firstFailure){firstFailure=false;REX::WARN("Unsupported bottle capture; retaining vanilla rendering.");}
    return false;
}
}
