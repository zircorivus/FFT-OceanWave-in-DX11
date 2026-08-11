# FFT Ocean 曲面细分渲染管线 — 逐行学习指南

> 适用读者：计算机图形学新手，对 VS/PS 管线有基础了解，曲面细分零基础。
> 配合项目文件对照阅读：打开 Shader 文件，按本文顺序一行行看。

---

## 第 1 章：D3D11 曲面细分管线概览

### 1.1 完整的 GPU 渲染管线

```
  IA (输入装配)        →  把顶点缓冲区的数据读进来，组装成三角形面片
   ↓
  VS (顶点着色器)      →  传统管线的第一步，处理每个顶点。但在曲面细分管线下角色变了！
   ↓
  HS (外壳着色器)      →  【细分专用】决定"每个三角形该被切成多少个小三角形"
   ↓
  Tessellator (细分器) →  【硬件固定功能】根据 HS 的输出，自动生成新的顶点
   ↓
  DS (域着色器)        →  【细分专用】把细分器生成的每个新顶点变换到正确的世界位置
   ↓
  GS (几何着色器)      →  【可选】处理完整三角形，可增删顶点。这里用来生成重心坐标
   ↓
  RS (光栅化器)        →  把三角形转成像素，插值顶点属性
   ↓
  PS (像素着色器)      →  计算每个像素的最终颜色
   ↓
  OM (输出合并)        →  深度测试、混合，写入渲染目标
```

### 1.2 对比：传统管线 vs 曲面细分管线

| | 传统 VS/PS | 曲面细分 VS/HS/DS |
|---|---|---|
| VS 职责 | 变换顶点 + 计算光照所需数据 | **仅透传数据**（所有重活移到 DS 中做） |
| 三角形密度 | 固定的（模型顶点数） | **动态的**（近处密远处疏，根据距离实时调整） |
| 内存占用 | 大模型需要很多 VRAM | **小网格 + 动态细分** = 内存省、细节多 |
| 适用场景 | 普通物体渲染 | 地形、水面等大面积表面 |

### 1.3 为什么水面要用曲面细分？

水面是一块巨大的平面（256m × 256m），但只有 64×64 = 4096 个顶点。如果不细分，每个三角形边长 4m，远看勉强够，近看就是明显的折线。

曲面细分让引擎在**摄像机近处自动产生密集顶点**（实现波浪细节）、**远处保持稀疏**（节省性能），实现 LOD（细节层次）的自动管理。

---

## 第 2 章：数据流追踪 — 结构体链

### 2.1 结构体在各阶段的流转

```
  C++ IA 输入        VSInput        { vertex:P, uv:T, normal:N }
       ↓   VS
  VS→HS 透传         HSInput        { vertex:P, uv:T, normal:N }     ← 与 VSInput 完全相同
       ↓   HS（每个控制点透传）
  HS→DS              HSCPOutput     { vertex:P, uv:T, normal:N }     ← 与 HSInput 完全相同
       ↓   Tessellator（生成新顶点 + 重心坐标 domain.xyz）
       ↓   DS（位移采样 + 变换）
  DS→GS              VSOutput       { clipPos:SV_POSITION, worldPos, uv, clipDepth, viewDepth, screenUV, foam, waveHeight, ndcDepth }
       ↓   GS（透传 + 重心坐标）
  GS→PS              GSOutput       { VSOutput 全部字段 + barycentric }
       ↓   RS（光栅化插值）→ PS 拿到的 VSOutput 子集
  PS                 输入 VSOutput  { SV_POSITION, TEXCOORD0~7 }
```

### 2.2 为什么需要多个结构体？

每个着色器阶段有**自己的输入和输出类型**，D3D11 要求类型严格匹配：

- **VSOutput**（DS 输出）≠ **HSInput**（VS 输出）：DS 需要输出裁剪空间坐标 `SV_POSITION`，而 VS 输出的还是 object-space 顶点
- **GSOutput**（GS 输出）比 **VSOutput** 多一个 `barycentric` 字段：GS 为每个三角形顶点附加了重心坐标

这些结构体定义在 `Shaders/Ocean_Common.hlsli` 中，所有 shader 文件通过 `#include` 共享。

---

## 第 3 章：Ocean_VS.hlsl — 最简单的着色器

```hlsl
#include "Ocean_Common.hlsli"

HSInput VS(VSInput input)
{
    HSInput output;
    output.vertex = input.vertex;   // object-space 位置 → 原样传递
    output.uv     = input.uv;       // UV → 原样传递
    output.normal = input.normal;   // 法线 → 原样传递
    return output;
}
```

### 逐行解释

| 行 | 代码 | 解释 |
|----|------|------|
| 1 | `#include "Ocean_Common.hlsli"` | 引用共用结构体和 cbuffer 声明 |
| 3 | `HSInput VS(VSInput input)` | VS 函数：输入 VSInput（来自 IA），输出 HSInput（给 HS） |
| 6 | `output.vertex = input.vertex;` | **透传**：顶点保持 object-space 坐标。不乘 World/View/Proj 矩阵！ |
| 7-8 | `output.uv / normal` | 同样透传，不处理 |

### 关键理解

**为什么曲面细分下的 VS 什么都不做？**

