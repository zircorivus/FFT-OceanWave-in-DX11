// ============================================================
// AI-GENERATED: 曲面细分 Domain Shader
// 匹配 Unity FFTOcean_Shader.shader 的 domain() + vp()
// 重心插值 → 世界空间 → 位移采样 → 深度衰减 → 裁剪空间
// ============================================================
#include "Ocean_Common.hlsli"

#define NUM_CONTROL_POINTS 3

[domain("tri")] //处理三角形
VSOutput DS(
    HSPatchOutput patchConstant, //从HS里面输出出来的细分因子
    float3 domain : SV_DomainLocation, //细分器生成的重心坐标（这个数据从哪里来的）
    const OutputPatch<HSCPOutput, NUM_CONTROL_POINTS> patch) //3个控制点（HS输出的顶点数据）
{
    VSOutput output = (VSOutput) 0;

    // 1. 重心坐标插值得到新的物体空间位置
    float3 objPos = patch[0].vertex.xyz * domain.x //domain是到某一条边的权重
                  + patch[1].vertex.xyz * domain.y
                  + patch[2].vertex.xyz * domain.z;
    float2 uv    = patch[0].uv * domain.x
                  + patch[1].uv * domain.y
                  + patch[2].uv * domain.z;

    // 2. 新老顶点一起变换到世界空间
    float3 worldPosBase = mul(float4(objPos, 1.0f), g_World).xyz;

    // 3. 采样位移纹理（共享函数，匹配 Unity vp()）
    float3 displacement;
    float  foamAccum, waveHeight0;
    SampleDisplacement(worldPosBase, displacement, foamAccum, waveHeight0);

    // 4. 深度衰减（匹配 Unity: clipDepth = 1 - Linear01Depth(z/w)）太远的三角形就减少采样频率？
    float4 clipPosBase = mul(float4(worldPosBase, 1.0f), g_ViewProj); //做一次MVP变换
    // clipPosBase.w = view-space 线性深度（≈ 摄像机到顶点的实际距离，单位米）
    float  linearDepth  = saturate(clipPosBase.w / 1000.0f); // /farPlane → [0,1]
    float  clipDepth    = 1.0f - linearDepth;                // Unity: 1 - Linear01Depth
    float  attenuation  = pow(saturate(clipDepth), _DisplaceDepthAttenuation);
    displacement = lerp(float3(0, 0, 0), displacement, attenuation);

    // 5. 应用位移 + 变换到裁剪空间
    float3 worldPos = worldPosBase + displacement;
    float4 clipPos  = mul(float4(worldPos, 1.0f), g_ViewProj); //对真正的最终输出做MVP裁剪空间变换

    // 6. 输出（匹配旧 VSOutput → PS）
    output.clipPos    = clipPos;
    output.worldPos   = worldPos;
    output.uv         = worldPos.xz;
    output.waveHeight = waveHeight0;
    output.foam       = foamAccum;
    output.clipDepth  = clipDepth;
    output.viewDepth  = distance(worldPos, g_EyePosW);
    output.screenUV   = clipPos.xy / clipPos.w * 0.5f + 0.5f;
    output.screenUV.y = 1.0f - output.screenUV.y;
    output.ndcDepth   = clipPos.z / clipPos.w;  // AI-GENERATED: NDC 深度，用于边缘泡沫的深度比较

    return output;
}
