#include "FFT_Ocean_CS_Common.hlsli"

RWTexture2D<float4> _InitializeSpectrumTex : register(u0);
RWTexture2D<float4> _SpectrumTex01 : register(u1);
RWTexture2D<float4> _SpectrumTex02 : register(u2);
RWTexture2D<float4> _VariationMask : register(u3);

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    float LengthScale[] = { _LengthScale0, _LengthScale1, _LengthScale2, _LengthScale3 };
    
    float4 Initial = _InitializeSpectrumTex[uint2(id.xy)];
    float2 h0 = Initial.xy;
    float2 h0conju = Initial.zw;
    
    float halfResolution = _Resolution / 2.0f;
    float2 k = (id.xy - halfResolution) * 2.0f * PI / LengthScale[_InitialSpectrumIndex];
    float kLength = length(k);
    float kLengthRcp = rcp(kLength);
    
    if (kLength < 0.0001f)
    {
        kLengthRcp = 1.0f;
    }

    float w0 = 2.0f * PI / _RepeatTime;
    float dispersion = floor(sqrt(_Gravity * kLength) / w0) * w0 * _FrameTime; //在 _FrameTime 这段时间内，该频率分量累积的相位变化量（单位弧度）
    
    float2 exponent = EulerFormula(dispersion);
    
    float2 htime = ComplexMul(h0, exponent) + ComplexMul(h0conju, float2(exponent.x, -exponent.y));
    float2 ihtime = float2(-htime.y, htime.x);
    
    float2 displacementX = ihtime * k.x * kLengthRcp;
    float2 displacementY = htime;
    float2 displacementZ = ihtime * k.y * kLengthRcp;
    
    float2 displacementX_dx = -htime * k.x * k.x * kLengthRcp;
    float2 displacementY_dx = ihtime * k.x;
    
    float2 displacementY_dz = k.y * ihtime;
    float2 displacementZ_dz = -htime * k.y * k.y * kLengthRcp;
    
    float2 displacementZ_dx = -htime * k.x * k.y * kLengthRcp;
    
    float2 htimeDisplacementX = float2(displacementX.x - displacementZ.y, displacementX.y + displacementZ.x);
    float2 htimeDisplacementZ = float2(displacementY.x - displacementZ_dx.y, displacementY.y + displacementZ_dx.x);
    
    float2 htimeSlopeX = float2(displacementY_dx.x - displacementY_dz.y, displacementY_dx.y + displacementY_dz.x);
    float2 htimeSlopeZ = float2(displacementX_dx.x - displacementZ_dz.y, displacementX_dx.y + displacementZ_dz.x);
    
    _SpectrumTex01[uint2(id.xy)] = float4(htimeDisplacementX, htimeDisplacementZ);
    _SpectrumTex02[uint2(id.xy)] = float4(htimeSlopeX, htimeSlopeZ);
    
    if (_InitialSpectrumIndex == 0)
    {
        _VariationMask[uint2(id.xy)] = float4(_InitializeSpectrumTex[id.xy].x, 0.0f, 0.0f, 0.0f);
    }
}