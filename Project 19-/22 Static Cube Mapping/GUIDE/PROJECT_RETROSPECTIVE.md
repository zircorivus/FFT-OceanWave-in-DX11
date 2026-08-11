# FFT Ocean Wave — 项目架构复盘

> 基于 DirectX 11 + HLSL，复刻 Unity FFTOcean 项目
> 2026-06-22 ~ 2026-06-24

---

## 一、项目一句话描述

**在 DirectX 11 中实现基于 FFT 的实时海洋波浪模拟**，包含完整的计算着色器频谱管线（6 个 CS）和曲面细分渲染管线（VS → HS → Tessellator → DS → GS → PS），对标 Unity 参考项目 `FFTOcean_Shader.shader` / `FFTOcean_Compute.compute`。

---

## 二、整体架构：两大子系统

```
┌─────────────────────────────────────────────────────────────────┐
│                        每帧更新循环                              │
│                                                                 │
│  ┌──────────────────────────┐    ┌──────────────────────────┐  │
│  │   计算着色器子系统 (CS)    │    │   渲染着色器子系统 (RS)    │  │
│  │   物理波浪模拟             │    │   视觉效果呈现             │  │
│  │                          │    │                          │  │
│  │  JONSWAP 频谱             │    │  VS — 顶点透传           │  │
│  │       ↓                  │    │  HS — 距离基曲面细分      │  │
│  │  InitializeSpectrum       │    │  Tessellator — 硬件切分   │  │
│  │       ↓                  │    │  DS — 位移采样 + 变换     │  │
│  │  PackConjugate            │    │  GS — 重心坐标生成        │  │
│  │       ↓                  │    │  PS — 光照 + 散射 + 泡沫  │  │
│  │  UpdateSpectrum           │    │                          │  │
│  │       ↓                  │    │                          │  │
│  │  HorizontalIFFT           │    │                          │  │
│  │       ↓                  │    │                          │  │
│  │  VerticalIFFT             │    │                          │  │
│  │       ↓                  │    │                          │  │
│  │  AssembleTextures         │    │                          │  │
│  │       ↓                  │    │                          │  │
│  │  位移纹理 + 斜率纹理 ──────┼───→│  采样位移纹理生成波浪形状    │
│  └──────────────────────────┘    └──────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### CS 子系统职责：生产数据

6 个 Compute Shader 在 GPU 上以 1024×1024 线程并行执行，将 8 组 JONSWAP 频谱通过 512 次蝶形 IFFT 变换到空间域，输出：
- 4 张位移纹理（t0~t3）：`RGBA = (Δx, Δy, Δz, foam)`，描述水面每点的三维偏移
- 4 张斜率纹理（t4~t7）：`RG = (∂y/∂x, ∂y/∂z)`，描述水面每点的倾斜
- 1 张变化掩码（t8）：法线细节混合权重
- 1 张浮力数据（u6）：第 0 层波高，给 C# 浮力系统用（未接入）

### RS 子系统职责：呈现画面

将 CS 输出的位移纹理采样后应用到 256m×256m 的平面网格上，通过曲面细分在近处动态生成密集顶点，经光照模型计算每个像素的颜色。

**两个子系统之间唯一的联系是位移纹理和斜率纹理**——CS 写入 → RS 读取，完全解耦。

---

## 三、数据流全景

```
  ╔═══════════════════════════════════════════════════════════════════╗
  ║                     CPU 端：参数初始化                             ║
  ╠═══════════════════════════════════════════════════════════════════╣
  ║                                                                   ║
  ║  GameApp::InitResource()                                         ║
  ║    │                                                              ║
  ║    ├── 8 组 JONSWAP 参数 (FillJonswapStruct)                      ║
  ║    │     scale, windSpeed, fetch, swell, ...                     ║
  ║    │     → StructuredBuffer → CS b0 的 t0                         ║
  ║    │                                                              ║
  ║    ├── CS 常量缓冲区 m_CBFFTOceanParams (96B)                     ║
  ║    │     Depth, Gravity, RepeatTime, Seed, Foam*, Speed          ║
  ║    │     → b0 → 6 个 Compute Shader                              ║
  ║    │                                                              ║
  ║    ├── 渲染常量缓冲区 m_pCB_OceanEffectParams (192B)              ║
  ║    │     Tile*, LayerContribute*, HeightStrength,                 ║
  ║    │     Roughness, Scatter*, FoamColor, Fog*, Tess*             ║
  ║    │     → b1 → VS/HS/DS/PS (OceanEffect::Apply 上传)            ║
  ║    │                                                              ║
  ║    ├── 光源常量缓冲区 (1200B)                                     ║
  ║    │     DirLight[5] + PointLight[5] + SpotLight[5]              ║
  ║    │     → b2 → VS/HS/DS/PS                                      ║
  ║    │                                                              ║
  ║    └── 纹理：1024² × 4 InitSpectrum, × 8 Spectrum,               ║
  ║               × 4 Displacement, × 4 Slope,                       ║
  ║               × 1 VariationMask, × 1 BuoyancyData               ║
  ║                                                                   ║
  ╚═══════════════════════════════════════════════════════════════════╝
                              │
                              ▼
  ╔═══════════════════════════════════════════════════════════════════╗
  ║                GPU 端：每帧更新 (UpdateScene)                      ║
  ╠═══════════════════════════════════════════════════════════════════╣
  ║                                                                   ║
  ║  _FrameTime += dt × 0.5                                          ║
  ║                                                                   ║
  ║  遍历 4 层 (step=0..3):                                           ║
  ║                                                                   ║
  ║  ┌── DispatchInitializeSpectrum_CS(step) ───────────────────┐    ║
  ║  │   每次 128×128 线程                                       │    ║
  ║  │   输入: JONSWAP 参数[i×2], [i×2+1]                       │    ║
  ║  │   计算: JONSWAP(ω) × DirectionSpectrum(θ) × ShortWaveFade │    ║
  ║  │       × Gaussian random → H0 (复振幅)                    │    ║
  ║  │   输出: InitSpectrum[i] ← (H0.r, H0.i, 0, 0)             │    ║
  ║  └──────────────────────────────────────────────────────────┘    ║
  ║                              ↓                                    ║
  ║  ┌── DispatchPackSpectrumConjugate_CS(step) ───────────────┐    ║
  ║  │   读取 H0, 计算共轭 H0*                                  │    ║
  ║  │   输出: InitSpectrum[i] ← (H0.r, H0.i, H0*.r, -H0*.i)   │    ║
  ║  └──────────────────────────────────────────────────────────┘    ║
  ║                              ↓                                    ║
  ║  ┌── DispatchUpdateSpectrum_CS(step) ──────────────────────┐    ║
  ║  │   时间演进: h(t) = h0·e^(iωt) + h0*·e^(-iωt)           │    ║
  ║  │   计算位移梯度: ∂D/∂x, ∂D/∂y, ∂D/∂z                     │    ║
  ║  │   输出: Spectrum[step×2]   ← 位移频域数据                 │    ║
  ║  │         Spectrum[step×2+1] ← 斜率频域数据                 │    ║
  ║  └──────────────────────────────────────────────────────────┘    ║
  ║                                                                   ║
  ║  遍历 8 层 (step=0..7):                                           ║
  ║  ┌── DispatchHorizontalIFFT_CS(step) ──────────────────────┐    ║
  ║  │   Cooley-Tukey 蝶形 IFFT (1024 点, 10 级)               │    ║
  ║  │   groupshared memory ping-pong                           │    ║
  ║  │   Spectrum[step] ← IFFT_row(Spectrum[step])             │    ║
  ║  └──────────────────────────────────────────────────────────┘    ║
  ║  ┌── DispatchVerticalIFFT_CS(step) ────────────────────────┐    ║
  ║  │   同上，但按列读取 (id.yx) —— 实现矩阵转置              │    ║
  ║  │   Spectrum[step] ← IFFT_col(Spectrum[step])             │    ║
  ║  └──────────────────────────────────────────────────────────┘    ║
  ║                                                                   ║
  ║  遍历 4 层 (step=0..3):                                           ║
  ║  ┌── DispatchAssembleTextures_CS(step) ────────────────────┐    ║
  ║  │   Permute 重排 IFFT 乱序数据                              │    ║
  ║  │   计算 Jacobian → 泡沫                                    │    ║
  ║  │   输出: Displacement[i] ← (Δx, Δy, Δz, foam)            │    ║
  ║  │         Slope[i]        ← (∂y/∂x, ∂y/∂z)                │    ║
  ║  │         VariationMask    ← 变化细节遮罩                    │    ║
  ║  └──────────────────────────────────────────────────────────┘    ║
  ║                                                                   ║
  ╚═══════════════════════════════════════════════════════════════════╝
                              │
                              ▼
  ╔═══════════════════════════════════════════════════════════════════╗
  ║              GPU 端：渲染 (DrawScene)                              ║
  ╠═══════════════════════════════════════════════════════════════════╣
  ║                                                                   ║
  ║  1. Clear RT + Depth                                              ║
  ║  2. 绘制球体 (BasicEffect, 写入主深度)                             ║
  ║  3. CopySubresourceRegion 拷贝深度 → 场景深度纹理 (边缘泡沫用)     ║
  ║  4. 绘制水面 (OceanEffect)                                        ║
  ║  5. 绘制天空盒 (SkyboxEffect)                                     ║
  ║                                                                   ║
  ║  ┌─── 水面渲染管线 (曲面细分开启时) ───────────────────────┐     ║
  ║  │                                                           │     ║
  ║  │  IA ──→ VS ──→ HS ──→ [Tessellator] ──→ DS ──→ GS ──→ PS  │     ║
  ║  │  │       │      │          │            │      │      │    │     ║
  ║  │  │  64×64  │   透传   CalcPatch-  硬件切分  位移采样  透传  光照  │     ║
  ║  │  │  网格   │   object Constants   三角形    变换裁剪  重心  着色  │     ║
  ║  │  │  顶点   │   space  (1次/△)    →新顶点   (+深度衰减) 坐标  (BRDF  │     ║
  ║  │  │        │   →HS   (3次/△)    (N次调用)  (N次/△)  (1次/△) 散射  │     ║
  ║  │  │        │                                              泡沫)  │     ║
  ║  └──────────────────────────────────────────────────────────┘     ║
  ║                                                                   ║
  ╚═══════════════════════════════════════════════════════════════════╝
