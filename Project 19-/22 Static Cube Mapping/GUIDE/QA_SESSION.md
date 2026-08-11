# FFT Ocean — Q&A 对话记录

> 项目：`Project 19- / 22 Static Cube Mapping`
> 主题：FFT 水面模拟 + 曲面细分渲染管线
> 角色：用户（学习者）/ Claude（AI 导师）
> 开始于：2026-06-22

---

### `[2026-06-22 15:30]` **Q1**

**用户**：

> HS 里面细分因子被确定之后，DS 里面就去确定新的顶点是吗？位移采样和深度衰减是什么，尤其是深度衰减。DS 输入是一个一个顶点输入进来的，但是经过细分之后会出现新的顶点，那如果是这样子的话，不就要输出多个顶点了吗，这样一对多怎么可行呢？

---

### `[2026-06-22 15:30]` **A1**

**Claude**：

你的困惑来自一个常见的误解——DS **不是**一对多，而是**多对一**：

```
  HS 输出 3 个控制点 + 细分因子 (edgeTess=4, 内部=4)
                  │
                  ▼
  ┌─────────────────────────────────────┐
  │  硬件细分器 (Tessellator)            │
  │  固定功能硬件，不可编程               │
  │  根据细分因子，在三角形内部            │
  │  自动生成 N 个新顶点                  │
  │  每个新顶点附带唯一重心坐标 domain.xyz │
  └─────────────────────────────────────┘
                  │
                  ▼  假设生成了 27 个新顶点
  ┌─────────────────────────────────────┐
  │  DS 被 GPU 并行调用 27 次             │
  │  每次调用的输入：                     │
  │    • 完全相同的 3 个控制点 (patch)     │
  │    • 独一无二的重心坐标 (domain)       │
  │  每次调用的输出：1 个 VSOutput        │
  └─────────────────────────────────────┘
                  │
                  ▼
           27 个 VSOutput → 光栅化器
```

**类比**：
- HS = 工头，说"这个三角形每条边切成 4 段，内部也切 4 段"
- 细分器 = 自动切割机，切出 27 个新顶点，每个标好"你在三角形里的哪个位置"（重心坐标）
- DS = 工人，被 GPU 叫了 27 次，每次告诉你"处理第 k 个顶点，它在三角形里的坐标是 (0.3, 0.2, 0.5)"

你问的"一对多怎么可行"——实际上不是"一个顶点输入变成多个顶点输出"，而是**GPU 硬件帮你生成了 N 个新顶点，然后为每个新顶点调用一次 DS**。DS 的每次调用和 VS 一样：输入一个顶点，输出一个顶点。

---

**位移采样**：

原始水面网格是一个**完全平坦**的平面（所有顶点 y = 0）。波浪的形状 100% 来自位移纹理：

```
  平面顶点 (x, 0, z)
       │
       ▼  SampleDisplacement( )
  从 4 层位移纹理采样，按层权重混合
       │
       ▼
  得到 (Δx, Δy, Δz) —— y 分量就是波高！
       │
       ▼
  最终顶点 = (x+Δx, Δy, z+Δz)  ← 现在有波浪了
```

位移纹理是 CS 端 6 个 Compute Shader 每帧实时算出来的（JONSWAP 频谱 → IFFT → 空间域）。

---

**深度衰减** — 解决什么问题？

水面 256m×256m。500m 外的三角形在屏幕上不到 5 个像素。为这几个像素去完整计算波浪位移 = 纯粹浪费。而且远处三角形大（不细分），位移采样精度也差。

做法：

```
1. 这个顶点离摄像机多远？
   linearDepth = clipPosBase.w / 1000.0f    // 0~1000m → 0~1 归一化

2. 翻转成"近处=1，远处=0"
   clipDepth = 1.0 - linearDepth

3. 指数衰减（DisplaceDepthAttenuation=10）
   attenuation = pow(clipDepth, 10)
   // 10m 处:  clipDepth≈0.99,  pow(0.99,10)≈0.90  → 位移保留 90%
   // 100m 处: clipDepth≈0.90,  pow(0.90,10)≈0.35  → 位移保留 35%
   // 500m 处: clipDepth≈0.50,  pow(0.50,10)≈0.001 → 位移几乎抹平

4. 混合
   displacement = lerp(0, 原始位移, attenuation)
```

直观效果：站在海边，脚边的水每个小波纹都看得清 → 500m 外只剩涌浪轮廓 → 地平线处完全平坦。

**历史 Bug**（`PROJECT_SESSION_HISTORY.md` Bug 5）：旧代码用 `clipPos.z/clipPos.w`（NDC 非线性深度）。摄像机离水面才 10m，NDC 深度已经到 0.8，`pow(0.2, 10)≈0`，所有位移全被抹平——水面死平。改用 `clipPosBase.w`（view-space 线性深度，单位米）才修好。

---

### `[2026-06-22 16:00]` **Q2**

**用户**：

> 在前面你提到HS/DS每次只能看见一个顶点，但是代码里面不是要同时获取多个顶点信息来决定细分因子吗？

---

### `[2026-06-22 16:00]` **A2**

**Claude**：

你说得对，我之前过度简化了，甚至有误。HS 内部其实**拆成了两个独立函数**，它们的"视野"完全不同：

```
  VS 输出: 三角形 3 个顶点 [v0, v1, v2]
               │
               ├──► HS 控制点函数 (HS)           → 被调用 3 次，每次只看 1 个顶点
               │        ↓
               │    输出 3 个 HSCPOutput（透传，啥也不改，纯搬运工）
               │
               └──► HS 面片常量函数 (CalcHSPatchConstants) → 只被调用 1 次，同时看到 3 个顶点！
                        ↓
                    输出 HSPatchOutput（edgeTess[3] + insideTess）→ 送给硬件细分器
```

**这是 D3D11 曲面细分的核心设计**——`[patchconstantfunc("CalcHSPatchConstants")]` 这个 attribute 把 HS 拆成两个独立函数。

---

**逐函数对比：**

| | HS 控制点函数 | HS 面片常量函数 |
|---|---|---|
| 被调用次数 | 每个控制点 1 次 = 三角形 **3 次** | 每个三角形 **1 次** |
| 看到的顶点数 | **1 个** | **全部 3 个** ✓ |
| 输入类型 | `patch[i]` + `SV_OutputControlPointID` | `InputPatch<HSInput, 3> patch` + `SV_PrimitiveID` |
| 输出 | `HSCPOutput`（顶点数据 → 给 DS） | `HSPatchOutput`（细分因子 → 给硬件细分器） |
| 职责 | 透传数据（纯搬运） | **计算"这个三角形该切成几段"** |
| 代码位置 | `Ocean_HS.hlsl:66-76` | `Ocean_HS.hlsl:27-59` |

---

**代码对照：**

```hlsl
// ★ 面片常量函数 — 同时拿到全部 3 个顶点
HSPatchOutput CalcHSPatchConstants(
    InputPatch<HSInput, 3> patch,  // ← 3 个顶点一次全给！
    uint patchID : SV_PrimitiveID)
{
    float3 p0 = patch[0].vertex.xyz;  // 同时访问顶点 0
    float3 p1 = patch[1].vertex.xyz;  // 同时访问顶点 1
    float3 p2 = patch[2].vertex.xyz;  // 同时访问顶点 2

    // 转换到世界空间，算出三角形中心离相机多远
    float3 center = (mul(p0,World) + mul(p1,World) + mul(p2,World)) / 3;
    float dist = distance(center, g_EyePosW);

    if (dist > _TessFar)
        // 太远了，不细分（3条边各1段）
        output.edgeTess[0] = output.edgeTess[1] = output.edgeTess[2] = 1;
    else
        // 近处，每条边独立算细分因子
        output.edgeTess[0] = TessellationHeuristic(p1, p2);  // 边 1→2
        output.edgeTess[1] = TessellationHeuristic(p2, p0);  // 边 2→0
        output.edgeTess[2] = TessellationHeuristic(p0, p1);  // 边 0→1
}
```

```hlsl
// ★ 控制点函数 — 每次只看 1 个顶点
HSCPOutput HS(
    InputPatch<HSInput, 3> patch,
    uint i : SV_OutputControlPointID,  // i=0,1,2 —— 当前处理第几个
    ...)
{
    HSCPOutput output;
    output.vertex = patch[i].vertex;  // 只看 patch[i]，不看另外两个
    output.uv     = patch[i].uv;
    output.normal = patch[i].normal;
    return output;  // 被调用 3 次（i=0,1,2）
}
```

---