在传统管线中，VS 负责把顶点从 object-space 变换到 clip-space（乘 MVP 矩阵），并计算光照数据。但曲面细分管线下：

1. VS 只是管线入口，真正的顶点位置要在**细分器生成新顶点之后**（DS 中）才能确定
2. 如果 VS 提前变换了，细分器生成的顶点就不知道自己在世界空间的真实位置了
3. 所以 VS 的职责从"变换"变成了"透传"——把原始数据完整交给后面的 HS/DS

这个模式在 Unity 的 `vert()` 函数中完全相同：`d.vertex = h.vertex; d.uv = h.uv; d.normal = h.normal;`

---

## 第 4 章：Ocean_HS.hlsl — 决定细分成多少块

### 4.1 HS 的结构

HS 由**两部分**组成：

1. **`CalcHSPatchConstants()`** — 面片常量函数：对整个三角形做一次决策，输出"每条边切几段"（细分因子）
2. **`HS()`** — 控制点函数：对每个控制点顶点调用一次，透传数据

### 4.2 声明式属性（第 61-65 行）

```hlsl
[domain("tri")]                                    // 输入图元类型是三角形（另外可选 "quad" 四边形、"isoline" 等值线）
[partitioning("fractional_odd")]                   // 细分因子的分割方式：fractional_odd = 分数奇数
[outputtopology("triangle_cw")]                    // 输出三角形按顺时针绕序
[outputcontrolpoints(NUM_CONTROL_POINTS)]           // 输出 3 个控制点（三角形 3 个顶点）
[patchconstantfunc("CalcHSPatchConstants")]         // 指定面片常量函数名
HSCPOutput HS(...)
```

**为什么 `fractional_odd`？** 细分因子在小数部分的过渡是平滑的（相邻帧细分级别渐变），不会出现整数分割的"跳变"。`odd` 保证输出结构更好。

对比 Unity 用的 `integer` 分割：整数分割有跳变，但性能开销略低。

### 4.3 细分启发式函数（第 10-25 行）

```hlsl
float TessellationHeuristic(float3 cp0, float3 cp1)
{
    float3 wp0 = mul(float4(cp0, 1.0f), g_World).xyz;  // 控制点 cp0 → 世界空间
    float3 wp1 = mul(float4(cp1, 1.0f), g_World).xyz;  // 控制点 cp1 → 世界空间
    float edgeLength = distance(wp0, wp1);              // 这条边的世界空间长度（米）
    float3 edgeCenter = (wp0 + wp1) * 0.5f;             // 边的中点
    float viewDist = distance(edgeCenter, g_EyePosW);   // 中点到摄像机的距离

    float screenHeight = 1080.0f;  // 屏幕像素高度（替代 Unity 的 _ScreenParams.y）
    float factor = edgeLength * screenHeight
                 / (_TessEdgeLength * pow(max(viewDist * 0.5f, 0.01f), 1.2f));
    //         ↑                             ↑
    //   边长和屏高越大，细分越多        距离越远，分母越大 → 因子越小 → 细分越少

    return clamp(factor, 1.0f, 64.0f);  // 限制在 1~64 之间
}
```

**公式解析**（匹配 Unity 同名函数）：

$$TessFactor = \frac{edgeLength \times screenHeight}{TessEdgeLength \times (viewDist \times 0.5)^{1.2}}$$

- **edgeLength** 越大 → 因子越大 → 更多细分（长边需要更多段才能平滑）
- **screenHeight**：屏幕越高 → 同一物体占据更多像素 → 需要更多细节
- **_TessEdgeLength**（默认 16）：分母越大 → 因子越小 → 整体细分抑制
- **viewDist**：距离越远 → 分母的 `pow(viewDist, 1.2)` 迅速增长 → 因子急剧下降 → 远处不细分（LOD 效果）
- **clamp(1, 64)**：最少 1 段（不细分），最多 64 段

### 4.4 面片常量函数（第 27-59 行）

```hlsl
HSPatchOutput CalcHSPatchConstants(InputPatch<HSInput, NUM_CONTROL_POINTS> patch, uint patchID)
{
    // 1. 提取三个控制点的 object-space 位置
    float3 p0 = patch[0].vertex.xyz;
    float3 p1 = patch[1].vertex.xyz;
    float3 p2 = patch[2].vertex.xyz;

    // 2. 转换到世界空间，计算三角形中心
    float3 wp0 = mul(float4(p0, 1.0f), g_World).xyz;  // object-space → world-space
    float3 wp1 = mul(float4(p1, 1.0f), g_World).xyz;
    float3 wp2 = mul(float4(p2, 1.0f), g_World).xyz;
    float3 center = (wp0 + wp1 + wp2) / 3.0f;
    float dist = distance(center, g_EyePosW);         // 三角形中心到摄像机的距离

    if (dist > _TessFar)  // 超过 _TessFar（默认 500m）的三角形不细分
    {
        output.edgeTess[0] = output.edgeTess[1] = output.edgeTess[2] = 1;
        output.insideTess = 1;
    }
    else
    {
        // 三条边各自的细分因子
        output.edgeTess[0] = TessellationHeuristic(p1, p2);  // 边 p1→p2
        output.edgeTess[1] = TessellationHeuristic(p2, p0);  // 边 p2→p0
        output.edgeTess[2] = TessellationHeuristic(p0, p1);  // 边 p0→p1
        // 内部细分因子 = 三条边的平均值
        output.insideTess = (output.edgeTess[0] + output.edgeTess[1] + output.edgeTess[2]) / 3.0f;
    }
    return output;
}
```