```

---

## 四、曲面细分管线设计逻辑

### 为什么用曲面细分？

水面是 256m×256m 的大平面，只有 64×64 = 4096 个原始顶点。不用细分：
- 近处一个三角形边长 4m → 能看到明显的直线边缘
- 如果想在近处看到波浪细节，需要数十万顶点 → VRAM 爆炸

**曲面细分 = LOD 的硬件加速实现**：近处自动切成密集顶点，远处保持稀疏。

### 各阶段职责与设计意图

| 阶段 | 职责 | 为什么这样设计 |
|------|------|---------------|
| **VS** | 纯透传 object-space 数据 | 不在这里做变换——细分器需要原始坐标来生成新顶点 |
| **HS CalcPatchConstants** | 距离基细分因子计算 | `edgeLength × screenHeight / (_TessEdgeLength × pow(viewDist, 1.2))` — 近密远疏的数学公式 |
| **HS control point** | 纯透传 3 个控制点 | 不做任何处理，把数据原样交给 DS |
| **Tessellator** | 硬件自动切分三角形 | 固定功能硬件，无 HLSL 代码——靠 `PATCHLIST_3` 拓扑 + `HSSetShader` + `DSSetShader` 自动激活 |
| **DS** | 1. 重心插值 → 2. 位移采样 → 3. 深度衰减 → 4. MVP 变换 | 管线的核心：把平坦平面变成波浪形状，近处波浪大，远处衰减至平 |
| **GS** | 透传所有数据 + 生成重心坐标 | 匹配 Unity 架构；为未来线框模式预留；教育完整性 |
| **PS** | 法线重建 → BRDF → 散射 → 反射 → 泡沫 → 雾 | 完整的水面光照模型，每项对应一个视觉特征 |

---

## 五、Unity → DX11 适配：受限于框架的"曲线救国"决策

> **面试价值最高的部分**——以下每一项都是因为 DX11 框架限制或与 Unity 的差异，
> 被迫深入理解底层机制后做出的针对性解决方案。

---

### 决策 1：HLSL cbuffer 数组 16B 对齐 — 整个项目最隐蔽的 Bug

**Unity 怎么做**：`Material.SetFloat("_Tile0", 0.04f)` — 直接按名字 set，Unity 内部处理布局。

**DX11 的问题**：HLSL 常量缓冲区有自己的打包规则——**数组的每个元素按 float4（16 字节）对齐**。

```hlsl
// 你以为的布局：
float _Tile[4];  // 4 × 4 = 16 字节
// GPU 实际布局：
float _Tile[0]; 16B  // 有效数据 4B + 填充 12B
float _Tile[1]; 16B  // 同上
float _Tile[2]; 16B
float _Tile[3]; 16B  // 总计 64 字节！
```

**结果**：C++ 端按 192B 创建 `CBOceanParams`，GPU 期望 320B → **RenderDoc 里 b1 标红**，整个水面没有任何参数输入，一片混乱。

**解决方案**：将所有 cbuffer 中的数组拆成独立 float 标量：

```hlsl
float _Tile0; float _Tile1; float _Tile2; float _Tile3;  // 4×4=16B，紧凑排列
```

在 Shader 中需要数组的地方用局部变量重建：`float tiles[4] = {_Tile0, _Tile1, _Tile2, _Tile3};`

**涉及范围**：`Ocean_Common.hlsli`（b1 全部数组）、`FFT_Ocean_CS_Common.hlsli`（CS b0）、`GameApp.h`（C++ struct + `static_assert`）、`GameApp.cpp`（填充代码）、`OceanEffect.cpp`（C++ struct）。

**思考深度**：这个问题不查硬件文档根本无法定位——C++ 端 `sizeof` 是对的，HLSL 语法也没报错，只有 RenderDoc 看到 b1 红色 + 水面全黑。最终是根据 DX11 `packoffset` 规则推算出 16B 对齐才是根因。

---

### 决策 2：HS/DS 的 b0 完全手动管理 — 框架不支持曲面细分 Stage

**Unity 怎么做**：`Material.SetMatrix("g_World", ...)` — 所有 Shader Stage 自动共享。

**DX11 框架的限制**：`EffectHelper::AddEffectPass` 创建时只填了 `nameVS="OceanVS"` 和 `namePS="OceanPS"`。框架内部的 cbuffer 管理逻辑只遍历 VS 和 PS 的 Shader Reflection，**HS/DS 完全不在框架管辖范围内**。

**解决方案**：
1. 单独为 HS/DS 手动创建 144B 的 `CBOceanPerFrame` 缓冲区（只含 `g_World` + `g_ViewProj` + `g_EyePosW`）
2. 在 `Apply()` 中用 `Map/Unmap` 手动上传
3. 用 `HSSetConstantBuffers(0,1,...)` / `DSSetConstantBuffers(0,1,...)` 手动绑定到 b0
4. VS/PS 的 b0 仍由框架的 `Pass::Apply()` 管理（保持框架一致性）
5. HS/DS 的采样器（s0）同样手动绑定

**为什么不是统一全手动？** 用户要求 VS/PS 的 b0 仍由框架管，保留框架的设计模式。结果就是 b0 有两个版本——框架版（VS/PS）和手动版（HS/DS），内容相同但缓冲区对象不同。

---

### 决策 3：4 张独立 Texture2D 替代 Unity 的 Texture2DArray

**Unity 怎么做**：

```hlsl
UNITY_DECLARE_TEX2DARRAY(_DisplacementTexture);  // 4 层在一个纹理数组里
displacement = UNITY_SAMPLE_TEX2DARRAY_LOD(_DisplacementTexture, float3(uv, layer), 0);
```

**DX11 的限制**：项目的纹理创建函数 `CreateTextureUAVSRV` 只支持单张 `Texture2D`。虽然 D3D11 完全支持 Texture2DArray，但改造框架的纹理创建函数 + UAV/SRV 绑定逻辑需要动大量通用代码。

**解决方案**：用 4 张独立的 `Texture2D`（`DXGI_FORMAT_R32G32B32A32_FLOAT`，1024² 各一张），在 Shader 中声明为纹理数组：

```hlsl
Texture2D g_DisplacementSRVs[4] : register(t0);  // t0, t1, t2, t3 各一张
```

代价：CS 端需要 `for (int i=0; i<4; i++)` 循环分 4 次 Dispatch（而不是一次处理整个数组），C++ 端需要 4 组 UAV/SRV 分别管理。但换来了零框架改动。

---

### 决策 4：深度缓冲拷贝实现边缘泡沫 — D3D11 不能同时绑定 DSV 和 SRV

**Unity 怎么做**：内置的 `_CameraDepthNormalsTexture` 由引擎在渲染水面之前自动生成，直接采样即可。

**DX11 的限制**：D3D11 不允许同一个纹理子资源**同时**绑定为深度模板视图（DSV）和着色器资源视图（SRV）。球体渲染到主深度缓冲后，水面渲染时主深度缓冲仍然是 DSV（深度测试开启），所以不能同时把它当 SRV 采样。

**解决方案**：
1. 球体渲染到主深度缓冲后，在 DrawScene 中水面渲染前插入 `CopyDepthBeforeOcean()`
2. 用 `CopySubresourceRegion` 将主深度缓冲（`R24G8_TYPELESS`）拷贝到一张独立的 `m_pSceneDepthCopyTex`（同格式）
3. 拷贝纹理只绑定 SRV，不绑定 DSV → 水面 PS 中 t10 槽位自由采样
4. 代价：每帧一次 GPU 拷贝（同格式同尺寸，`CopySubresourceRegion` 纯硬件 DMA，几乎零开销）

**额外处理**：NDC 深度比较。Unity 的 `screenDepth` 和 `viewDepth` 都在归一化线性深度空间。我们的主深度缓冲存储的是 D24 UNORM = NDC 深度。为此在 DS 中额外输出 `ndcDepth = clipPos.z / clipPos.w`，使两个深度值处于同一空间（均为 NDC [0,1]），可直接比较。

---

### 决策 5：`clipPosBase.w` 替代 `Linear01Depth` — 理解透视投影的数学本质

**Unity 怎么做**：

```hlsl
float clipDepth = 1 - Linear01Depth(clipPos.z / clipPos.w);  // 引擎内置函数
```

**DX11 的问题**：`Linear01Depth` 是 Unity 内置函数，HLSL 没有等价物。代码最初用了 `clipPos.z / clipPos.w`（NDC 深度）做深度衰减。

**根因分析**：摄像机离水面 10m，远平面 1000m。NDC z 在透视投影下是 **1/z 非线性**的：
- 10m 处 NDC_z ≈ 0.802
- `1.0 - 0.802 = 0.198`
- `pow(0.198, 10) ≈ 0` → 位移被完全抹平！

**解决方案**：透视投影矩阵的数学性质——**变换后 w 分量 = -viewZ**（view-space 线性深度，单位米）。

```hlsl
float4 clipPosBase = mul(worldPosBase, g_ViewProj);
float linearDepth = saturate(clipPosBase.w / 1000.0f);  // 线性！0=近处, 1=远平面
float clipDepth   = 1.0f - linearDepth;                  // 翻转: 近处1, 远处0
float attenuation = pow(clipDepth, 10);                   // 远处快速衰减
```

**为什么这体现了思考深度**：这个问题不能靠试参数解决——`_DisplaceDepthAttenuation` 调成任何值都没用，因为问题出在输入数据（非线性 vs 线性）而不是参数值。需要理解透视投影矩阵第 4 行 `[0, 0, 1, 0]` 的构造原理。

---

### 决策 6：GS 目前是"基建" — 匹配 Unity 架构但承认暂未使用

**Unity 怎么做**：

```hlsl
#pragma geometry geo
[maxvertexcount(3)]
void geo(triangle v2g g[3], inout TriangleStream<g2f> stream) { ... }
```

**DX11 的实现**：完全复刻了 Unity 的 GS——透传所有插值数据 + 生成重心坐标 `(1,0) (0,1) (0,0)`。但当前 PS 中没有读取 `barycentric` 字段，重心坐标生成后被光栅化器丢弃。

**为什么还保留**：
- 没有 GS 时管线也能正常工作（DS → RS → PS），画面完全不受影响
- 但 Unity 有这个 Stage，保留是为了**架构完整性**（面试时可以说"完整实现了 VS→HS→Tess→DS→GS→PS 全链路"）
- GS 是管线中唯一能看到完整三角形（3 个顶点）的阶段——未来做逐三角形法线计算、动态剔除、Cubemap 多面渲染时，代码框架已经就位

---

### 决策 7：`PATCHLIST_3` 拓扑切换 — DX11 细分管线的"开关"

**Unity 怎么做**：引擎根据 Shader 中是否有 `#pragma hull` / `#pragma domain` 自动决定拓扑。