**DS 呢？DS 也能看到全部 3 个控制点！**

```hlsl
VSOutput DS(
    ...
    const OutputPatch<HSCPOutput, 3> patch)  // ← 3 个控制点全在
{
    // 用重心坐标 domain.xyz 对 3 个控制点做加权平均
    float3 objPos = patch[0].vertex * domain.x   // 顶点0 × 权重x
                  + patch[1].vertex * domain.y   // 顶点1 × 权重y
                  + patch[2].vertex * domain.z;  // 顶点2 × 权重z
    // ...
}
```

DS 每次调用拿到 3 个控制点 + 1 个重心坐标，输出 1 个细分后顶点。GPU 并行调用 N 次。

---

**修正后的完整视野表：**

```
  阶段              每次调用看到几个顶点      被调用次数/三角形
  ─────────────────────────────────────────────────────────
  VS                1 个                      3 次
  HS 控制点函数      1 个                      3 次
  HS 面片常量函数    ★ 3 个（全部）             1 次
  硬件细分器         —（固定功能硬件）          —
  DS                ★ 3 个 + 重心坐标          N 次（每个新顶点1次）
  GS                ★ 3 个（完整输出三角形）    1 次
  PS                1 个（插值后像素）          ~屏幕覆盖像素数
```

之前我笼统地说"HS/DS 每次只能看见一个顶点"是**错的**。正确的说法是：**HS 控制点函数**只看 1 个顶点，**HS 面片常量函数**看全部 3 个——这正是 HS 要拆成两个函数的原因：一个做"全局决策"（需要全貌），一个做"逐顶点透传"（只看自己）。

---

### `[2026-06-22 16:20]` **Q3**

**用户**：

> 你不是说DS里面的细分顶点使用固定的硬件来完成吗，我想知道是哪个语句发起了对这个硬件的调用，使顶点输入进入这个流程？

---

### `[2026-06-22 16:20]` **A3**

**Claude**：

没有对应的 HLSL 语句。硬件细分器**不需要、也不存在一个 "调用它" 的指令**。它是靠**管线配置**自动激活的。

---

**类比：光栅化器是怎么启动的？**

你写 PS 的时候，从来没写过 `call Rasterizer()` 对吧？光栅化器是固定功能硬件，只要管线里设置了 VS + PS + 三角形拓扑，硬件自动就跑 VS → 光栅化 → PS 这个流程。

**硬件细分器完全一样**：它不是一个"被调用的函数"，而是管线中间的一个**自动门**。门开不开，取决于你配了哪些 Stage。

---

**触发条件：三个缺一不可**

```
  C++ 端设拓扑类型                    HLSL 端绑定着色器
  ─────────────────────               ──────────────────
                                      ① HSSetShader( ... )  ← HS 存在
  ② 拓扑 = PATCHLIST_3               ③ DSSetShader( ... )  ← DS 存在
```

**三个条件都满足 → 管线自动走细分路径：**

```
  IA → VS → HS → [硬件细分器] → DS → GS → 光栅化器 → PS
                   ▲
                   │
              自动激活！没有任何 HLSL 指令触发它
```

**缺少任意一个 → 不走细分：**

```
  IA → VS → 光栅化器 → PS     （传统管线，没有 HS/DS，拓扑用 TRIANGLELIST）
```

---

**具体代码追踪（`OceanEffect.cpp`）：**

**条件 ② — 拓扑类型（`SetRenderDefault`，第 204-211 行）：**

```cpp
void OceanEffect::SetRenderDefault()
{
    pImpl->m_pCurrEffectPass = ...;
    pImpl->m_pCurrInputLayout = ...;
    pImpl->m_CurrTopology = pImpl->m_TessellationEnabled
        ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST  // ← 细分路径！
        : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;               // ← 传统路径
}
```

`PATCHLIST_3` 告诉 IA（输入装配器）："每 3 个顶点组成一个 patch，不是普通三角形"。这是 GPU 判断"要不要走细分管线"的**第一个信号**。

**条件 ①③ — HS/DS 绑定（`Apply`，第 455-464 行）：**

```cpp
// ———— 这段代码 + 上面的 PATCHLIST_3 = 硬件细分器被激活 ————
if (pImpl->m_TessellationEnabled)
{
    deviceContext->HSSetShader(m_pHullShader, nullptr, 0);     // ① 挂上 HS
    deviceContext->DSSetShader(m_pDomainShader, nullptr, 0);   // ③ 挂上 DS
    deviceContext->GSSetShader(m_pGeometryShader, nullptr, 0); //   挂上 GS（可选）
}
```

`HSSetShader` + `DSSetShader` 这俩 D3D11 API 调用就是"打开细分之门"的钥匙。但注意——它们只是**把着色器对象绑定到管线**，绑完之后，GPU 硬件自己判断"HS 绑了 + 拓扑是 Patch → 走细分路径"。

---

**完整流程图：**

```
  C++ DrawScene()
       │
       ├── SetRenderDefault()
       │      └── 设置拓扑 = PATCHLIST_3           ← 条件②
       │
       ├── SetEyePos() / SetWorldMatrix() / ...
       │      └── 填充 cbuffer 数据
       │
       └── Apply()
              ├── Pass::Apply()      → 框架绑 VS + PS
              ├── HSSetShader(...)   → 条件①  ← 这些 API 调用
              ├── DSSetShader(...)   → 条件③  ← 激活了细分管线
              ├── GSSetShader(...)   → GS（可选）
              └── 绑 SRV / cbuffer

  ── 以上全部是 CPU 端的"配置"，不是"调用" ──

  Draw() 被调用
       │
       ▼
  【GPU 硬件】
  看到拓扑 = PATCHLIST_3 + HS 已绑 + DS 已绑
       │
       ▼
  自动走: VS → HS → [硬件细分器] → DS → GS → 光栅化器 → PS
                        ▲
                   这里没有任何 HLSL 指令！
                   硬件看到 patch 的 3 个控制点 + HS 输出的 edgeTess
                   就自己开始切三角形、生成新顶点、喂给 DS
```

---

**总结：**

硬件细分器不是被"调用"的——它是被**管线配置激活**的。就像你把水龙头拧开（配 PATCHLIST_3 + HSSetShader + DSSetShader），水（顶点流）就自动流过细分器。没有一行 HLSL 代码负责"启动细分器"。

类比：

| 管线 Stage | 触发方式 |
|---|---|
| VS | `VSSetShader()` + `Draw()` |
| HS | `HSSetShader()` + **拓扑 = PATCHLIST** |
| 硬件细分器 | **自动**（有 HS + DS + PATCHLIST 就自动介入） |
| DS | `DSSetShader()` + 细分器输出顶点 |
| 光栅化器 | **自动**（前面 Stage 输出图元就给光栅化器） |
| PS | `PSSetShader()` + 光栅化器输出像素 |

光栅化器和硬件细分器都是**固定功能硬件**——配好了自动跑，不需要、也没有对应的 HLSL 调用语句。

---

### `[2026-06-22 16:40]` **Q4**

**用户**：

> 深度衰减那一块的作用是不是太远的三角形就减少细分三角形数量和位移纹理采样精度？

---

### `[2026-06-22 16:40]` **A4**

**Claude**：

你问的问题其实包含了**两个独立机制**，它们都是 LOD（细节层次）策略的一部分，但操作的是不同的东西。

---

**机制一：曲面细分减少（HS 控制）— "少切几刀"**

发生在 **Hull Shader**，控制的是**顶点数量**：

```
  HS 面片常量函数:
    if (三角形中心离相机 > _TessFar)  → edgeTess = 1（完全不切，就是原始三角形）
    else                               → edgeTess 根据距离动态计算（近处切 20 刀，远处切 3 刀）
```

参数：`_TessFar`（500m）、`_TessEdgeLength`（16）

效果：**远 = 少顶点，近 = 多顶点**

---

**机制二：深度衰减（DS 控制）— "浪变小一点"**

发生在 **Domain Shader**，控制的是**每个顶点的位移幅度**：

```hlsl
attenuation = pow(clipDepth, _DisplaceDepthAttenuation);
displacement = lerp(0, 原始位移, attenuation);
```

参数：`_DisplaceDepthAttenuation`（默认 10）

效果：**远 = 位移缩到接近 0（水面趋近平坦），近 = 位移完全保留**

---

**它们是两个完全独立的机制：**

