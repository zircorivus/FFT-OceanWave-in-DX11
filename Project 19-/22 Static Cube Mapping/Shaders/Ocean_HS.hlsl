// ============================================================
// AI-GENERATED: 曲面细分 Hull Shader
// 匹配 Unity FFTOcean_Shader.shader 的 hull() + PatchFunction()
// 距离基细分因子：近处细、远处粗
// ============================================================
#include "Ocean_Common.hlsli"

#define NUM_CONTROL_POINTS 3

//通过空间上的距离和长度决定细分段数
float TessellationHeuristic(float3 cp0, float3 cp1)
{
    float3 wp0 = mul(float4(cp0, 1.0f), g_World).xyz; // 控制点 cp0 → 世界空间
    float3 wp1 = mul(float4(cp1, 1.0f), g_World).xyz; // 控制点 cp1 → 世界空间
    float edgeLength = distance(wp0, wp1); // 这条边的世界空间长度（米）
    float3 edgeCenter = (wp0 + wp1) * 0.5f; // 边的中点
    float viewDist = distance(edgeCenter, g_EyePosW); // 中点到摄像机的距离

    // Unity: edgeLength * _ScreenParams.y / (_TessEdgeLength * pow(viewDist * 0.5, 1.2))
    // 没有 _ScreenParams，用常量 1080 替代（屏幕高度），调 _TessEdgeLength 即可
    float screenHeight = 1080.0f;
    // 边长和屏高越大，细分越多； 距离越远，分母越大 → 因子越小 → 细分越少
    float factor = edgeLength * screenHeight / (_TessEdgeLength * pow(max(viewDist * 0.5f, 0.01f), 1.2f));

    // 限制细分范围 (1~64)
    return clamp(factor, 1.0f, 64.0f);
}

//面片常量函数
HSPatchOutput CalcHSPatchConstants(
    InputPatch<HSInput, NUM_CONTROL_POINTS> patch,
    uint patchID : SV_PrimitiveID)
{
    HSPatchOutput output;

    // 1. 提取三个控制点的 object-space 位置
    float3 p0 = patch[0].vertex.xyz;
    float3 p1 = patch[1].vertex.xyz;
    float3 p2 = patch[2].vertex.xyz;

    // 2. 转换到世界空间，计算三角形中心
    float3 wp0 = mul(float4(p0, 1.0f), g_World).xyz;
    float3 wp1 = mul(float4(p1, 1.0f), g_World).xyz;
    float3 wp2 = mul(float4(p2, 1.0f), g_World).xyz;
    float3 center = (wp0 + wp1 + wp2) / 3.0f;
    // 远近裁剪：太远的三角形不做细分
    float dist = distance(center, g_EyePosW);

    if (dist > _TessFar) // 超过 _TessFar（默认 500m）的三角形不细分
    {
        output.edgeTess[0] = output.edgeTess[1] = output.edgeTess[2] = 1;
        output.insideTess = 1;
    }
    else
    {
        // 决定三条边各自的细分因子
        output.edgeTess[0] = TessellationHeuristic(p1, p2); //第一条边
        output.edgeTess[1] = TessellationHeuristic(p2, p0); //第二条边
        output.edgeTess[2] = TessellationHeuristic(p0, p1); //第三条边
        //全部加起来除以三？
        output.insideTess = (output.edgeTess[0] + output.edgeTess[1] + output.edgeTess[2]) / 3.0f;
    }

    return output;
}

[domain("tri")] //输入图元类型是三角形
[partitioning("fractional_odd")] //细分因子的分割方式：分数奇数
[outputtopology("triangle_cw")] //输出三角形按顺时针顺序
[outputcontrolpoints(NUM_CONTROL_POINTS)] //输出三个控制点（三角形三个顶点）
[patchconstantfunc("CalcHSPatchConstants")] //指定面片常量函数：对整个三角形做一次决策，输出"每条边切几段"（细分因子
HSCPOutput HS(
    InputPatch<HSInput, NUM_CONTROL_POINTS> patch,
    uint i : SV_OutputControlPointID,
    uint patchID : SV_PrimitiveID)
{
    HSCPOutput output; //HS本身也是纯纯的透传，决定细分密度的工作交给了上面的函数完成了
    output.vertex = patch[i].vertex;
    output.uv     = patch[i].uv;
    output.normal = patch[i].normal;
    return output;
}