**DX11 的做法**：手动切换：

```cpp
m_CurrTopology = m_TessellationEnabled
    ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST  // 每 3 顶点 = 1 个 patch
    : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;               // 普通三角形
```

**关键理解**：硬件细分器不需要、也没有对应的 HLSL 调用语句。它靠**三个条件同时满足**自动激活：
1. 拓扑 = `PATCHLIST_3`（IA 层面）
2. `HSSetShader` 绑定了 HS（`Apply()` 中手动设置）
3. `DSSetShader` 绑定了 DS（`Apply()` 中手动设置）

三个条件缺一个 → 管线降级为 VS → PS 标准路径。这和光栅化器一样——你没有一行代码"调用光栅化器"，但 VS/PS 配好就自动跑了。

**Bug 历史**：拓扑冲突 — `Apply()` 设 `PATCHLIST_3`，但 `GetInputData()` 覆盖回 `TRIANGLELIST` → 细分器从未激活。修复：`SetRenderDefault` 根据 `m_TessellationEnabled` 设置正确的拓扑。

---

### 决策 8：雾密度单位换算 — Unity 归一化深度 vs DX11 世界单位

**Unity 怎么做**：

```hlsl
float viewDepth = -mul(UNITY_MATRIX_MV, vertex).z * _ProjectionParams.w;
// viewDepth ∈ [0, 1]（归一化到远平面）
float fog = pow(1 - exp(-viewDepth * _FogDensity), _FogPower);
// _FogDensity = 1.0 → 中等远处就有明显雾
```