| | 细分减少（HS） | 深度衰减（DS） |
|---|---|---|
| 控制什么 | 顶点**数量** | 位移**大小** |
| 代码位置 | `Ocean_HS.hlsl` | `Ocean_DS.hlsl` |
| 关键参数 | `_TessFar`, `_TessEdgeLength` | `_DisplaceDepthAttenuation` |
| 极端情况 | edgeTess=1 → 3 顶点三角形 | attenuation=0 → 位移=0 → 水面平的 |
| 效果 | 远处三角形大而稀疏 | 远处波浪幅度衰减至零 |

---

**为什么需要两个？**

假设只有细分减少、没有深度衰减：
- 500m 外三角形 edgeTess=1，不切分 → 一个边长 4m 的大三角形
- 但位移纹理照常采样，给这个三角形的 3 个顶点赋予 ±2m 的波浪高度
- 结果：远处水面**三角形又大、波浪又夸张**，看起来像锯齿状的乱跳，很难看

加上深度衰减后：
- 500m 外的三角形 → 位移被压到 ~0 → 水面接近完全平坦
- 虽然三角形大，但反正水面是平的，不需要细节 → 完美配合

反过来，只有深度衰减、没有细分减少：
- 500m 外位移确实衰减到 0，水面是平的
- 但每帧还在为这 500m 外的三角形调用几十次 DS → 纯浪费
- 细分减少直接 edgeTess=1 → 不切分 → DS 只被调用 3 次 → 省性能

**两者互补：HS 减少顶点数量（省性能），DS 减少位移幅度（保证视觉正确）。**

---

**直观总结：**

```
  ┌──────────────────────────────────────────────────────┐
  │                        摄像机                          │
  │                          ●                            │
  │                         /|\                           │
  │                          │                            │
  │   近处 (0~50m)           │          远处 (100~500m)     │
  │   ┌──────────┐           │          ┌──────────┐       │
  │   │ 细分: 密  │           │          │ 细分: 疏  │       │
  │   │ edgeTess  │           │          │ edgeTess  │       │
  │   │  ≈ 20    │           │          │  ≈ 2~3   │  ←HS  │
  │   ├──────────┤           │          ├──────────┤       │
  │   │ 衰减: 弱  │           │          │ 衰减: 强  │       │
  │   │ atten     │           │          │ atten     │       │
  │   │  ≈ 0.95  │           │          │  ≈ 0.01  │  ←DS  │
  │   ├──────────┤           │          ├──────────┤       │
  │   │ 结果:     │           │          │ 结果:     │       │
  │   │ 密集顶点  │           │          │ 稀疏顶点  │       │
  │   │ 大波浪    │           │          │ 近乎平坦  │       │
  │   └──────────┘           │          └──────────┘       │
  └──────────────────────────────────────────────────────┘
```

两者协同工作实现了完整的距离基 LOD：近处细节丰富（多顶点+大波浪），远处性能友好（少顶点+平面）。

---

### `[2026-06-22 17:00]` **Q5**

**用户**：

> 我怎么觉得好像GS没有什么必要的作用啊，还是说我们这里根本就并没有用到GS的功能，他生成的重心坐标是不是给细分出的新顶点划线框用的？

---

### `[2026-06-22 17:00]` **A5**

**Claude**：

你的直觉完全正确——**当前项目中 GS 的确没有产生任何可见的渲染效果**。我诚实地拆开来说：

---

**当前 GS 做了什么？**

```hlsl
[maxvertexcount(3)]
void GS(triangle VSOutput input[3], inout TriangleStream<GSOutput> output)
{
    float2 bCentrics[3] = { float2(1,0), float2(0,1), float2(0,0) };  // 重心坐标

    for (uint i = 0; i < 3; i++)
    {
        GSOutput o;
        o.clipPos = input[i].clipPos;       // ← 透传
        o.worldPos = input[i].worldPos;     // ← 透传
        // ... 所有字段都是透传 ...
        o.barycentric = bCentrics[i];       // ← 新加了重心坐标，但 PS 没人读它！
        output.Append(o);
    }
}
```

两件事：
1. 把 DS 输出的所有字段**原样复制**到 GS 输出 → 纯透传，什么都没改
2. 给每个顶点附上一个重心坐标 → 但 PS 里从来没有 `input.barycentric` 的引用

结果：**如果直接把 GS 删掉（`GSSetShader(nullptr)`），画面不会有任何变化。**

---

**那为什么要有 GS？重心坐标是干嘛的？**

你说得对——重心坐标的经典用途就是**线框模式**。假如 PS 里有这段代码：

```hlsl
// 伪代码：用重心坐标做线框叠加
float3 bary = float3(input.barycentric.x, input.barycentric.y,
                     1.0 - input.barycentric.x - input.barycentric.y);
float minDist = min(bary.x, min(bary.y, bary.z));
float wire = 1.0 - smoothstep(0.0, 0.02, minDist);  // 离边越近越黑
color = lerp(color, float3(0,0,0), wire);  // 三角形边缘画黑线
```

效果：每个细分后的小三角形周围都有一圈细黑线 → 可以看到曲面细分的网格密度。

但我们目前**没有实现线框模式**，所以重心坐标生成之后就被光栅化器丢弃了，等于白算。

---

**那为什么还要保留 GS？**

三个原因，从最不重要到最重要：

**① 匹配 Unity 架构（次要）**

Unity 的 `FFTOcean_Shader.shader` 有 GS（`#pragma geometry geo`），我们复刻了相同的管线结构。虽然没有功能必要性，但保持了与参考代码的一一对应。

**② 数据流的完整性（中等）**

在 D3D11 的曲面细分管线下，一旦 GS 被绑定，它就成了管线中**唯一能看到完整三角形（3 个顶点）的阶段**。如果未来想做：

- 逐三角形的法线计算（`cross(v1-v0, v2-v1)`）
- 动态剔除（三角形面朝摄像机背面的就不要了）
- 渲染到 Cubemap 的多个面

GS 就是唯一能做这些事的地方。现在代码留好了框架，未来要加功能不需要从头来过。

**③ 教育目的（最重要）**

对于学习曲面细分管线的人来说，GS 是完整流水线的一部分。看到 `VS → HS → Tessellator → DS → GS → PS` 全链路跑通，亲眼确认"DS 输出的数据确实经过了 GS 才到达 PS"，这个认知价值比单纯删掉 GS 更大。

---

**诚实结论**

| 问题 | 答案 |
|------|------|
| GS 当前有视觉作用吗？ | **没有**。删掉它，画面不变 |
| 重心坐标当前被用到了吗？ | **没有**。生成即丢弃 |
| 保留它有实际意义吗？ | **有**——线框模式 + 未来扩展 + 学习完整性 |
| 开 GS 有性能开销吗？ | **有一点**——GPU 多跑一个 Stage，每个三角形多复制几行数据。但水面本来只有 4096 个原始顶点，GS 开销可以忽略不计 |

如果你有一天想加线框模式看细分网格，把上面那段 `smoothstep` 代码加到 PS 里，立刻就能用上现在 GS 生成的重心坐标。这就是为什么 Unity 保留了这个字段（即使它当前的 `frag()` 也没用 `barycentricCoordinates`）。

---

**一句话总结：GS 目前是"基建"——房子还没盖，但水管电线已经铺好了。**

---

### `[2026-06-22 17:20]` **Q6**

**用户**：

> 现在我看到了PS中次表面散射那一部分，原来次表面散射就是使用数学公式来模拟出来的，我以前对其应用的了解都是什么玉石和半透明材质之类的，竟然水面也用得到这个吗？

---

### `[2026-06-22 17:20]` **A6**

**Claude**：

水面不仅用得到次表面散射——对于大面积水体（海洋、湖泊），**散射是水体颜色的主要来源**，比高光反射更决定水的"样子"。

---

**先做一个实验：如果关掉散射，水会长什么样？**

PS 的合成公式是：

```hlsl
color = (1 - fresnel) * scatter   // ← 散射：水体的蓝绿色
      + specular                   // ← 高光：太阳的白色亮点
      + fresnel * envReflect;      // ← 反射：天空的倒影
```

如果把 `scatter` 那三项（k1+k2+k3+k4）全设成 0：

```
  垂直往下看水面（fresnel≈0.02）：color ≈ 0.02 × 散射(=0) + 高光 + 0.02 × 反射
                               ≈ 几乎黑色！只有一丁点天空倒影
```

你会看到**黑色的水**，只有太阳高光和远处天空反射。这不是你见过的任何真实水体。

水的蓝色/蓝绿色**不是来自表面反射**（如果水面是完美镜面，你看到的是天空的颜色），而是来自光进入水体后经历的真实物理过程：