### 4.5 关键概念：SV_TessFactor 和 SV_InsideTessFactor

- **`edgeTess[3]`**（`SV_TessFactor`）：控制三角形**每条边**被切成几段。值越大，边上新顶点越多
- **`insideTess`**（`SV_InsideTessFactor`）：控制三角形**内部**被切成几段。通常是三条边的平均值

```
  edgeTess[0] = 4, edgeTess[1] = 4, edgeTess[2] = 4, insideTess = 4:
  
      ●--------●        每条边 4 段
     / \      / \       内部按 insideTess 细分
    /   \    /   \      新顶点数量 ≈ O(insideTess²)
   ●-----●-----●
  / \   / \   / \
 /   \ /   \ /   \
●-----●-----●-----●
```

### 4.6 控制点函数（第 66-76 行）

```hlsl
HSCPOutput HS(InputPatch<HSInput, NUM_CONTROL_POINTS> patch, uint i : SV_OutputControlPointID, ...)
{
    HSCPOutput output;
    output.vertex = patch[i].vertex;   // 第 i 个控制点：透传
    output.uv     = patch[i].uv;
    output.normal = patch[i].normal;
    return output;
}
```

与 VS 一样，HS 也是**纯透传**。HS 的真正工作已经在 `CalcHSPatchConstants` 中完成了——它决定了细分密度，而每个控制点本身不需要修改。

**`SV_OutputControlPointID`**：GPU 自动传入，取值范围 [0, NUM_CONTROL_POINTS-1]，表示当前处理的是第几个控制点。

---

## 第 5 章：Ocean_DS.hlsl — 管线的核心

Domain Shader 是曲面细分管线的工作马。细分器生成了成千上万个新顶点后，DS 负责：
1. 计算新顶点的精确位置（重心坐标插值）
2. 采样波浪位移纹理
3. 应用深度衰减
4. 变换到裁剪空间
5. 输出给光栅化器

### 5.1 函数签名（第 10-14 行）

```hlsl
[domain("tri")]                                              // 告诉 GPU：我们处理三角形
VSOutput DS(
    HSPatchOutput patchConstant,                             // HS 的输出（细分因子，这里没用到）
    float3 domain : SV_DomainLocation,                       // 【关键】细分器生成的重心坐标
    const OutputPatch<HSCPOutput, NUM_CONTROL_POINTS> patch) // 3 个控制点（HS 输出的顶点数据）
```

**`SV_DomainLocation`（重心坐标）**：
```
   patch[0]  (顶点0)
      ●
     / \
    /   \
   /  ★  \      ← ★ 是细分器新生成的顶点
  /       \
 ●---------●
patch[1]   patch[2]

domain.x = 该顶点到 patch[1] 的权重
domain.y = 该顶点到 patch[2] 的权重
domain.z = 该顶点到 patch[0] 的权重 (= 1 - x - y)
```

### 5.2 重心坐标插值（第 18-24 行）

```hlsl
float3 objPos = patch[0].vertex.xyz * domain.x    // 顶点 0 的贡献
              + patch[1].vertex.xyz * domain.y    // 顶点 1 的贡献
              + patch[2].vertex.xyz * domain.z;   // 顶点 2 的贡献
// 结果：细分顶点的精确 object-space 位置（在原始三角形平面上的插值位置）
```

这是一个线性组合（Barycentric Interpolation）：
$$P_{new} = P_0 \times w_0 + P_1 \times w_1 + P_2 \times w_2, \; w_0 + w_1 + w_2 = 1$$

### 5.3 位移采样（第 29-32 行）

```hlsl
float3 displacement;
float  foamAccum, waveHeight0;
SampleDisplacement(worldPosBase, displacement, foamAccum, waveHeight0);
```

`SampleDisplacement()` 定义在 `Ocean_Common.hlsli` 中（AI 写的共享函数）：

```hlsl
void SampleDisplacement(float3 worldPosBase,
    out float3 displacement, out float foamAccum, out float waveHeight0)
{
    float tiles[4] = { _Tile0, _Tile1, _Tile2, _Tile3 };  // 从 cbuffer 标量重建数组
    float layerContributes[4] = { ... };

    displacement = float3(0, 0, 0);
    foamAccum = 0;
    waveHeight0 = 0;
    for (int i = 0; i < 4; ++i)
    {
        float2 uv_i = worldPosBase.xz * tiles[i];      // 世界坐标 × Tile 缩放 = 采样 UV
        float4 disp = g_DisplacementSRVs[i].SampleLevel(g_Sampler, uv_i, 0);
        // disp.rgb = 位移向量 (x, y, z)  ← 来自 CS 的 FFT 计算结果
        // disp.a   = 泡沫累积值
        displacement += disp.rgb * layerContributes[i];  // 按层权重混合
        foamAccum    += disp.a   * layerContributes[i];
        if (i == 0)
            waveHeight0 = disp.y * layerContributes[i];
    }
    displacement *= _HeightStrength;  // 乘全局高度缩放
}
```

