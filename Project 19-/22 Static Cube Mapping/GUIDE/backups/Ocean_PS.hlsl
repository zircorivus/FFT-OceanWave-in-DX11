#include "Ocean_Common.hlsli"


float4 PS(VSOutput input) : SV_TARGET
{
    // ----------------------------------------------------------------
    // View & Light vectors
    // ----------------------------------------------------------------
    float3 worldPos = input.worldPos;
    float3 viewDir = normalize(g_EyePosW - worldPos);
    // LightHelper convention: L.direction is surface-to-light? No, in LightHelper
    // we see lightVec = -L.direction, meaning stored direction is light-to-surface.
    // We follow that exactly:
    float3 lightVec = normalize(-g_DirLight[0].direction);
    float3 lightColor = g_DirLight[0].diffuse.rgb;
    float shadow = 1.0f; // Shadow placeholder
    // ----------------------------------------------------------------
    // Normal Reconstruction from slope textures
    // ----------------------------------------------------------------
    float2 slope0 = g_SlopeSRVs[0].SampleLevel(g_Sampler, input.uv * _Tile0, 0).rg * _LayerContribute0;
    float2 slope1 = g_SlopeSRVs[1].SampleLevel(g_Sampler, input.uv * _Tile1, 0).rg * _LayerContribute1;
    float2 slope2 = g_SlopeSRVs[2].SampleLevel(g_Sampler, input.uv * _Tile2, 0).rg * _LayerContribute2;
    float2 slope3 = g_SlopeSRVs[3].SampleLevel(g_Sampler, input.uv * _Tile3, 0).rg * _LayerContribute3;
    float2 slopeMixA = slope0 + slope1 + slope2 + slope3;
    float2 slopeMixB = slope2 + slope3;
    float invUVDepth = saturate(pow(length(input.uv / 500.0f * _VarMaskRange), _VarMaskPower));
    float normalVarMask = g_VariationMaskSRV.Sample(g_Sampler, input.uv / 1000.0f * _VarMaskTexScale).r 
                           * invUVDepth;
    normalVarMask = saturate(normalVarMask * 4.0f);
    float2 finalSlope = lerp(slopeMixA, slopeMixB, normalVarMask) * _NormalStrength;
    float3 macroNormal = float3(0.0f, 1.0f, 0.0f);
    float3 mesoNormal = normalize(float3(-finalSlope.x, 1.0f, -finalSlope.y));
    float normalAtten = pow(saturate(input.clipDepth), _DisplaceDepthAttenuation);
    mesoNormal = normalize(lerp(macroNormal, mesoNormal, normalAtten));
    float3 N = mesoNormal;
    // ----------------------------------------------------------------
    // Beckmann BRDF (matching Unity version)
    // ----------------------------------------------------------------
    float3 H = normalize(lightVec + viewDir);
    float NdotL = max(0.001f, dot(N, lightVec));
    float NdotH = max(0.001f, dot(N, H));
    float NdotV = max(0.001f, dot(N, viewDir));
    float roughness = _Roughness + input.foam * _FoamRoughness;
    // Fresnel with roughness correction (Unity approximation)
    float eta = 1.33f;
    float F0 = (eta - 1.0f) * (eta - 1.0f) / ((eta + 1.0f) * (eta + 1.0f));
    float fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f * exp(-2.69f * roughness));
    fresnel /= (1.0f + 22.7f * pow(roughness, 1.5f));
    fresnel = saturate(fresnel);
    // Geometry masking
    float viewMask = SmithMaskBeckmann(H, viewDir, roughness);
    float lightMask = SmithMaskBeckmann(H, lightVec, roughness);
    float geometryMask = 1.0f / (1.0f + viewMask + lightMask);
    // Specular term
    float3 specular = fresnel * geometryMask * Beckmann(NdotH, roughness) * lightColor;
    specular /= (4.0f * max(0.001f, dot(macroNormal, lightVec)));
    specular *= NdotL * shadow;
    // ----------------------------------------------------------------
    // Subsurface Scattering
    // ----------------------------------------------------------------
    // 使用物理波高做散射计算，不乘以 _HeightStrength
    // （_HeightStrength 已在 VS 中用于补偿粗网格的位移放大，散射应基于实际波高）
    float var_H = max(0.0f, input.waveHeight);
    float k1 = _WavePeakScatterStrength * var_H
               * pow(DotClamped(lightVec, -viewDir), 4.0f)
               * pow(abs(0.5f - 0.5f * dot(lightVec, N)), 3.0f);
    k1 = lerp(0.0f, k1, pow(saturate(input.clipDepth), _DisplaceDepthAttenuation));
    float k2 = _ScatterStrength * pow(DotClamped(viewDir, N), 2.0f);
    float k3 = _ScatterShadowStrength * NdotL;
    float k4 = _AmbientDensity;
    float3 ambientColor = g_DirLight[0].ambient.rgb;
    float3 scatter = (k1 * _ScatterPeakColor.rgb + k2 * _ScatterColor.rgb) * lightColor
                     * rcp(1.0f + lightMask) * saturate(shadow + _ShadowIntensity);
    scatter += k3 * _ScatterColor.rgb * lightColor * saturate(shadow + _ShadowIntensity);
    scatter += k4 * ambientColor;
    // ----------------------------------------------------------------
    // Environment Reflection
    // ----------------------------------------------------------------
    float3 reflectDir = reflect(-viewDir, N);
    float3 envReflect = g_Skybox.Sample(g_Sampler, reflectDir).rgb * _EnvironmentLightStrength;
    envReflect *= saturate(shadow + _ShadowIntensity + 0.5f);
    // ----------------------------------------------------------------
    // Foam (depth attenuated here, matching Unity logic)
    // ----------------------------------------------------------------
    float foam = lerp(0.0f, saturate(input.foam), pow(saturate(input.clipDepth), _FoamDepthAttenuation));
    foam *= saturate(shadow + _ShadowIntensity);
    // ----------------------------------------------------------------
    // Composition
    // ----------------------------------------------------------------
    float3 color = (1.0f - fresnel) * scatter + specular + fresnel * envReflect;
    color = lerp(color, _FoamColor.rgb * lightColor, saturate(foam));
    // Fog
    float fogFactor = ComputeExpFogFactor(input.viewDepth, _FogDensity);
    color = lerp(color, _FogColor.rgb, fogFactor);
  
    return float4(color, 1.0f);
}