```
  太阳光（白光）
       │
       ▼  进入水面
  ┌─────────────────────────┐
  │  水分子 + 悬浮颗粒       │
  │                         │
  │  红光被吸收（水对红光吸收强）│   → 剩下的光偏蓝绿
  │  蓝绿光被散射             │
  │  部分散射光从水面出来     │
  └─────────────────────────┘
       │
       ▼
  你看到的水体颜色（蓝绿色）
```

**散射就是水体颜色的物理根源。** 高光（specular）只是表面的亮斑，反射（envReflect）只是倒影。去掉散射，水就不是水了。

---

**这个 PS 里的散射模型：经验公式，不是物理模拟**

真正的物理散射需要模拟光子在水中走很多步（体积散射），在实时渲染中做不了。这个项目的散射是**4 项经验公式**，各自捕捉一个关键视觉现象：

```hlsl
// k1 — 波峰散射：
//     浪尖处光穿透最深 → 最亮、颜色最饱和的区域
float k1 = _WavePeakScatterStrength * var_H              // 波越高，散射越多
         * pow(dot(L, -V), 4.0f)                         // 视线和光源方向相同时最强
         * pow(abs(0.5 - 0.5 * dot(L, N)), 3.0f);       // 光线掠过水面时最强

// k2 — 视线散射：
//     垂直往下看 → 光路最短 → 散射最少 → 水最透明
//     掠射角度看 → 光路长 → 散射多 → 水更不透明
float k2 = _ScatterStrength * pow(dot(V, N), 2.0f);

// k3 — 光源散射：
//     太阳越高/越正面，光照越强 → 水越亮
float k3 = _ScatterShadowStrength * NdotL;

// k4 — 环境散射：
//     即使完全没阳光（阴天/阴影），水也不是纯黑的
//     环境光依然穿透水面产生底色（深蓝绿色）
float k4 = _AmbientDensity;
```

这四项叠加起来，加上 `_ScatterColor`（蓝绿色）和 `_ScatterPeakColor`（明亮的蓝绿色），就构成了你在水面上看到的所有非反射颜色。

---

**和玉石/皮肤的次表面散射有什么不同？**

| | 玉石/皮肤 | 海水 |
|---|---|---|
| **物理机制** | 光子进入固体内部，在微结构中弹跳后从附近出来 | 光子在水中被悬浮颗粒散射 |
| **视觉表现** | 光"扩散"到表面外（阴影边缘发红/发光） | 整体颜色改变（白色光变蓝绿） |
| **渲染方法** | 屏幕空间模糊 / 纹理空间扩散（昂贵） | **经验公式**（k1~k4，便宜） |
| **为什么不同** | 散射距离短（mm 级），需要逐像素计算扩散 | 散射深度大（m 级），整片水体均匀变色 |

换句话说：海水散射是**全局的颜色偏移**，可以用经验项近似；玉石散射是**局部的光扩散**，需要更贵的算法。

---

**这个 PS 中散射的实际视觉贡献：**

```
  Fresnel 混合权重：

  垂直看 (fresnel≈0.02):  2% 反射天空 + 高光 + 98% 散射 =  看到水的蓝绿色
  45°看  (fresnel≈0.15): 15% 反射天空 + 高光 + 85% 散射 =  看到蓝绿中带一点天空
  掠射看 (fresnel≈0.95): 95% 反射天空 + 高光 +  5% 散射 =  水面像镜子
```

这完美吻合现实：**站在船边垂直看水 → 清澈蓝绿；看远处海平线 → 天空的倒影。** 如果没有散射，垂直看水就是黑的，这违背所有真实观察。

---

**一句话：水面不是白纸上的镜子——它是发光的蓝绿色宝石，散射就是它的"自发光"。**

---

### `[2026-06-24 14:00]` **Q7**

**用户**：

> 我还是不太理得清HS到DS这一段的数据流。HS决定细分因子，交给Tess，那么Tess是已经切出了新顶点新三角形了吗，还是生成了什么半成品交给DS，DS里面的重心插值又是何物，生成重心坐标不是GS的工作吗，位移采样指的是不是直接就在DS里面采样SRV进行计算顶点位移了啊。输入DS的是一整个三角形输入进去吗，是细分前的三角形还是细分后的新三角形？

---

### `[2026-06-24 14:00]` **A7**

**Claude**：

你的混乱完全正常——因为这段管线里有**两个不同的"重心坐标"概念**被混在一起了。让我逐层拆开。

---

## 第一层：Tessellator 产出了什么？

**不是完整的顶点，是"胎位信息"**。

```
  HS 输出两个东西：
    ├── 3 个控制点 HSCPOutput[3]  →  原始三角形的 3 个顶点坐标（object-space）
    └── 细分因子 HSPatchOutput    →  边各切几段、内部切几段

  硬件 Tessellator：
    根据细分因子，算出"我要在三角形里生成 27 个新顶点"
    但它不生成最终的 3D 位置！

    它生成的每个"顶点"只是一个浮点数坐标：
      domain = (0.3, 0.2, 0.5)  ←  这个顶点在原始三角形里的"胎位"
      （三个数之和恒为 1.0，表示它离三个控制点各有多远）

  然后对每个这样的胎位，调用一次 DS
```

Tessellator 切割三角形、**但不决定新顶点的 3D 坐标**。它只决定**新顶点的"胎位"**——这个新顶点在原始三角形三个控制点之间的什么位置。

---

## 第二层：DS 接受什么输入、输出什么？

**DS 的输入不是"一个三角形"，而是"一个胎位 + 原三角形的三个控制点"**。

```hlsl
VSOutput DS(
    HSPatchOutput patchConstant,                // ① 细分因子（这里基本不用）
    float3 domain : SV_DomainLocation,           // ② ★ 胎位坐标
    const OutputPatch<HSCPOutput, 3> patch)      // ③ ★ 原始三角形的 3 个控制点
```

**每调用一次 DS，输入一个胎位 + 三个控制点，输出一个完整顶点**。

---

## 第三层：DS 里的"重心插值"是什么？

这和你后面问的"GS 生成重心坐标"是**完全不同的两件事**，虽然都叫"重心坐标"。

### DS 的重心插值（Barycentric Interpolation）

**目的**：从 3 个控制点 + 胎位，算出新顶点的 3D 位置。

```hlsl
// domain.xyz 就是胎位坐标。三个值加起来 = 1.0
// domain.x = 到顶点 0 的权重
// domain.y = 到顶点 1 的权重
// domain.z = 到顶点 2 的权重

float3 objPos = patch[0].vertex.xyz * domain.x     // 顶点0 × 权重0
              + patch[1].vertex.xyz * domain.y     // 顶点1 × 权重1
              + patch[2].vertex.xyz * domain.z;    // 顶点2 × 权重2
```

**图解**：

```
  patch[0] (控制点0)
      ●
     /＼
    /  ＼
   / ★  ＼        ← ★ 是新顶点的胎位 domain=(0.3, 0.5, 0.2)
  /      ＼          它的 3D 位置 = 0.3×P0 + 0.5×P1 + 0.2×P2
 ●────────●
patch[1]  patch[2]
```

这个插值出来的位置是**在原始三角形表面上的**——这就是水面网格的平面位置（还没有波浪位移！）。

### GS 生成的重心坐标（Barycentric Coordinates for Output Triangle）

**目的**：标记输出三角形的三个角各自是谁（纯标识，不是计算位置用的）。

```hlsl
// GS 里对每个输出三角形顶点贴的标签：
float2 bCentrics[3] = { float2(1,0), float2(0,1), float2(0,0) };
```

```
  顶点0 = (1,0) — "我是第一条边的起点"
  顶点1 = (0,1) — "我是第二条边的起点"
  顶点2 = (0,0) — "我是第三个角"
```

经过光栅化器插值后，三角形内部的每个像素会有一个 `(α, β, γ)` 值，表示"这个像素离三条边各有多远"——用来画线框。

### 对比

| | DS 的 domain 重心坐标 | GS 的 barycentric 重心坐标 |
|---|---|---|
| 来源 | 硬件 Tessellator 生成 | GS 手动赋值 |
| 作用对象 | 原始 patch 的 3 个控制点 | 最终输出三角形的 3 个顶点 |
| 用途 | **计算新顶点 3D 位置** | 标记"你是哪个角"给 PS 用 |
| DS 代码中 | `domain.xyz` — 插值出 `objPos` | 不存在 |
| GS 代码中 | 不存在 | `bCentrics[3]` |

**它们是两种不同语义的"重心坐标"，只是都满足三个分量之和 = 1。**

---