**为什么要 4 层？** 不同频率的波浪叠加，产生更自然的海洋外观：
- Layer 0 (Tile=0.04): 低频大浪（涌浪）
- Layer 1 (Tile=0.06): 中频
- Layer 2 (Tile=0.12): 高频
- Layer 3 (Tile=0.18): 最高频细浪（风浪）

### 5.4 深度衰减（第 34-40 行）

```hlsl
float4 clipPosBase = mul(float4(worldPosBase, 1.0f), g_ViewProj);
// clipPosBase.w = -viewZ（view-space 线性深度，单位米）
// 透视投影矩阵的性质：变换后 w 分量 = -viewZ

float linearDepth  = saturate(clipPosBase.w / 1000.0f); // /farPlane → 归一化到 [0,1]
float clipDepth    = 1.0f - linearDepth;                 // Unity: 1 - Linear01Depth
// clipDepth: 0 = 摄像机位置（远处）, 1 = 近平面（近处）
// 近处位移保留，远处位移衰减 → 节省性能

float attenuation  = pow(saturate(clipDepth), _DisplaceDepthAttenuation);
displacement = lerp(float3(0, 0, 0), displacement, attenuation);
// 远处 clipDepth→0 → pow(≈0, 10) ≈ 0 → 位移被抹平
// 近处 clipDepth→1 → pow(≈1, 10) ≈ 1 → 位移完全保留
```

**Bug 修复（参见 PROJECT_SESSION_HISTORY.md Bug 5）：** 原先用 `clipPos.z / clipPos.w`（NDC 非线性深度）做深度衰减，导致位移在摄像机 10m 远处就全部被抹平。改用 `clipPosBase.w`（view-space 线性深度）解决了这个问题。

### 5.5 应用位移 + 裁剪空间变换（第 43-44 行）

```hlsl
float3 worldPos = worldPosBase + displacement;  // 平面位置 + 波浪位移 = 最终世界位置
float4 clipPos  = mul(float4(worldPos, 1.0f), g_ViewProj);
```

这里进行了第二次 MVP 变换。第一次 `clipPosBase = mul(worldPosBase, VP)` 是为了拿深度信息（衰减用），第二次 `clipPos = mul(worldPos, VP)` 才是真正的最终输出。

### 5.6 输出组装（第 46-57 行）

```hlsl
output.clipPos    = clipPos;                                      // SV_POSITION → 裁剪空间
output.worldPos   = worldPos;                                     // 世界空间位置 → PS 光照用
output.uv         = worldPos.xz;                                  // 世界 XZ = 水面平面 UV
output.waveHeight = waveHeight0;                                  // 第 0 层波高 → PS 散射用
output.foam       = foamAccum;                                    // 泡沫累积 → PS 泡沫渲染
output.clipDepth  = clipDepth;                                    // 深度衰减值 → PS
output.viewDepth  = distance(worldPos, g_EyePosW);               // 到摄像机距离 → PS 雾效用
output.screenUV   = clipPos.xy / clipPos.w * 0.5f + 0.5f;        // 屏幕 UV [0,1] → PS 深度采样
output.screenUV.y = 1.0f - output.screenUV.y;                     // D3D Y 轴翻转
output.ndcDepth   = clipPos.z / clipPos.w;                        // NDC 深度 → 边缘泡沫用
```

---

## 第 6 章：Ocean_GS.hlsl — 几何着色器

### 6.1 GS 的角色

GS 是整个管线中唯一能**看到完整三角形（3 个顶点）的着色器阶段**。VS/HS/DS 每次只看到一个顶点。

在此项目中，GS 有两个职责：
1. **透传 DS 输出的所有数据**（不做任何变换）
2. **为每个三角形顶点附加重心坐标**（匹配 Unity 的 `geo()` 函数）

### 6.2 逐行解析（AI 编写）

```hlsl
[maxvertexcount(3)]                 // 最多输出 3 个顶点（三角形不变）
void GS(
    triangle VSOutput input[3],     // 输入：三角形 = 3 个 VSOutput 顶点
    inout TriangleStream<GSOutput> output  // 输出流：逐个 Append 顶点
)
{
    // 重心坐标：三个顶点分别标识自己在三角形中的位置
    // 顶点 0 = (1,0) = "我是第一条边的起点"
    // 顶点 1 = (0,1) = "我是第二条边的起点"
    // 顶点 2 = (0,0) = "我是第三个顶点"
    float2 bCentrics[3] = { float2(1, 0), float2(0, 1), float2(0, 0) };

    [unroll]
    for (uint i = 0; i < 3; i++)
    {
        GSOutput o;
        o.clipPos     = input[i].clipPos;      // 裁剪坐标透传
        o.worldPos    = input[i].worldPos;      // 世界坐标透传
        o.uv          = input[i].uv;
        o.clipDepth   = input[i].clipDepth;
        o.viewDepth   = input[i].viewDepth;
        o.screenUV    = input[i].screenUV;
        o.foam        = input[i].foam;
        o.waveHeight  = input[i].waveHeight;
        o.ndcDepth    = input[i].ndcDepth;
        o.barycentric = bCentrics[i];           // 附加重心坐标
        output.Append(o);                       // 发射这个顶点
    }
    // GS 自动 RestartStrip() — 三角形完成，进入光栅化
}
```