**DX11 的不同**：

```hlsl
output.viewDepth = distance(worldPos, g_EyePosW);  // 世界空间距离，单位米！
// viewDepth ∈ [0, 1000]（米制）
```

**问题**：如果 `_FogDensity = 1.0`，`exp(-500 × 1.0) ≈ 0` → 500m 外全是雾，水面看不到。

**解决**：`_FogDensity` 从 Unity 的 1.0 调整为 0.005。物理直觉：`e^(-500 × 0.005) = e^(-2.5) ≈ 0.08`，500m 外 92% 雾，合理。

**为什么不是直接改 Shader**：保留 `viewDepth` 的世界单位是因为它在雾效之外的用处（PS 中没用到，但 DS 输出的 `viewDepth` 还用于逻辑一致性检查）。在参数端做单位换算，Shader 保持物理语义清晰。

---

### 统一主题

这些决策的共同模式是：**Unity 引擎替你做了很多"看不见"的事**——cbuffer 布局管理、Shader Stage 间的资源绑定、深度纹理的自动生成、线性深度的内置函数——当搬到裸 DX11 时，这些事必须自己理解底层机制后手动实现。每一个问题的解决都需要先理解"Unity 在背后做了什么"，再找到 DX11 中对应的硬件行为或 API。

---

## 六、关键文件速查