## 第四层：位移采样是在 DS 里面直接采样 SRV 吗？

**是的，完全正确。**

```hlsl
// Ocean_DS.hlsl，line 30-32
float3 displacement;
float  foamAccum, waveHeight0;
SampleDisplacement(worldPosBase, displacement, foamAccum, waveHeight0);
//                    ↑
//           这个是重心插值出来的平面位置（还没加波浪）
```

`SampleDisplacement()` 内部：

```hlsl
for (int i = 0; i < 4; ++i)
{
    float2 uv_i = worldPosBase.xz * tiles[i];
    float4 disp = g_DisplacementSRVs[i].SampleLevel(g_Sampler, uv_i, 0);
    //                 ↑
    //          直接采样 t0~t3 的位移纹理 SRV！
    displacement += disp.rgb * layerContributes[i];
}
```

**DS 直接在 GPU 上采样纹理**。这一点和 PS 采样纹理没有区别——DS 也是着色器，有权访问所有绑定的 SRV。这就是为什么 `OceanEffect::Apply()` 里需要用 `DSSetShaderResources(0, 4, dispSRVs)` 把位移纹理绑给 DS。

---

## 第五层：输入 DS 的三角形是细分前的还是细分后的？

**细分前的——就是 IA 传入的那 3 个原始控制点。**

```
  IA 输入: 64×64 网格的顶点
           每 3 个一组 = 4096/3 ≈ 1365 个原始三角形

  VS → HS 的 3 个控制点就是这三个

  Tessellator 在原始三角形内部切出 N 个新顶点
           每个新顶点附带一个"胎位" domain

  DS 被调用 N 次，每次都拿到：
     输入: 同一个原始三角形的 3 个控制点
     输入: 这个新顶点的唯一胎位
     输出: 1 个完整的最终顶点（含波浪位移 + 裁剪坐标）

  所以 DS 输入的不是"新三角形"——
  新三角形要在 DS 输出之后，
  由 GS（或光栅化器）重新组装才知道。
```

DS 的视角里没有"新三角形"这个概念。它做的事是：**一个胎位进来 → 插值 + 位移 + 变换 → 一个顶点出去**。三角形的拓扑关系（哪三个顶点组成一个面）由光栅化器在后面处理。

---

## 完整数据流总结（一个原始三角形被细分的过程）

```
  原始三角形 (patch[0], patch[1], patch[2])  ← 来自 IA/VS
         │
         ├─→ HS 面片常量函数: 看 3 个点 → 算 edgeTess=4 → 输出给 Tessellator
         │
         ├─→ HS 控制点函数: 透传 3 个控制点 → HSCPOutput[3] → 给 DS
         │
         ▼
  ╔══════════════════════════════════════════════════════════╗
  ║  Tessellator (硬件)                                      ║
  ║  生成 27 个"胎位"，每个是一个 (x, y, z) 重心坐标          ║
  ║  domain[0] = (0.0, 0.0, 1.0)   ← 顶点0本身              ║
  ║  domain[1] = (0.0, 0.25, 0.75) ← 边上的新点             ║
  ║  domain[2] = (0.0, 0.5, 0.5)                            ║
  ║  ...                                                     ║
  ║  domain[26] = (0.33, 0.33, 0.34) ← 三角形内部的新点     ║
  ╚══════════════════════════════════════════════════════════╝
         │
         │  27 个胎位，每个触发一次 DS 调用
         │  每次 DS 都拿到同样的 3 个控制点
         ▼
  ╔══════════════════════════════════════════════════════════╗
  ║  DS 调用 × 27 次                                         ║
  ║                                                          ║
  ║  第 k 次:                                                ║
  ║    1. objPos = patch[0]×domain.x                        ║
  ║              + patch[1]×domain.y  ← 重心插值             ║
  ║              + patch[2]×domain.z                         ║
  ║                                                          ║
  ║    2. worldPosBase = mul(objPos, g_World)              ║
  ║                                                          ║
  ║    3. SampleDisplacement(worldPosBase) ← 采样位移 SRV    ║
  ║                                                          ║
  ║    4. 深度衰减                                          ║
  ║                                                          ║
  ║    5. worldPos = worldPosBase + displacement            ║
  ║       clipPos  = mul(worldPos, g_ViewProj)              ║
  ║                                                          ║
  ║    6. 输出 VSOutput (1 个完整顶点)                       ║
  ╚══════════════════════════════════════════════════════════╝
         │
         │  27 个 VSOutput 顶点输出
         │  光栅化器自动将它们重新组合成小三角形
         ▼
  ╔══════════════════════════════════════════════════════════╗
  ║  GS (可选) — 每个输出三角形 3 个顶点                     ║
  ║    透传数据 + 贴重心坐标标签 (1,0)(0,1)(0,0)             ║
  ╚══════════════════════════════════════════════════════════╝
         │
         ▼
  ╔══════════════════════════════════════════════════════════╗
  ║  光栅化器 → PS                                           ║
  ╚══════════════════════════════════════════════════════════╝
```

---

**一句话总结你问的 5 个问题的答案：**

1. Tessellator 产出了"胎位坐标"（domain），**不是**完整顶点
2. DS 的"重心插值"是用胎位坐标从 3 个控制点算出新顶点的 3D 位置
3. GS 的"重心坐标"是给输出三角形贴的角标，**和 DS 的重心插值是完全不同的东西**
4. 位移采样**是**在 DS 里直接采样 SRV — DS 有权访问纹理
5. DS 输入的是**原始三角形的 3 个控制点 + 一个胎位**，输出一个完整顶点

---

### `[2026-06-24 15:00]` **Q8**

**用户**：

> 现在还需要再详细介绍一下PS阶段的东西，就是BRDF等等，各部分各自实现了怎么样的视觉效果？

---

### `[2026-06-24 15:00]` **A8**

**Claude**：

PS 的最终颜色由 6 层视觉成分叠加而成。每一层控制一个独立的视觉特征——关掉某一层，水面就缺了一个"像水"的理由。

---

## 总公式

```
最终颜色 = (1-F)×散射 + 高光 + F×反射 + 泡沫 + 雾
            ──────   ──   ────   ──   ──
              ③      ②     ④     ⑤    ⑥

其中 F = Fresnel ①（决定 ③ 和 ④ 怎么分权重）
```

**Fresnel ① 不是颜色，是分配开关**——它决定这个像素偏向"看透水体"还是"看天空倒影"。

---

## ① Fresnel — 分配开关

```hlsl
float F0 = (1.33-1)²/(1.33+1)² ≈ 0.02;           // 水的垂直反射率
float fresnel = F0 + (1-F0) × pow(1 - N·V, 5);
// 垂直看: N·V≈1  → fresnel≈0.02 → 几乎全透明 → 散射主导
// 45°看:  N·V≈0.7→ fresnel≈0.15 → 散射为主 + 少量反射
// 掠射看: N·V≈0  → fresnel≈1.0  → 几乎全反射 → 镜像主导
```

**视觉效果**：站在船边低头看脚边的水 → 清澈蓝绿（散射）；抬头看远处海平线 → 天空的倒影（反射）。Fresnel 让这个过渡物理正确。

**如果关掉 Fresnel（强制 fresnel=0）**：水面永远不会像镜子，即使你看海平线也是蓝绿色的——不像真实的大面积水体。

**如果关掉 Fresnel（强制 fresnel=1）**：水面永远是完美的镜子，你看不到任何水的颜色——这不是水面，是水银面。

---

## ② 高光 Specular — 太阳的亮斑

```hlsl
// Beckmann 法线分布 — 粗糙度控制高光集中度
float Beckmann = exp((N·H²-1) / (roughness² × N·H²)) / (π × roughness² × N·H⁴);

// Smith 几何遮蔽 — 微表面互相遮挡
float geometryMask = 1.0 / (1.0 + viewMask + lightMask);

// 最终高光
float3 specular = fresnel × geometryMask × Beckmann × lightColor;
specular /= 4.0 × max(0.001, macroNormal·L);  // 归一化
specular ×= N·L × shadow;
```

**每部分干什么**：

| 子项 | 视觉作用 |
|------|---------|
| **Beckmann** | 控制高光斑的**大小和锐度**。roughness=0.05 → 高光斑又小又亮（光滑水面）；roughness=0.5 → 高光散开成暗淡的大片（粗糙水面） |
| **Smith 几何遮蔽** | 掠射角时微表面互相遮挡，高光减弱。没有它，掠射角的水面会不自然地闪亮 |
| **macroNormal·L 分母** | 防止用宏观法线（朝上）算出来的高光过曝 |

