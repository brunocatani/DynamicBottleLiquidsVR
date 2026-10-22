#pragma once
#include "Profiles.h"
#include "CarryFrame.h"
#include "DrawCall.h"
#include <array>
namespace nuka {
class Renderer;
// Runtime-neutral bottle draw. Each runtime's glue reads its own engine layouts
// and fills these plain inputs; this file owns simulation, LOD, lighting and
// material constants so flat Fallout 4 and Fallout 4 VR produce the same result.
struct ProjectionInputs {
    bool valid=false;        // perspective camera with a known frustum
    float frustumHeight=0;   // top-bottom of the unit-distance frustum
    float frustumWidth=0;    // right-left of the unit-distance frustum
    float viewportWidth=0,viewportHeight=0; // one view, in pixels
};
struct AmbientInputs {bool valid=false;Vec translate{};Vec axes[3]{};float scale=0;};
struct SunInputs {bool valid=false;Vec direction{};Vec diffuse{};float dimmer=0,sunlightScale=1;};
struct LocalLightInput {Vec toBottle{};float radius=0,dimmer=0,fade=0;Vec diffuse{};};
struct SunShadowInputs {
    // Nullptr view or zero cascades leaves sun shadowing disabled.
    ID3D11ShaderResourceView* view=nullptr;unsigned cascades=0,width=0,height=0;
    std::array<std::array<float,16>,4> worldToShadow{};
    std::array<std::array<int,4>,4> viewport{};
    std::array<unsigned,4> slice{};
};
struct MaterialInputs {
    bool effectEnvironment=false; // ordinary Effect material with the env-map flag
    float strength=0,lightingInfluence=0,baseAlpha=1;bool falloff=false;float falloffData[4]{0,1,1,1};
    bool environmentTechnique=false; // technique bit 10
};
struct BottleDrawInputs {
    RE::BSGeometry* geometry=nullptr;const DrinkProfile* drink=nullptr;
    unsigned frame=0;double now=0;
    Vec eye{};bool havePosition=false,localEyeValid=false,firstPerson=false,held=false;
    Vec offset{};CarryFrame carry{};
    // Flat world draws pass the true camera through MeshToWorld.w. VR draws derive
    // each eye's mesh-space position from its native ModelView in the producer VS.
    bool cameraMeshEye=false;
    ProjectionInputs projection{};AmbientInputs ambient{};SunInputs sun{};
    const LocalLightInput* lights=nullptr;unsigned lightCount=0;
    MaterialInputs material{};SunShadowInputs shadow{};
};
void ResetBottleFrames();
// Returns true when the replacement path consumed the draw.
bool DrawBottle(ID3D11DeviceContext*,Renderer&,const BottleDrawInputs&,const DrawCall&);
}