### 6.3 重心坐标的用途

重心坐标 `(1,0)` `(0,1)` `(0,0)` 经过光栅化器插值后，片元着色器里的每个像素得到一个 `(α, β, γ)` 值，表示"这个像素离三角形三个顶点各有多远"。

这可以用于：
- **线框模式**：`if (min(α, min(β, γ)) < edgeWidth) color = wireColor;`
- **边缘检测**
- **调试可视化**

Unity 的 GS 也生成相同的重心坐标（`g2f.barycentricCoordinates`）。虽然 Unity 的 `frag()` 没有直接使用，但在 D3D11 中，一旦 GS 被设置，它就必须通过 GS 才能到 PS。如果 GS 不输出某个字段，该字段在后续阶段就丢失了。

---

## 第 7 章：Ocean_PS.hlsl — 光照模型

### 7.1 法线重建（第 20-35 行）

水面没有传统模型的顶点法线——波浪是动态的。所以法线从**斜率纹理**中重建：

```hlsl
// 4 层斜率采样
float2 slope0 = g_SlopeSRVs[0].SampleLevel(g_Sampler, input.uv * _Tile0, 0).rg * _LayerContribute0;
float2 slope1 = ...;
// slope.r = ∂y/∂x（高度沿 X 方向的变化率）
// slope.g = ∂y/∂z（高度沿 Z 方向的变化率）

// 两层混合（A: 全部 4 层, B: 只取高频 2 层）
float2 slopeMixA = slope0 + slope1 + slope2 + slope3;
float2 slopeMixB = slope2 + slope3;  // 高频浪细节

// VariationMask 控制混合权重
float normalVarMask = g_VariationMaskSRV.Sample(...);

float2 finalSlope = lerp(slopeMixA, slopeMixB, normalVarMask) * _NormalStrength;

// 从斜率重建法线
float3 macroNormal = float3(0.0f, 1.0f, 0.0f);  // 宏观法线 = 朝上（平静水面）
float3 mesoNormal = normalize(float3(-finalSlope.x, 1.0f, -finalSlope.y));  // 中观法线 = 含波浪
// 原理：如果表面在 x 方向上升 ∂y/∂x，则法线偏转 (-∂y/∂x, 1, -∂y/∂z)
```

### 7.2 Beckmann BRDF（第 37-57 行）

BRDF（Bidirectional Reflectance Distribution Function，双向反射分布函数）描述"光从方向 L 来，在方向 V 看到多少反射"。

**Beckmann 法线分布函数**：

$$D_{Beckmann} = \frac{e^{(N \cdot H)^2 - 1) / (r^2 (N \cdot H)^2)}}{π r^2 (N \cdot H)^4}$$

- **r** = roughness（粗糙度）：r 越大 → 高光越散（磨砂效果），r 越小 → 高光越集中（镜面效果）
- **H** = halfDir = normalize(L + V)：半程向量。如果 H = N，完美反射 → D 最大
- **N·H** = 法线和半程向量的夹角：越接近 1（越平行），高光越强

```hlsl
float Beckmann(float nDotH, float roughness)
{
    float r2 = roughness * roughness;
    float expArg = (nDotH * nDotH - 1.0f) / (r2 * nDotH * nDotH);  // 指数项
    return exp(expArg) / (3.14159265f * r2 * nDotH * nDotH * nDotH * nDotH);
    // 分母：归一化因子
}
```

**Fresnel（菲涅尔）**：掠射角反射更多光（你为什么能看到远处水面的倒影，但低头看脚边的水是透明的）

```hlsl
float F0 = (eta - 1)^2 / (eta + 1)^2;  // 垂直入射反射率 (Schlick 近似, eta=1.33 水的折射率)
float fresnel = F0 + (1 - F0) * pow(1 - NdotV, 5);  // 掠射角修正
```

**Smith 几何遮蔽**：微表面互相遮挡，减少反射光

```hlsl
float viewMask  = SmithMaskBeckmann(H, V, roughness);  // 视线方向遮蔽
float lightMask = SmithMaskBeckmann(H, L, roughness);  // 光源方向遮蔽
float geometryMask = 1.0f / (1.0f + viewMask + lightMask);
```

### 7.3 次表面散射（第 59-75 行）

水不是不透明的——光进入水面后在内部散射，从另一个位置出来。这是水面颜色（蓝绿色）的主要来源。

四项散射模型（经验公式，匹配 Unity）：

| 项 | 公式 | 物理含义 |
|----|------|---------|
| **k1** | `WavePeakScatterStrength × var_H × pow(dot(L, -V), 4) × ...` | **波峰散射**：浪尖处光穿透最深，散射最强。`var_H`=波高，波越高散射越多 |
| **k2** | `ScatterStrength × pow(dot(V, N), 2)` | **视线散射**：你越垂直往下看，越能看到水下的散射光（水更透明） |
| **k3** | `ScatterShadowStrength × NdotL` | **光源散射**：光越正面照在水面上，散射越多 |
| **k4** | `AmbientDensity` | **环境散射**：即使没有直接光照，环境光也会散射（暗处水体不是纯黑的） |