**视觉效果**：太阳在水面上的白色亮斑。近看是一个个小闪光，远看是一条亮带。

**如果关掉**：水面失去所有光泽感，变成哑光的蓝绿色平面——即使在阳光下也不闪。

---

## ③ 次表面散射 — 水体的蓝绿色（水为什么是蓝绿色的）

这是 PS 中最长的一段代码，也是水面"像水"的最核心原因。

```hlsl
float var_H = max(0.0f, input.waveHeight);  // 这个像素的波高
float k1 = _WavePeakScatterStrength × var_H × pow(L·(-V), 4) × pow(0.5-0.5×(L·N), 3);
float k2 = _ScatterStrength × pow(V·N, 2);
float k3 = _ScatterShadowStrength × N·L;
float k4 = _AmbientDensity;

float3 scatter = (k1×_ScatterPeakColor + k2×_ScatterColor) × lightColor × rcp(1+lightMask) × (...)
               + k3 × _ScatterColor × lightColor × (...)
               + k4 × ambientColor;
```

**四项各自的视觉贡献**：

### k1 — 波峰散射

```
  _WavePeakScatterStrength × 波高 × pow(L和V同向的程度, 4)
```

**视觉**：浪尖处看到**最亮、最饱和的蓝绿色**。浪尖是光穿透最深的地方，散射最强的位置。

**关掉 k1**：浪尖和浪谷颜色一样 → 波浪失去立体感，看起来像纸片而不是体积。

### k2 — 视线散射

```
  _ScatterStrength × pow(你垂直看水面的程度, 2)
```

**视觉**：垂直往下看 → 水最透明（光线路径最短）。掠射看水面 → 水更浑浊（光线斜穿更长的水体距离）。

**关掉 k2**：水面透明度不随视角变化，看起来像不透明的油漆而不是水。

### k3 — 光源散射

```
  _ScatterShadowStrength × 光照强度
```

**视觉**：太阳光越正面照在水面上，散射越多 → 白天水面亮，黄昏水面暗。

**关掉 k3**：水面亮度不随光照方向变化，失去昼夜感。

### k4 — 环境散射

```
  _AmbientDensity × 环境光颜色
```

**视觉**：在完全阴影里（没有直接阳光），水面依然有一层**暗蓝绿色的底色**——不是纯黑的。这就是为什么阴天海面还是有颜色的。

**关掉 k4**：阴影区域的水面是纯黑的 → 不真实（实际海洋即使在阴天也能看到蓝绿色）。

---

**为什么是蓝绿色？**

`_ScatterColor = (0, 0.67, 1.0)` — 蓝色分量 = 1.0，绿色 = 0.67，红色 = 0。这是因为水分子对红光的吸收率远高于蓝绿光。白光（RGB 平衡）进入水后红光最先被吸收，剩下蓝绿光被散射回来。这就是为什么**深海是蓝色的**。

---

## ④ 环境反射 — 天空倒影

```hlsl
float3 reflectDir = reflect(-viewDir, N);     // 视线在法线方向的镜面反射
float3 envReflect = g_Skybox.Sample(reflectDir) × _EnvironmentLightStrength;
envReflect *= saturate(shadow + _ShadowIntensity + 0.5);
```

**视觉**：水面倒映天空盒的颜色。Fresnel 控制它有多明显——近处几乎看不到倒影，远处水面像镜子。

**如果关掉**：水面变成完全不反射的漫反射表面 → 像蓝绿色的布，不是水。

---

## ⑤ 泡沫

```hlsl
// 泡沫来源：
//   位移纹理的 .a 通道 (CS 端 Jacobian 计算: 浪尖 → 泡沫)
//   + 边缘泡沫 (场景深度比较: 物体贴近水面 → 泡沫)
float foam = lerp(0, saturate(input.foam), pow(clipDepth, _FoamDepthAttenuation));
// 边缘泡沫加成
foam = foam + intersect × pow(foam, _EdgeFoamPower);
foam *= saturate(shadow + _ShadowIntensity);

// 混合：白色泡沫覆盖在水体颜色之上
color = lerp(color, _FoamColor × lightColor, saturate(foam));
```

**两个泡沫来源**：

| 来源 | 触发条件 | 视觉效果 |
|------|---------|---------|
| 浪尖泡沫（CS端） | Jacobian > 阈值（波面自交/挤压） | 白浪花在浪尖处 |
| 边缘泡沫（PS端） | 场景物体深度 ≈ 水面深度 | 球体与水面交界处白色泡沫环 |

**如果关掉泡沫**：水面失去所有白色纹理 → 太平滑太干净，不像真实海洋。

---

## ⑥ 雾效 — 大气散射

```hlsl
float fogFactor = saturate(pow(1.0f - exp(-viewDepth × _FogDensity), _FogPower));
color = lerp(color, _FogColor, fogFactor);
```

**视觉**：远处水面逐渐融合到雾的颜色（淡蓝灰色），这是空气中水汽/微粒散射的结果。

- **`_FogDensity`（0.01）**：控制雾的浓度。`exp(-500×0.01) ≈ 0.007` → 500m 外几乎全是雾色
- **`_FogPower`（2.0）**：控制过渡曲线。>1 表示雾在远处突然出现
- **`_FogColor`（淡蓝灰）**：雾的色相 = 水天线融合色

**如果关掉雾**：远处水面和近处水面颜色完全一样 → 没有深度感，所有距离的水一样蓝。

---

## 最终合成顺序

```
  散射 (蓝绿色水体)
    ↓ ×(1-fresnel)
  + 高光 (白色太阳亮斑)
    ↓
  + 反射 (天空倒影) × fresnel
    ↓
  = 水面基底颜色
    ↓
  叠加泡沫 (白色浪花)
    ↓
  叠加雾 (远距离大气色)
    ↓
  = 最终像素颜色
```

**为什么是这个顺序？** 这个顺序模拟的是光在到达你眼睛之前经过的物理过程：先穿透水体散射变蓝 → 表面反光 → 泡沫在表面之上 → 你和水面之间的空气散射（雾）。

---

## 视觉对照表：关掉某个成分后的效果

| 关掉的成分 | 水面变什么样 | 为什么不真实 |
|-----------|-------------|-------------|
| Fresnel | 全角度同样外观 | 失去"近清远镜"的过渡 |
| Specular | 哑光水面 | 没有闪光，阴天感 |
| 散射 (全部k1~k4) | 黑色的水 | 没有蓝绿色，只有反射和白色高光 |
| k1（波峰散射） | 波浪没有体积感 | 浪尖浪谷无区分 |
| k2（视线散射） | 透明度不随视角变化 | 不像立体水体 |
| k4（环境散射） | 阴影区全黑 | 真实海洋阴影区依然有暗蓝绿色 |
| 环境反射 | 不反射天空 | 没有倒影 |
| 泡沫 | 太干净 | 真海洋到处是白沫 |
| 雾 | 无景深感 | 远近颜色完全一样 |

---

### `[2026-06-24 15:30]` **Q9**

**用户**：

> 所以BRDF是什么？

---

### `[2026-06-24 15:30]` **A9**

**Claude**：

BRDF = **Bidirectional Reflectance Distribution Function** = 双向反射分布函数。

拆开来看：

- **双向** = 两个方向：光来的方向（L）和你看的方向（V）
- **反射** = 光从表面弹回来，不是穿透进去
- **分布函数** = 一个数学函数，告诉你反射光怎么"分布"到各个方向

**一句话：BRDF 是一个函数，输入是"光从哪来 + 你从哪看"，输出是"你能看到多少反射光"。**

---

## 为什么需要 BRDF？

你可以把 BRDF 理解为**表面的"指纹"**——不同材质有不同的 BRDF：

```
  完美镜面 (镜子):         粗糙表面 (水泥):
      L  V                     L
       \│                      │
        ●  —— 光只弹到        ●  —— 光四散开
        │    V 方向            │\
                               V  (各个方向都能看到一点)
  
  BRDF: 只在 V = reflect(L)  |  BRDF: V 在各个方向都差不多
         处非零               |
```

我们这个项目的 BRDF 用的是 **Beckmann 微表面模型**。

---

## Beckmann 微表面模型的三个组件

在我们的 PS 代码中，BRDF 由三个子函数相乘得到：

```
BRDF(L, V) = D × G × F

   D = Beckmann NDF    —— 表面上微镜面朝向分布
   G = Smith 几何遮蔽   —— 微镜面之间互相遮挡
   F = Fresnel         —— 取决于入射角反射多少光
```

