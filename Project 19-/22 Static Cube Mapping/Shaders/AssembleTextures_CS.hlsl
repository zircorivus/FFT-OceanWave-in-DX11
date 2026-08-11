#include "FFT_Ocean_CS_Common.hlsli"

RWTexture2D<float4> _SpectrumTex01 : register(u0);
RWTexture2D<float4> _SpectrumTex02 : register(u1);
RWTexture2D<float4> _VariationMask : register(u2);
RWTexture2D<float4> _DisplacementTex : register(u3);
RWTexture2D<float2> _SlopeTex : register(u4);
RWTexture2D<float> _BuoyancyData : register(u5);

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    float4 htimeDisplacement = Permute(_SpectrumTex01[uint2(id.xy)], id);
    float4 htimeSlope = Permute(_SpectrumTex02[uint2(id.xy)], id);
    float4 variation = Permute(_VariationMask[uint2(id.xy)], id);
    
    float2 DxDy = htimeDisplacement.rg;
    float2 DyDxz = htimeDisplacement.ba;
    float2 DyxDyz = htimeSlope.rg;
    float2 DxxDzz = htimeSlope.ba;
    
    float jacobian = (1.0f + _WaveSharpX * DxxDzz.x) * (1.0f + _WaveSharpY * DxxDzz.y) - _WaveSharpX * _WaveSharpY * DyDxz.y * DyDxz.y;
    jacobian = pow(saturate(jacobian), _FoamPower);
    
    float3 finalDisplace = float3(DxDy.x * _WaveSharpX, DyDxz.x, DxDy.y * _WaveSharpY);
    float2 finalSlope = DyxDyz.xy / (1 + abs(DxxDzz * float2(_WaveSharpX, _WaveSharpY)));
    
    float foam = _DisplacementTex[uint2(id.xy)].a; //读取上一帧的泡沫存量
    foam *= exp(-_FoamDecayRate); //自然衰减
    foam = saturate(foam); //钳制到零到一
    
    float biasedJacobian = max(0.0f, -(jacobian - _FoamBias));
    
    if (biasedJacobian > 0)
        foam += _FoamAdd * biasedJacobian;
    
    _DisplacementTex[uint2(id.xy)] = float4(finalDisplace, foam); //将泡沫数据一并写入顶点偏移贴图
    _SlopeTex[uint2(id.xy)] = float2(finalSlope); //将法线斜率贴图
    
    if (_SpectrumIndex == 0)
    {
        _BuoyancyData[id.xy] = finalDisplace.y; // 第 0 层的垂直位移存给浮力系统 将位移传给C#物体的物体浮力计算
        _VariationMask[uint2(id.xy)] = variation; // 泡沫遮罩保存给渲染 Shader
    }
}