```hlsl
float var_H = max(0.0f, input.waveHeight);  // 用物理波高（不用乘 _HeightStrength）
float k1 = _WavePeakScatterStrength * var_H
         * pow(DotClamped(lightVec, -viewDir), 4.0f)
         * pow(abs(0.5f - 0.5f * dot(lightVec, N)), 3.0f);
k1 = lerp(0.0f, k1, pow(saturate(input.clipDepth), _DisplaceDepthAttenuation));  // 远处衰减
```

**Bug 修复点**：AI 修改前，`var_H = input.waveHeight * _HeightStrength`，但 `_HeightStrength` 在 DS 中已用于放大位移（补偿粗网格），PS 中再用会"双重放大"。修复后 PS 直接用物理波高。

### 7.4 环境反射（第 78-81 行）

```hlsl
float3 reflectDir = reflect(-viewDir, N);  // 视线在法线方向的反射向量
float3 envReflect = g_Skybox.Sample(g_Sampler, reflectDir).rgb;  // 从天空盒采样反射颜色
```

### 7.5 边缘泡沫（第 83-96 行，AI 新增）

见第 10 章详细说明。

### 7.6 合成 + 雾（第 98-103 行）

```hlsl
// Fresnel 混合：掠射角 → 更多反射（fresnel 大）、较少散射 (1-fresnel 小)
float3 color = (1.0f - fresnel) * scatter + specular + fresnel * envReflect;

// 叠加白色泡沫
color = lerp(color, _FoamColor.rgb * lightColor, saturate(foam));

// 指数雾效
float fogFactor = ComputeExpFogFactor(input.viewDepth, _FogDensity);  // 1 - exp(-depth * density)
color = lerp(color, _FogColor.rgb, fogFactor);
```

---

## 第 8 章：OceanEffect.cpp Apply() — C++ 管线调度

`Apply()` 是每帧渲染水面时调用的函数，负责把 CPU 端准备好的数据上传到 GPU 并绑定着色器。它的 7 步流程如下：

### 第 1 步：上传 b1（OceanParams）+ b2（ChangeRarely）

```cpp
deviceContext->Map(m_pCBOceanParams, ...);   // Map → memcpy → Unmap
deviceContext->Map(m_pCBChangeRarely, ...);  // 同上
```

b1 包含波浪参数（tile、层权重、泡沫、雾等 192B），b2 包含光源数据（5 个方向光 + 5 个点光 + 5 个聚光灯，共 1200B）。

### 第 2 步：Pass::Apply()

```cpp
pImpl->m_pCurrEffectPass->Apply(deviceContext);
```

框架的 EffectPass 自动管理 VS/PS 的 cbuffer（b0）、shader、采样器绑定。

**为什么框架不管 HS/DS？** EffectPass 创建时填的是 `nameVS="OceanVS"` 和 `namePS="OceanPS"`，不包含 HS/DS/GS。所以这些阶段需要手动管理。

### 第 3 步：手动上传 b0 给 HS/DS

```cpp
CBOceanPerFrame perFrame;
perFrame.g_World    = ...;  // 世界矩阵（转置）
perFrame.g_ViewProj = ...;  // VP 矩阵（转置）
perFrame.g_EyePosW  = ...;  // 摄像机世界位置

deviceContext->HSSetConstantBuffers(0, 1, m_pCBPerFrame_HSDS);
deviceContext->DSSetConstantBuffers(0, 1, m_pCBPerFrame_HSDS);
```

HS/DS 需要 g_World 和 g_EyePosW 来计算细分因子和位移采样。

### 第 4 步：手动绑定采样器给 HS/DS

```cpp
ID3D11SamplerState* sampler = RenderStates::SSLinearWrap.Get();
deviceContext->HSSetSamplers(0, 1, &sampler);
deviceContext->DSSetSamplers(0, 1, &sampler);
```

### 第 5 步：覆盖 b1/b2 给全部 Stage

框架的 Pass::Apply() 绑定了 b1/b2 的空版本（因为 EffectPass 解析出的 cbuffer 是空的——框架自动创建了缓冲区但没填数据）。这里用正确数据覆盖：

```cpp
ID3D11Buffer* cbs[2] = { m_pCBOceanParams, m_pCBChangeRarely };
deviceContext->VSSetConstantBuffers(1, 2, cbs);
deviceContext->HSSetConstantBuffers(1, 2, cbs);
deviceContext->DSSetConstantBuffers(1, 2, cbs);
deviceContext->PSSetConstantBuffers(1, 2, cbs);
```

### 第 6 步：设置 HS/DS/GS

```cpp
if (m_TessellationEnabled)
{
    deviceContext->HSSetShader(m_pHullShader, ...);
    deviceContext->DSSetShader(m_pDomainShader, ...);
    deviceContext->GSSetShader(m_pGeometryShader, ...);  // AI 新增
}
```

### 第 7 步：绑定 SRV（t0~t10）

```cpp
// t0~t3: 位移纹理 → VS, DS, PS
deviceContext->DSSetShaderResources(0, 4, dispSRVs);
deviceContext->VSSetShaderResources(0, 4, dispSRVs);
// t4~t7: 斜率纹理 → PS, DS
deviceContext->PSSetShaderResources(4, 4, slopeSRVs);
// t8: VariationMask → PS
// t9: Skybox → PS
// t10: SceneDepthMap → PS (AI 新增)
```