### D — Normal Distribution Function（法线分布函数）

**作用**：描述"表面上有多少微镜面的法线方向和半程向量 H 一致"。

```hlsl
float Beckmann(nDotH, roughness) {
    return exp((nDotH²-1) / (roughness² × nDotH²)) / (π × roughness² × nDotH⁴);
}
```

```
  roughness = 0.05 (光滑)        roughness = 0.5 (粗糙)
  
    微镜面朝向分布:                微镜面朝向分布:
        │                            │
        │    ╱╲                      │   ╱  ╲
        │   ╱  ╲                     │  ╱    ╲
        │  ╱    ╲                    │ ╱      ╲
        └──────────                  └────────────
         集中在法线附近               分散到各个方向
         
    结果：高光又小又亮            结果：高光又大又暗
```

这就是为什么 `_Roughness` 从 0.05 调到 0.5 时，水面从"镜面感"变成"磨砂感"。

### G — Geometry Masking（几何遮蔽）

**作用**：微镜面互相遮挡——有些微镜面虽然在正确朝向，但被邻居挡住了。

```hlsl
float SmithMaskBeckmann(H, otherDir, roughness) {
    float a = dot(H, otherDir) / (roughness × sqrt(1 - dot²));
    // 近似公式: 掠射角 → 大量微镜面被遮挡 → 反射减弱
}
float geometryMask = 1.0 / (1.0 + viewMask + lightMask);
```

```
  正常视角:                      掠射视角:
    光                             光  ↙
     ↓                              ↓↓↓
  ╱╲╱╲╱╲                        ╱╲╱╲╱╲
  │微表面│  每个微镜面           │████│  前排挡住后排
  │ 都可见│  都能反射            │微表面│  反射大幅度减弱
```

没有 G 项 → 掠射角的水面会异常闪亮（不自然）。加了 G → 掠射角反射按物理规律减弱。

### F — Fresnel（菲涅尔）

**作用**：光在介质界面有多少被反射、多少穿透。

已在 Q8 详细讲过，简言之：

- 垂直看 → 2% 反射，98% 穿透（看到水里的颜色）
- 掠射看 → 接近 100% 反射（水面变镜子）

---

## 三个组件怎么组成 Specular

```hlsl
// D × G × F = Beckmann 微表面 BRDF
float3 specular = fresnel(F)           // F: 多少光参与反射（vs 穿透）
                × geometryMask(G)     // G: 微表面互相遮挡了多少
                × Beckmann(D, N·H)   // D: 微表面分布
                × lightColor;         // 光源颜色

specular /= 4.0 × (macroNormal·L);    // 归一化（BRDF 的标准分母）
specular ×= N·L;                      // 光照角度衰减
```

**最终视觉**：太阳在水面上的白色亮斑（高光），粗糙度决定它是锐利的小点还是散开的大片。

---

## BRDF vs 散射：一个反射、一个穿透

这是最容易混淆的地方：

| | BRDF (Specular) | 散射 (Subsurface Scattering) |
|---|---|---|
| 光去了哪里 | 从表面**弹回来** | **穿透表面**进入水体 |
| 产生什么 | 白色高光亮斑 | 蓝绿色水体颜色 |
| 依赖粗糙度吗 | 是（Beckmann 的 roughness） | 不直接（是 k1~k4 的经验项） |
| 是 BRDF 吗 | **是** | **不是** — 散射是 BSSRDF（多了"次表面"） |
| 代码中 | `Beckmann()` + `SmithMaskBeckmann()` | `k1 + k2 + k3 + k4` |

BRDF 只管表面那层皮怎么反光。散射管光线进去之后在里面弹跳再出来的过程。两者加在一起才是一个完整的水面材质。

---

**一句话：BRDF = 表面的指纹，描述"光打到这个表面会怎么弹"。我们的 BRDF = Beckmann(D) × Smith(G) × Fresnel(F)。**

---

### `[2026-06-24 16:00]` **Q10**

**用户**：

> 对于着色器的各个阶段，是不是他们的常量缓冲区等(b0、t0、s0、u0)都是各自独立的，需要每个每一个阶段单独管理吗？

---

### `[2026-06-24 16:00]` **A10**

**Claude**：

**是的，每个 Shader Stage 拥有自己独立的资源槽位。** 这是 D3D11 资源绑定模型的核心设计。

---

## 每个 Stage 有自己的一套"抽屉"

把 GPU 的每个 Shader Stage 想象成一个独立的工位，每个工位有自己的抽屉柜：

```
  VS 工位                    HS 工位                    DS 工位
  ┌──────────┐              ┌──────────┐              ┌──────────┐
  │ b0 b1 b2 │              │ b0 b1 b2 │              │ b0 b1 b2 │  ← 常量缓冲区抽屉
  │ t0 t1 t2 │              │ t0 t1 t2 │              │ t0 t1 t2 │  ← 纹理抽屉
  │ s0 s1    │              │ s0 s1    │              │ s0 s1    │  ← 采样器抽屉
  └──────────┘              └──────────┘              └──────────┘

  PS 工位                    
  ┌──────────┐              
  │ b0 b1 b2 │              
  │ t0 t1 t2 │              
  │ s0 s1    │              
  └──────────┘              
```

**每个 Stage 的 b0 是完全独立的不同槽位。VS 的 b0 ≠ HS 的 b0 ≠ DS 的 b0 ≠ PS 的 b0。**

---

## D3D11 API 的命名体现

每个 Stage 都有自己专属的 Set 函数：

```
  VSSetConstantBuffers(slot, count, buffers)    → 只设置 VS 的 cbuffer
  HSSetConstantBuffers(slot, count, buffers)    → 只设置 HS 的 cbuffer
  DSSetConstantBuffers(slot, count, buffers)    → 只设置 DS 的 cbuffer
  GSSetConstantBuffers(slot, count, buffers)    → 只设置 GS 的 cbuffer
  PSSetConstantBuffers(slot, count, buffers)    → 只设置 PS 的 cbuffer
  CSSetConstantBuffers(slot, count, buffers)    → 只设置 CS 的 cbuffer
```

同理纹理（ShaderResources）和采样器（Samplers）也有各自独立的 Set 函数。

**没有"全局"绑定**——不存在一个 API 调用能同时把 cbuffer 绑到所有 Stage。

---

## 这直接解释了 OceanEffect::Apply() 里"看起来冗余"的代码

现在再看 `Apply()` 中的绑定代码，每一段的存在理由就很清楚了：

### 为什么 b1/b2 要分别绑 4 次？

```cpp
ID3D11Buffer* cbs[2] = { m_pCBOceanParams, m_pCBChangeRarely };
deviceContext->VSSetConstantBuffers(1, 2, cbs);  // VS 的 b1,b2
deviceContext->HSSetConstantBuffers(1, 2, cbs);  // HS 的 b1,b2
deviceContext->DSSetConstantBuffers(1, 2, cbs);  // DS 的 b1,b2
deviceContext->PSSetConstantBuffers(1, 2, cbs);  // PS 的 b1,b2
```

**每行只影响一个 Stage。4 行 = 4 个 Stage 各自获得相同数据。** 没有捷径，必须一个一个绑。

### 为什么位移纹理要绑给 3 个 Stage？

```cpp
deviceContext->DSSetShaderResources(0, 4, dispSRVs);  // DS 能用位移纹理
deviceContext->VSSetShaderResources(0, 4, dispSRVs);  // VS 也能用
deviceContext->PSSetShaderResources(0, 4, dispSRVs);  // PS 也能用
```

**DS 采样位移纹理来变形顶点，PS 采样位移纹理来读泡沫值。** 不同 Stage 从同一纹理中读不同的通道，但必须各自绑定。

### 为什么斜率纹理只绑给 PS（和 DS 可选）？

```cpp
deviceContext->PSSetShaderResources(4, 4, slopeSRVs);  // PS 需要法线重建
// DS 可选绑（不绑也不影响——DS 不读斜率）
```

DS 不读斜率纹理，所以 DS 的 t4~t7 槽位可以是空的。**每个 Stage 的纹理槽位可以绑不同的内容——完全独立。**

### 为什么采样器 s0 要手动绑给 HS/DS？

```cpp
// 框架的 Pass::Apply() 只绑了 VS 和 PS 的采样器！
// HS/DS 的采样器抽屉是空的——需要手动填
ID3D11SamplerState* sampler = RenderStates::SSLinearWrap.Get();
deviceContext->HSSetSamplers(0, 1, &sampler);  // 往 HS 的 s0 抽屉放采样器
deviceContext->DSSetSamplers(0, 1, &sampler);  // 往 DS 的 s0 抽屉放采样器
```