| 文件 | 角色 | 行数 |
|------|------|------|
| `GameApp.h` | 所有成员变量声明（纹理、buffer、CS 对象、JONSWAP 数据） | ~190 |
| `GameApp.cpp` | 初始化所有纹理/缓冲区/JONSWAP 参数 + 每帧 CS 调度 + 渲染调度 | ~900 |
| `OceanEffect.cpp` | 渲染管线的 CPU 端调度：InitAll 加载 Shader + Apply 绑定所有资源 | ~520 |
| `Effects.h` | OceanEffect 类声明（公开接口） | ~200 |

### Shader 文件依赖

```
Ocean_Common.hlsli ── #include "LightHelper.hlsli"
    │
    ├── Ocean_VS.hlsl
    ├── Ocean_HS.hlsl
    ├── Ocean_DS.hlsl
    ├── Ocean_GS.hlsl
    └── Ocean_PS.hlsl

FFT_Ocean_CS_Common.hlsli (独立，不与渲染端共享)
    │
    ├── InitializeSpectrum_CS.hlsl
    ├── PackSpectrumConjugate_CS.hlsl
    ├── UpdateSpectrum_CS.hlsl
    ├── HorizontalIFFT_CS.hlsl
    ├── VerticalIFFT_CS.hlsl
    └── AssembleTextures_CS.hlsl
```

