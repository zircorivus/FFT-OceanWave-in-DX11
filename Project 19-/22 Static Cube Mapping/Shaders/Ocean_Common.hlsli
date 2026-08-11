#include "LightHelper.hlsli"

cbuffer CBOceanUpdatePerFrame : register(b0)
{
    matrix g_World;
    matrix g_ViewProj;
    float3 g_EyePosW;
    float g_Pad;  // 必须是标量不能用 float[1]——HLSL cbuffer 数组元素按 16B 对齐
};

cbuffer CBOceanParams : register(b1)
{
    float _Tile0;
    float _Tile1;
    float _Tile2;
    float _Tile3;
    float _LayerContribute0;
    float _LayerContribute1;
    float _LayerContribute2;
    float _LayerContribute3;
    float _HeightStrength;
    float _NormalStrength;
    float _DisplaceDepthAttenuation;
    float _FoamDepthAttenuation;
    float _Roughness;
    float _FoamRoughness;
    float _AmbientDensity;
    float _EnvironmentLightStrength;
    float _ScatterStrength;
    float _ScatterShadowStrength;
    float _WavePeakScatterStrength;
    float _EdgeFoamPower;
    float _ShadowIntensity;
    float _FogDensity;
    float _FogPower;
    float _VarMaskRange;
    float _VarMaskPower;
    float _VarMaskTexScale;
    float _TessEdgeLength;
    float _TessNear;
    float _TessFar;
    float _Pad0;
    float _Pad1;
    float _Pad2;
    float4 _ScatterColor;
    float4 _ScatterPeakColor;
    float4 _FoamColor;
    float4 _FogColor;
}

cbuffer CBOceanChangeRarely : register(b2)
{
    DirectionalLight g_DirLight[5];
    PointLight g_PointLight[5];
    SpotLight g_SpotLight[5];
}

Texture2D g_DisplacementSRVs[4] : register(t0);
Texture2D g_SlopeSRVs[4] : register(t4);
Texture2D g_VariationMaskSRV : register(t8);
TextureCube g_Skybox : register(t9);
Texture2D g_SceneDepthMap : register(t10);  // AI-GENERATED: 场景深度纹理（水面渲染前拷贝），用于边缘泡沫深度比较

SamplerState g_Sampler : register(s0);

// ============================================================
// AI-GENERATED: 曲面细分管线结构体（HSInput / HSCPOutput / HSPatchOutput）
// 以及 SampleDisplacement() 共享函数
// ============================================================

// ---- 顶点输入（IA → VS）----
struct VSInput
{
    float4 vertex : POSITION;
    float2 uv     : TEXCOORD0;
    float3 normal : NORMAL;
};

// ---- VS 输出 = HS 输入（object-space 透传）----
struct HSInput
{
    float4 vertex : POSITION;
    float2 uv     : TEXCOORD0;
    float3 normal : NORMAL;
};

// ---- HS 控制点输出（→ DS）----
struct HSCPOutput
{
    float4 vertex : POSITION;
    float2 uv     : TEXCOORD0;
    float3 normal : NORMAL;
};

// ---- HS 面片常量 ----
struct HSPatchOutput
{
    float edgeTess[3]   : SV_TessFactor;
    float insideTess    : SV_InsideTessFactor;
};

// ---- DS 输出（通往 GS 或直接到 PS）----
struct VSOutput
{
    float4 clipPos   : SV_POSITION;
    float3 worldPos  : TEXCOORD0;
    float2 uv        : TEXCOORD1;
    float  clipDepth : TEXCOORD2;
    float  viewDepth : TEXCOORD3;
    float2 screenUV  : TEXCOORD4;
    float  foam      : TEXCOORD5;
    float  waveHeight: TEXCOORD6;
    float  ndcDepth  : TEXCOORD7;  // AI-GENERATED: NDC 深度 (= clipPos.z / clipPos.w)，用于边缘泡沫深度比较
};

// ============================================================
// AI-GENERATED: GSOutput 结构体 — GS 输出 = PS 输入超集
// 包含 VSOutput 全部字段 + 重心坐标。TEXCOORD7=ndcDepth, TEXCOORD8=barycentric
// PS 取 VSOutput 子集（TEXCOORD0~7）即可，barycentric 被光栅化器丢弃
// ============================================================
struct GSOutput
{
    float4 clipPos     : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float2 uv          : TEXCOORD1;
    float  clipDepth   : TEXCOORD2;
    float  viewDepth   : TEXCOORD3;
    float2 screenUV    : TEXCOORD4;
    float  foam        : TEXCOORD5;
    float  waveHeight  : TEXCOORD6;
    float  ndcDepth    : TEXCOORD7;  // AI-GENERATED: NDC 深度（从 VSOutput 透传）
    float2 barycentric : TEXCOORD8;  // AI-GENERATED: 重心坐标，匹配 Unity g2f
};

// ---- 共享的位移采样函数（DS 和 VS 共用）----
void SampleDisplacement(float3 worldPosBase,
    out float3 displacement, out float foamAccum, out float waveHeight0)
{
    float tiles[4]          = { _Tile0, _Tile1, _Tile2, _Tile3 };
    float layerContributes[4] = { _LayerContribute0, _LayerContribute1, _LayerContribute2, _LayerContribute3 };

    displacement = float3(0, 0, 0);
    foamAccum    = 0;
    waveHeight0  = 0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 uv_i = worldPosBase.xz * tiles[i];
        float4 disp = g_DisplacementSRVs[i].SampleLevel(g_Sampler, uv_i, 0);
        displacement += disp.rgb * layerContributes[i];
        foamAccum    += disp.a   * layerContributes[i];
        if (i == 0)
            waveHeight0 = disp.y * layerContributes[i];
    }
    displacement *= _HeightStrength;
}
// ============================================================================
// Shared Utilities
// ============================================================================
float DotClamped(float3 a, float3 b)
{
    return saturate(dot(a, b));
}
float Beckmann(float nDotH, float roughness)
{
    float r2 = roughness * roughness;
    float expArg = (nDotH * nDotH - 1.0f) / (r2 * nDotH * nDotH);
    return exp(expArg) / (3.14159265f * r2 * nDotH * nDotH * nDotH * nDotH);
}
float SmithMaskBeckmann(float3 halfDir, float3 otherDir, float roughness)
{
    float hDotO = max(0.001f, dot(halfDir, otherDir));
    float a = hDotO / (roughness * sqrt(1.0f - hDotO * hDotO));
    float a2 = a * a;
    return a < 1.6f ? (1.0f - 1.259f * a + 0.396f * a2) / (3.535f * a + 2.181f * a2) : 0.0f;
}
float ComputeExpFogFactor(float depth, float density)
{
    return saturate(pow(1.0f - exp(-depth * density), _FogPower));
}