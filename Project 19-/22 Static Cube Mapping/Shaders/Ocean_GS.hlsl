// ============================================================
// AI-GENERATED: 曲面细分管线的 Geometry Shader
// 匹配 Unity FFTOcean_Shader.shader 的 geo() 函数
// 功能：1. 透传 DS 输出的所有插值数据
//        2. 为每个三角形顶点生成重心坐标（用于线框调试/未来扩展）
// ============================================================
#include "Ocean_Common.hlsli"

[maxvertexcount(3)] // 最多输出 3 个顶点（三角形不变）
void GS(
    triangle VSOutput input[3],      // DS 输出的三角形（3 个顶点，含 SV_POSITION + 全部 TEXCOORD）
    inout TriangleStream<GSOutput> output  // 输出流，每次 Append 一个顶点
)
{
    // AI-GENERATED: 重心坐标 — 三个顶点分别赋 (1,0) (0,1) (0,0)
    // 经过光栅化器插值后，片元着色器可拿到 "该像素离三角形三个顶点有多远"
    // 与 Unity g2f.barycentricCoordinates 完全匹配
    float2 bCentrics[3] = { float2(1, 0), float2(0, 1), float2(0, 0) };

    [unroll]
    for (uint i = 0; i < 3; i++)
    {
        GSOutput o;
        // AI-GENERATED: 透传 DS 输出的全部字段
        o.clipPos     = input[i].clipPos;
        o.worldPos    = input[i].worldPos;
        o.uv          = input[i].uv;
        o.clipDepth   = input[i].clipDepth;
        o.viewDepth   = input[i].viewDepth;
        o.screenUV    = input[i].screenUV;
        o.foam        = input[i].foam;
        o.waveHeight  = input[i].waveHeight;
        o.ndcDepth    = input[i].ndcDepth;     // AI-GENERATED: NDC 深度透传
        o.barycentric = bCentrics[i];              // 附加重心坐标
        output.Append(o);                           // 发射这个顶点
        // GS 自动 RestartStrip() — 三角形完成，进入光栅化
    }
}
