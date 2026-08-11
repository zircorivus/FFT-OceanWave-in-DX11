#include "FFT_Ocean_CS_Common.hlsli"

RWTexture2D<float4> _InitializeSpectrumTex : register(u0);

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    //四套不同的初始化频谱就要靠C++端手动配置了
    float2 h0 = _InitializeSpectrumTex[uint2(id.xy)].rg;
    float2 h0conju = _InitializeSpectrumTex[uint2((_Resolution - id.x) % _Resolution, (_Resolution - id.y) % _Resolution)];
    
    _InitializeSpectrumTex[uint2(id.xy)] = float4(h0, h0conju.x, -h0conju.y);
}