**为什么 SRV 绑定必须在 Pass::Apply() 之后？** Pass::Apply() 内部会清空所有 SRV 然后再绑定它自己的。如果在这之前绑 SRV，会被清掉。

---

## 第 9 章：Q&A — 常见问题

### Q1：为什么 HLSL cbuffer 中 `float arr[4]` 占 64B 而不是 16B？

**A**：HLSL cbuffer 的默认打包规则中，**数组的每个元素按 16 字节（float4 对齐）**。这是因为 GPU 常量缓冲区的最小读写单元是 16 字节（一个 `float4` 向量寄存器）。

```hlsl
// 你以为的布局（16B）：
cbuffer {
    float _Tile[4];  // 期望: 4 × 4 = 16B
}
// 实际 GPU 布局（64B）：
cbuffer {
    float _Tile[0];  // 地址0  = 4B有效
    float _pad[3];   // 地址4  = 12B填充 → 凑够16B
    float _Tile[1];  // 地址16 = 4B有效
    float _pad[3];   // 地址20 = 12B填充
    // ... 每个元素各占 16B，总共 64B
}
```

**修复方法**：把数组拆成独立标量 `float _Tile0, _Tile1, _Tile2, _Tile3;`（各 4B，紧凑排列=16B）。在 Shader 中需要数组时用局部变量重建：`float tiles[4] = {_Tile0, _Tile1, _Tile2, _Tile3};`

### Q2：为什么 HS/DS 的 b0 要手动管理？

**A**：框架的 `EffectPass` 只管理它在创建时指定的 Shader Stage（当前是 VS + PS）。HS 和 DS 不在 Pass 的管辖范围内，框架不会为它们上传 cbuffer。所以我们需要手动创建缓冲区、手动用 `HSSetConstantBuffers` / `DSSetConstantBuffers` 绑定。

### Q3：SV_TessFactor 怎么控制细分程度？

**A**：`SV_TessFactor` 告诉硬件细分器"这条边切成几段"。值是浮点数（因为用了 `fractional_odd` 分割），可以在 [1, 64] 范围内平滑过渡。

```
TessFactor = 1  → 不切     (原始三角形)
TessFactor = 3  → 切 3 段  (边上有 4 个顶点)
TessFactor = 8  → 切 8 段  (边上有 9 个顶点)
```

### Q4：为什么用 `distance(edgeCenter, g_EyePosW)` 控制细分？

**A**：距离基 LOD（Level of Detail）。离摄像机越远的三角形，在屏幕上占的像素越少，用少量顶点就够了。离摄像机越近，需要更多顶点才能呈现波浪细节。

公式中的 `pow(viewDist * 0.5, 1.2)` 让细分因子随距离快速衰减（1.2 次方衰减比线性更快），确保远距离性能。

### Q5：_TessEdgeLength 和 _TessFar 的区别？

- **`_TessEdgeLength`（默认 16）**：控制在"多远"的距离上三角形应该达到屏幕像素级的精度。越小 → 细分越密（性能消耗越大）。
- **`_TessFar`（默认 500）**：绝对距离阈值。超过这个距离的三角形完全不做细分（TessFactor=1）。

Bug 历史：`_TessFar` 原来设的是 1.0f，意味着距离摄像机超过 1m 就不细分 → 等于整个水面都没开曲面细分。修复为 500.0f 后才有效果。

### Q6：`clipPosBase.w` 为什么是线性深度？

**A**：D3D11 透视投影矩阵的形式：

$$P = \begin{bmatrix} \frac{2n}{r-l} & 0 & 0 & 0 \\ 0 & \frac{2n}{t-b} & 0 & 0 \\ 0 & 0 & \frac{f}{f-n} & 1 \\ 0 & 0 & -\frac{nf}{f-n} & 0 \end{bmatrix}$$

将 `(x_w, y_w, z_w, 1)` 乘这个矩阵，w 分量 = `z_w`（view-space Z）。所以 `clipPosBase.w = -viewZ`，是线性的！

对比：`clipPosBase.z / clipPosBase.w` 是 NDC 深度（非线性的，1/z 关系），因为 z 分量包含了常数项。

### Q7：Beckmann BRDF 和 Fresnel 的物理含义？

**Beckmann BRDF** 是微表面模型的一种，把表面看作无数微小镜面的集合：
- **粗糙表面**（roughness 大）：微镜面朝向随机 → 光散开 → 高光区域大但暗（漫反射感）
- **光滑表面**（roughness 小）：微镜面朝向一致 → 光集中 → 高光区域小但亮（镜面感）

**Fresnel** 是电磁波在介质界面的反射率：
- 垂直看水面（N·V≈1）：Fresnel ≈ 0.02 → 97% 透射，3% 反射（水很透明）
- 掠射角度看水面（N·V→0）：Fresnel ≈ 1 → 几乎全反射（水面像镜子）
- 水折射率 η=1.33，F0 ≈ 0.02

### Q8：散射 k1/k2/k3/k4 各自的物理含义？

