// ============================================================
// AI-GENERATED: 曲面细分管线的 VS（纯透传，匹配 Unity vert()）
// 原先的位移采样逻辑已移到 Ocean_DS.hlsl
// ============================================================
#include "Ocean_Common.hlsli"

HSInput VS(VSInput input)
{
    HSInput output; //使用曲面细分时VS仅作透传
    output.vertex = input.vertex;
    output.uv     = input.uv;
    output.normal = input.normal;
    return output;
}
