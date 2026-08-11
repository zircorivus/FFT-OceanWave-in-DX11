#define FFT_OCEAN_IFFT_ENABLED
#include "FFT_Ocean_CS_Common.hlsli"

RWTexture2D<float4> _SpectrumTex : register(u0);
RWTexture2D<float4> _VariationMask : register(u1);

#define SIZE 1024
#define LOG_SIZE 10
groupshared float4 fftGroupBuffer[2][SIZE];

void ButterFlyValues(uint step, uint index, out uint2 index2, out float2 twiddle)
{
    const float twoPi = 6.28318530718;
    uint b = SIZE >> (step + 1);
    uint w = b * (index / b);
    uint i = (w + index) % SIZE;
    index2 = uint2(i, i + b);
    
    sincos(-twoPi / SIZE * w, twiddle.y, twiddle.x);
    twiddle.y = -twiddle.y;
}

float4 IFFT(uint threadIndex, float4 input)
{
    fftGroupBuffer[0][threadIndex] = input;
    GroupMemoryBarrierWithGroupSync();
    bool flag = false;
    
    [unroll]
    for (uint step = 0; step < LOG_SIZE; ++step)
    {
        uint2 outputIndices;
        float2 twiddle;
        ButterFlyValues(step, threadIndex, outputIndices, twiddle);
        
        float4 v = fftGroupBuffer[flag][outputIndices.y];
        
        fftGroupBuffer[!flag][threadIndex] = fftGroupBuffer[flag][outputIndices.x] + float4(ComplexMul(twiddle, v.xy), ComplexMul(twiddle, v.zw));
        
        flag = !flag;
        
        GroupMemoryBarrierWithGroupSync();
    }
    return fftGroupBuffer[flag][threadIndex];
}

[numthreads(SIZE, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    _SpectrumTex[uint2(id.yx)] = IFFT(id.x, _SpectrumTex[uint2(id.yx)]);
    if(_SpectrumIndex == 7)
        _VariationMask[uint2(id.yx)] = IFFT(id.x, _VariationMask[uint2(id.yx)]);
}