| 项 | 名称 | 物理现象 |
|----|------|---------|
| k1 | Wave Peak Scatter | 浪尖的逆光散射——从浪尖看光源方向，光穿透最深 |
| k2 | View Scatter | 视线穿透散射——垂直看水时光路最短，散射最少，水最透明 |
| k3 | Light Scatter | 光源强度散射——光源越正面，光射入水越深，散射越多 |
| k4 | Ambient Scatter | 环境光散射——阴天/阴影下水的底色（暗蓝绿色） |

### Q9：_HeightStrength 在 DS 和 PS 中的不同角色？

- **DS 中**：`displacement *= _HeightStrength` — 乘以 _HeightStrength 放大波浪位移。当前值 1.0，即使用物理正确值。如果水面的粗网格无法呈现足够细节，可以增大此值来"补偿"。
- **PS 中**：散射计算的 `var_H = input.waveHeight`（不带 _HeightStrength）— 散射应基于真实波高，而非放大后的视觉高度。

---

## 第 10 章：边缘泡沫原理（AI 新增）

### 10.1 问题

普通泡沫（`input.foam`）由波浪 Jacobian 决定——浪尖处泡沫多。但还有一个场景：**物体与水面交界处**（如球体半浸在水中），应该有白色泡沫环绕。

### 10.2 Unity 方案

Unity 用 `_CameraDepthNormalsTexture`（Unity 引擎内置的场景深度 + 法线纹理，在渲染水面之前由引擎自动生成）：

```hlsl
float screenDepth = DecodeFloatRG(tex2D(_CameraDepthNormalsTexture, screenUV).zw);
float depthDiff = screenDepth - viewDepth;    // 场景深度 - 水面深度
float intersect = 0;
if (depthDiff > 0)                             // 场景在水面后方（正常情况）
    intersect = 1 - smoothstep(0, _ProjectionParams.w, depthDiff);
foam = foam + intersect * pow(foam, _EdgeFoamPower);  // 交界处泡沫加成
```

### 10.3 D3D11 实现方案

在 D3D11 中，没有 Unity 的 `_CameraDepthNormalsTexture`。我们的方案：

1. **DrawScene 渲染顺序**：球体 → **深度拷贝** → 水面 → 天空盒
2. **深度拷贝**：`CopySubresourceRegion` 从主深度缓冲拷贝到 `m_pSceneDepthCopyTex`
3. **水面 PS**：采样拷贝的深度纹理，与水面自身 NDC 深度比较

### 10.4 数学推导

```
场景渲染后，主深度缓冲中的值：
  sceneNdcDepth = sceneClipPos.z / sceneClipPos.w  ∈ [0, 1]

水面顶点的 NDC 深度（DS 计算）：
  waterNdcDepth = waterClipPos.z / waterClipPos.w  ∈ [0, 1]

深度差：
  depthDiff = sceneNdcDepth - waterNdcDepth

  如果球体在水面之后：sceneNdcDepth > waterNdcDepth → depthDiff > 0
  如果无物体在水面后：sceneNdcDepth = 1.0（远平面）→ depthDiff ≈ 1
  如果球体贴近水面：depthDiff ≈ 0

smoothstep(0, 0.001, depthDiff)：
  depthDiff → 0    → smoothstep → 0          → intersect → 1  (很多泡沫)
  depthDiff → 0.001 → smoothstep → 1         → intersect → 0  (无泡沫)
  阈值 0.001 ≈ 1/farPlane → 约 1m 物理距离的泡沫过渡带
```

### 10.5 格式注意

场景深度纹理使用 `DXGI_FORMAT_R24_UNORM_X8_TYPELESS` 格式（与主深度缓冲 `R24G8_TYPELESS` 兼容）。采样得到的 `.r` 值是 UNORM 归一化的 NDC 深度 [0, 1]，直接与 `input.ndcDepth` 比较。

---

## 附录 A：关键参数速查

| 参数 | 默认值 | 作用 |
|------|--------|------|
| `_TessEdgeLength` | 16 | 细分密度：越小越密 |
| `_TessFar` | 500 | 细分最远距离（m），超过不细分 |
| `_HeightStrength` | 1.0 | 位移放大系数 |
| `_NormalStrength` | 1.0 | 法线强度 |
| `_Roughness` | 0.05 | 水面粗糙度（0=镜面，1=磨砂） |
| `_FogDensity` | 0.01 | 雾密度（米制适配） |
| `_DisplaceDepthAttenuation` | 10 | 远处位移衰减指数 |
| `_FoamDepthAttenuation` | 20 | 远处泡沫衰减指数 |
| `_EdgeFoamPower` | 0.2 | 边缘泡沫强度 |
| `_ShadowIntensity` | 0.2 | 阴影环境光补偿 |

## 附录 B：Shader 文件依赖关系

```
Ocean_VS.hlsl ──┐
Ocean_HS.hlsl ──┤
Ocean_DS.hlsl ──┼── #include "Ocean_Common.hlsli" ── #include "LightHelper.hlsli"
Ocean_GS.hlsl ──┤
Ocean_PS.hlsl ──┘

6 个 CS 文件 ──── #include "FFT_Ocean_CS_Common.hlsli"（独立、与渲染管线隔离）
```