### 常量缓冲区布局

```
  ┌──────────────────────────────────────────────────────────┐
  │  b0 (144B) — CBOceanPerFrame (HS/DS 手动)               │
  │    g_World(64) + g_ViewProj(64) + g_EyePosW(12) + pad(4)│
  ├──────────────────────────────────────────────────────────┤
  │  b1 (192B) — CBOceanParams (全部 stage)                  │
  │    Tile* + LayerContribute* + Height + Roughness +       │
  │    Scatter* + Foam* + Fog* + VarMask* + Tess* + Pad*    │
  ├──────────────────────────────────────────────────────────┤
  │  b2 (1200B) — CBOceanChangeRarely                        │
  │    DirLight[5] + PointLight[5] + SpotLight[5]           │
  └──────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────┐
  │  CS b0 (96B) — FFTOceanParams                            │
  │    Depth, Gravity, FrameTime, RepeatTime, CutOffs,       │
  │    Resolution, LengthScales, Seed, Foam*, Speed          │
  └──────────────────────────────────────────────────────────┘

纹理槽位:
  t0~t3:  位移纹理 (Displacement) → VS + DS + PS
  t4~t7:  斜率纹理 (Slope)        → DS + PS
  t8:     VariationMask           → PS
  t9:     天空盒 Cubemap           → PS
  t10:    场景深度纹理 (edge foam) → PS
  s0:     线性 Wrap 采样器         → DS + PS
```