---

## 为什么 D3D11 这样设计？

**好处**：精细控制——可以为不同 Stage 绑定完全不同的资源。

```
  例：VS 的 t0 = 位移纹理，PS 的 t0 = 完全不同的纹理
      HS 的 b0 = 144B CBOceanPerFrame（手动管理）
      PS 的 b0 = 框架管理的 b0（可能更大，含更多数据）
```

这对我们这个项目至关重要：**HS/DS 的 b0 只需要 144B（World+ViewProj+EyePosW），PS 的 b0 由框架管（可能包含不同的数据布局）。** 如果所有 Stage 共享一个槽位，这种灵活性就不可能了。

**代价**：每个 Stage 要单独绑定，代码看起来冗余。

---

## 一个特殊情况：HLSL 的 register 声明是"每个 Stage 各自的命名空间"

```hlsl
// Ocean_Common.hlsli 中：
cbuffer CBOceanPerFrame : register(b0) { ... }   // 这个 b0 在每个 Stage 中独立解析
Texture2D g_DisplacementSRVs[4] : register(t0);   // 同理，每个 Stage 各自有 t0~t3
```

`register(b0)` 的意思是"我占用**当前编译目标 Stage** 的 b0 槽位"。同一个 `.hlsli` 文件被 VS、HS、DS、PS 分别 include 时，每个 Stage 编译出来都会占用各自的 b0——它们不冲突，因为物理上是不同的硬件寄存器。

---

## 对比：Unity 怎么简化这件事

Unity 的 Shader Lab 替开发者藏掉了这些细节：

```hlsl
// Unity: 你不需要知道哪个 Stage 绑了什么
Material.SetTexture("_DisplacementTexture", tex);  // 引擎自动绑到所有需要的 Stage
Material.SetMatrix("g_World", mat);                // 同上
```

Unity 在背后做了我们 `Apply()` 里 7 步手动绑定的等价操作。裸 DX11 暴露了这些底层细节——但理解了之后你会更清楚地知道 GPU 管线到底是怎么工作的。

---

**一句话：D3D11 的每个 Shader Stage 有自己完全独立的资源槽位。b0 有 6 个版本（VS/HS/DS/GS/PS/CS），每个都要单独绑定。**

---

### `[2026-06-24 16:30]` **Q11**

**用户**：

> 我还注意到一个东西，明明并不是每一个着色器阶段都会用到三个常量缓冲区的所有数据，但是为什么还是给每一个阶段都绑上了三个常量缓冲区，是有什么考虑吗？

---

### `[2026-06-24 16:30]` **A11**

**Claude**：

好眼力。答案分两层：**第一层是"确实有需要"**，**第二层是"框架的锅"**。

---

## 第一层：其实各个 Stage 用到 b1/b2 比你想象的更多

先盘点每个 Stage 到底用了哪些 cbuffer：

```
         b0(World/VP/Eye)    b1(渲染参数)          b2(光源)
  VS      ✗ 不需要            ✗ 不需要              ✗ 不需要
  HS      ✓ 需要(World,Eye)   ✓ 需要(_TessEdgeLength, _TessFar)   ✗
  DS      ✓ 需要(全部三项)     ✓ 需要(tile,Height,衰减)             ✗
  GS      ✗ 不需要            ✗ 不需要              ✗ 不需要
  PS      ✓ 需要(框架版b0)     ✓ 需要(大部分参数)    ✓ 需要(灯光)
```

**HS 确实需要 b1！** `TessellationHeuristic()` 函数里用了 `_TessEdgeLength`，`CalcHSPatchConstants()` 里用了 `_TessFar`——这两个都在 b1 里。很多人会忽略这一点。

**DS 确实需要 b1！** `SampleDisplacement()` 里用了 `_Tile0~3`、`_LayerContribute0~3`、`_HeightStrength`，以及 `_DisplaceDepthAttenuation`——全在 b1 里。

所以真正"绑了但没用到"的组合只有：
- VS 不需要 b1/b2（但被绑了）
- GS 不需要 b1/b2（但被绑了）
- HS/DS 不需要 b2（但被绑了）

---

## 第二层：为什么要多绑——框架的 Pass::Apply() 埋的坑

这才是主要原因。

**框架的 `Pass::Apply()` 会清空并重新绑定所有 cbuffer。** 它做的事大致是：

```
  Pass::Apply() 内部:
    1. 读取 VS 和 PS 的 Shader Reflection → 发现 b0, b1, b2 三个 cbuffer
    2. 对每个 Stage (VS, HS, DS, GS, PS)，绑定框架的版本：
       - b0: 框架管理的数据（World, ViewProj, EyePosW...）
       - b1: 框架创建的"空缓冲区"（大小正确但数据全是 0）
       - b2: 框架创建的"空缓冲区"（大小正确但数据全是 0）
```

关键：**框架把 b1、b2 的空版本绑到了全部 5 个 Stage。**

所以如果不做第 5 步的覆盖：

```cpp
// 第 5 步：覆盖 b1/b2 到全部 stage
ID3D11Buffer* cbs[2] = { m_pCBOceanParams, m_pCBChangeRarely };
deviceContext->VSSetConstantBuffers(1, 2, cbs);  // 覆盖 VS
deviceContext->HSSetConstantBuffers(1, 2, cbs);  // 覆盖 HS
deviceContext->DSSetConstantBuffers(1, 2, cbs);  // 覆盖 DS
deviceContext->PSSetConstantBuffers(1, 2, cbs);  // 覆盖 PS
```

那么：
- **HS 的 b1** = 框架的空版本（全是 0）→ `_TessEdgeLength=0` → 细分计算全部出错
- **DS 的 b1** = 框架的空版本（全是 0）→ `_Tile*=0` → 位移为 0 → 水面全平
- **PS 的 b1** = 空版本 → 所有渲染参数为 0 → 画面全黑
- **PS 的 b2** = 空版本 → 灯光数据为 0 → 没有任何光照

---

## 为什么不全 Stage 都绑 b0 呢？

b0 的情况不同——框架的 b0 和我们的手动 b0 有**不同的数据布局**：

```
  框架的 b0 (VS/PS):  可能包含 World, ViewProj, EyePosW, + 框架额外数据
  手动的 b0 (HS/DS):  只有 World(64B) + ViewProj(64B) + EyePosW(12B) + pad(4B) = 144B
```

HS/DS 不需要框架的额外数据，只需要 144B 的核心变换数据。所以 b0 不能用"全绑同一份"的策略——HS/DS 用手动版，VS/PS 用框架版。

---

## 为什么不精确绑定——只绑给需要的 Stage？

技术上完全可以：

```cpp
// 精确绑定：只给需要的 Stage
deviceContext->HSSetConstantBuffers(1, 2, cbs);  // HS 需要 b1 (确有必要)
deviceContext->DSSetConstantBuffers(1, 2, cbs);  // DS 需要 b1 (确有必要)
deviceContext->PSSetConstantBuffers(1, 2, cbs);  // PS 需要 b1+b2 (确有必要)
// VS、GS 不需要 → 不绑
```

但这样做有两个问题：

1. **框架已经给 VS/GS 绑了空版本**——不覆盖的话，VS/GS 的 b1/b2 槽位里是空数据。虽然 VS/GS 不读这些数据，但万一以后改了 Shader、某个 Stage 开始读了，调试起来很痛苦
2. **代码简单性**——4 行代码 vs 3 行代码 + 解释"为什么跳过了 VS 和 GS"的注释。对于一份学习项目来说，显式绑给所有 Stage 更清晰，也避免了"我怎么知道哪个绑了哪个没绑"的问题

---

## 总结

绑了但没用的组合（VS/b1、VS/b2、GS/b1、GS/b2、HS/b2、DS/b2）不是设计意图——它们是**框架把空版本扩散到所有 Stage 后，我们用正确数据覆盖回去时的"附带绑定的"**。

真正的需求矩阵其实只有：

```
        b0     b1     b2
  VS     -      -      -
  HS   手动     ✓      -
  DS   手动     ✓      -
  GS     -      -      -
  PS   框架     ✓      ✓
```

但框架机制 + 代码简洁性 → 实际绑成了全矩阵。

---

**一句话：HS 和 DS 确实需要 b1（只是你不一定注意到），框架的 Pass::Apply() 把空 b1/b2 扩散到所有 Stage，我们用正确数据覆盖回去时顺便绑给了不需要的 Stage——无害、简单、比精确绑定少出 bug。**

---

