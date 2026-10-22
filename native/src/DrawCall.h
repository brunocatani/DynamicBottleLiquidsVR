#pragma once
#include <d3d11.h>
namespace nuka {
// One intercepted indexed draw. Flat Fallout 4 issues bottle geometry through
// DrawIndexed; Fallout 4 VR issues the same geometry as DrawIndexedInstanced with
// two instances (one per eye). Replacement draws reissue the original call shape,
// so stereo instancing survives every optical pass.
struct DrawCall {
    using IndexedFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
    using InstancedFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
    UINT indexCount=0,instanceCount=1,startIndex=0,startInstance=0;INT baseVertex=0;
    IndexedFn indexed{};InstancedFn instanced{};
    static DrawCall Indexed(IndexedFn original,UINT count,UINT start,INT base){
        DrawCall d;d.indexed=original;d.indexCount=count;d.startIndex=start;d.baseVertex=base;return d;
    }
    static DrawCall Instanced(InstancedFn original,UINT count,UINT instances,UINT start,INT base,UINT firstInstance){
        DrawCall d;d.instanced=original;d.indexCount=count;d.instanceCount=instances;d.startIndex=start;d.baseVertex=base;d.startInstance=firstInstance;return d;
    }
    void Issue(ID3D11DeviceContext* c)const{
        if(instanced)instanced(c,indexCount,instanceCount,startIndex,baseVertex,startInstance);
        else indexed(c,indexCount,startIndex,baseVertex);
    }
};
}