---

## 七、光照模型公式速查

PS 中每个像素的最终颜色由以下成分按 Fresnel 权重混合：

```
  color = (1 - F) × scatter    ← 水体蓝绿色（次表面散射 4 项）
        + specular             ← 太阳高光（Beckmann BRDF）
        + F × envReflect       ← 天空倒影（Cubemap 采样）
        + foam                 ← 白色泡沫叠加
        + fog                  ← 大气雾效
```

```
  Fresnel (F):  垂直看≈0.02（水透明）, 掠射看≈1（水面镜面）
  Scatter (k1): 波峰散射 — 浪尖处光穿透最深
  Scatter (k2): 视线散射 — 垂直看水最透明
  Scatter (k3): 光源散射 — 正面照时散射多
  Scatter (k4): 环境散射 — 阴影下水的底色
```

---

## 八、未实现功能（扩展方向）

| 功能 | 说明 |
|------|------|
| 线框模式 | GS 已输出重心坐标，PS 加 `smoothstep(bary, 0.02)` 即可 |
| 阴影贴图 | 框架已有 `RSShadow` + `SSShadowPCF`，需加 Shadow Pass |
| 水下折射 | 需水面下场景的渲染结果 + 屏幕空间 UV 偏移 |
| ImGui 调参面板 | 当前参数硬编码在 `InitOceanEffectParams()`，可接 ImGui slider |
| 浮力系统 | `_BuoyancyData` 已生成，未接 C#/C++ 物体浮力 |

---

## 九、调参指南

| 想达到的效果 | 调哪个参数 | 方向 |
|-------------|-----------|------|
| 波浪更大更剧烈 | `_HeightStrength` | ↑ (1.0→2.0) |
| 水面更光滑/更粗糙 | `_Roughness` | ↓光滑 / ↑粗糙 |
| 水的颜色更蓝/更绿 | `_ScatterColor` | RGB 偏蓝(0,0.5,1) 或偏绿(0,1,0.5) |
| 泡沫更多 | `_FoamBias` / `_FoamAdd` | ↓Bias / ↑Add |
| 远处雾更浓 | `_FogDensity` | ↑ (0.01→0.05) |
| 反射更亮 | `_EnvironmentLightStrength` | ↑ (1.0→2.0) |
| 近处细分更密集 | `_TessEdgeLength` | ↓ (16→8) |
| 远处完全不做细分 | `_TessFar` | ↓ (500→200) |
| 波浪动画更快/更慢 | `_Speed` | ↑快 / ↓慢 |
| 改变波浪整体外观 | `JONSWAP.windSpeed/fetch/scale` | 不同风场参数 |

---

## 十、性能特征

- **CS 端**：6×4 + 2×8 = 40 次 Dispatch/帧，每次 128×128 线程 = 655K 线程/帧 → GPU 轻松应对
- **RS 端**：原始 4096 顶点 → 曲面细分后近处可达数万顶点 → DS 每顶点 4 次纹理采样 → 屏幕 1080p ≈ 2M 像素的光照计算
- **瓶颈**：PS 的 4 层位移+斜率采样（每像素 8 次 `SampleLevel`）+ 天空盒采样，是性能主要消耗点

---

> 文档版本 1.0 | 基于 2026-06-24 代码状态
