# AI 生成代码标注与学习指引

> 本文档标注了哪些代码是 AI 在 2026-06-16~17 对话中直接写入的，
> 哪些是你原有的。按管线顺序组织，方便逐行 Review。

---

## 一、管线总览

```
CS 端（你写的）：InitializeSpectrum → PackConjugate → UpdateSpectrum → HorizIFFT → VertIFFT → AssembleTextures
                                                                                          ↓
渲染端（AI 写的新管线）：VS(透传) → HS(细分因子) → DS(位移+变换) → PS(光照)
                                          ↑ AI 重写        ↑ AI 重写
渲染端 C++：OceanEffect::Apply 调度（AI 重构） + GameApp 参数填充（AI 修）
```

---

## 二、Shader 文件（HLSL）

### 2.1 `Shaders/Ocean_Common.hlsli` — 部分修改

| 行 | 作者 | 说明 |
|----|------|------|
| 1-63 | 原有 | include、三个 cbuffer 声明、纹理/Sampler、采样器 |
| cbuffer b0 `g_Pad[1]` → `g_Pad` | **AI** | 数组改标量，修 HLSL cbuffer 16B 对齐 bug |
| cbuffer b1 数组拆分 | 你+**AI** | `_Tile[4]`→`_Tile0~3` 等，你发现 bug → AI 执行 |
| 65-81 | **AI** | VSInput / HSInput / HSCPOutput / HSPatchOutput / VSOutput 结构体 |
| 83-100 | **AI** | `SampleDisplacement()` 共享函数 |

**Review 重点：**
- 为什么 HLSL cbuffer 中 `float arr[4]` 占 64B 而不是 16B？（每元素 16B 对齐）
- HS/DS 结构体为什么需要独立定义？（VS→HS→DS 每阶段有各自的输入输出）

### 2.2 `Shaders/Ocean_VS.hlsl` — **AI 完全重写**

原来是"VS 中采样位移纹理 + 变换到裁剪空间"的非曲面细分版。
现在改为**纯透传**：`VSInput → HSInput`，匹配 Unity 的 `vert()` 函数。

**Review 重点：**
- 为什么曲面细分管线下 VS 只需要透传？（位移采样移到 DS 中做）
- 对比旧版 VS 理解管线拆分逻辑

### 2.3 `Shaders/Ocean_HS.hlsl` — **AI 完全重写**

实现距离基曲面细分因子计算：
- `TessellationHeuristic()` — 匹配 Unity 同名函数
- `CalcHSPatchConstants()` — 匹配 Unity `PatchFunction`
- HS 主函数 — 透传控制点

**Review 重点：**
- `[domain("tri")]` `[partitioning("fractional_odd")]` 等 attribute 含义
- `SV_TessFactor` / `SV_InsideTessFactor` 的作用
- 为什么用 `distance(edgeCenter, g_EyePosW)` 控制细分程度？
- `_TessEdgeLength` / `_TessFar` 参数如何影响细分密度

### 2.4 `Shaders/Ocean_DS.hlsl` — **AI 完全重写**

域着色器，是曲面细分管线的核心：
- 重心坐标插值 → 世界空间 → 采样位移 → 深度衰减 → 裁剪空间

**Review 重点：**
- 对比 Unity 的 `vp()` 函数，理解位移采样+深度衰减的完整流程
- `clipPosBase.w` 为什么是线性深度？（透视投影后 w = -viewZ）
- `clipDepth = 1 - linearDepth` 的逻辑（匹配 Unity `1 - Linear01Depth`）

### 2.5 `Shaders/Ocean_PS.hlsl` — 部分修改

| 区域 | 作者 | 说明 |
|------|------|------|
| 法线重建 (行20-35) | 原有 | slope 纹理采样 + mesoNormal |
| Beckmann BRDF (行37-57) | 原有 | 高光反射 |
| 散射 (行59-75) | 原有+**AI**修 | `var_H` 从 `* _HeightStrength` 改回纯物理波高 |
| 环境反射 (行77-81) | 原有 | skybox cubemap |
| 泡沫 (行83-86) | 原有 | 深度衰减泡沫 |
| 合成+雾 (行88-96) | 原有 | fresnel 混合 + 雾 |

**Review 重点：**
- Beckmann BRDF 与 Fresnel 公式的物理含义
- 散射 k1/k2/k3/k4 四项分别代表什么
- `_HeightStrength` 在 VS/DS 和 PS 中的不同角色

### 2.6 `Shaders/FFT_Ocean_CS_Common.hlsli` — 微调

| 修改 | 作者 |
|------|------|
| `_Padding[2]` → `_Pad0; _Pad1;` | **AI** |
| 添加 `_Speed` | 你 |

### 2.7 CS 文件（`*_CS.hlsl`） — 你写的，未动

- `InitializeSpectrum_CS.hlsl`
- `PackSpectrumConjugate_CS.hlsl`
- `UpdateSpectrum_CS.hlsl`
- `HorizontalIFFT_CS.hlsl`
- `VerticalIFFT_CS.hlsl`
- `AssembleTextures_CS.hlsl`

---

## 三、C++ 文件

### 3.1 `OceanEffect.cpp` — **AI 重构 Apply + 新增 HS/DS b0**

| 区域 | 作者 | 说明 |
|------|------|------|
| Impl 结构体 | 原有+**AI** | `CBOceanPerFrame` 是 AI 加的（HS/DS 专用 b0） |
| `InitAll()` | 原有+**AI** | 新增 HS/DS b0 缓冲区创建 |
| `SetRenderDefault()` | **AI**修 | 根据 `m_TessellationEnabled` 切换拓扑 |
| `SetEyePos()` | **AI**修 | 同时存 `m_EyePos`（给手动 b0） |
| `Apply()` | **AI**重构 | 管线调度流程完全重排 |
| Setter 函数 | 原有 | SetTiles 等 AI 适配了数组→独立变量 |
| `GetInputData()` | 原有 | 未改 |

**Apply() 流程（AI 设计）：**
```
1. 上传 b1/b2 数据
2. Pass::Apply → 框架管理 VS/PS 的 b0/shader/sampler
3. 手动上传 b0 → HS/DS（框架不管理这两个 stage）
4. 手动绑定 s0 → HS/DS
5. 覆盖 b1/b2 → 全部 stage（框架版本是空的）
6. 手动设置 HS/DS shader
7. 手动设置 SRV → 全部 stage
```

**Review 重点：**
- 为什么 HS/DS 的 b0 要手动管理？（框架的 EffectPass 不含 HS/DS）
- `VSGetConstantBuffers` / `DSSetConstantBuffers` 的 stage 隔离概念
- `D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST` vs `TRIANGLELIST`

### 3.2 `GameApp.h` — 微调

| 修改 | 作者 |
|------|------|
| CB_FFTOceanParams: `_Padding[2]`→`_Pad0;_Pad1;` + `_Speed` | **AI**+你 |
| CB_OceanEffectParams: 数组拆分 + `static_assert` | **AI**+你 |

### 3.3 `GameApp.cpp` — 微调

| 修改 | 作者 |
|------|------|
| `InitOceanEffectParams()` 数组→独立变量 | **AI** |
| `FillOceanEffectParams()` 数组→独立变量 | **AI** |
| `_TessFar = 500.0f`（原来 1.0 → 无细分） | **AI** |
| CS 调度函数 (Dispatch*) | 你写的 |
| 纹理/UAV/SRV 创建 | 你写的 |
| JONSWAP 参数填充 | 你写的 |

---

## 四、推荐学习路径

### 第 1 步：理解计算着色器端（你写的，先巩固）

1. **FFT_Ocean_CS_Common.hlsli** — 公用函数/结构体
   - `JONSWAP()` 函数：波浪频谱公式（参考 oceanographic 文献）
   - `hash()` / `gaussian()`：随机数生成
   - `Dispersion()`：深水色散关系 ω² = gk

2. **InitializeSpectrum_CS.hlsl** → **UpdateSpectrum_CS.hlsl**
   - 理解从 JONSWAP 频谱 → 初始化复振幅 H0 → 时间演进
   - 理解 IFFT 之前的频域数据流

3. **HorizontalIFFT_CS.hlsl + VerticalIFFT_CS.hlsl**
   - Cooley-Tukey 蝶形 IFFT 算法
   - groupshared memory 的使用

4. **AssembleTextures_CS.hlsl**
   - 频域数据 → 空间域位移 + 斜率 + 泡沫

5. **GameApp.cpp Dispatch* 函数**
   - 理解每帧 CS 调度顺序
   - UAV/SRV 绑定与解绑

### 第 2 步：理解渲染着色器端（AI 写的，重点 Review）

1. **Ocean_Common.hlsli** — 先读懂结构体链
   ```
   VSInput → HSInput → HSCPOutput → DS(插值) → VSOutput → PS
   ```

2. **Ocean_VS.hlsl** — 最简单的透传（5 行代码）

3. **Ocean_HS.hlsl** — 曲面细分因子计算
   - 关键：`TessellationHeuristic()` 怎样用距离控制细分
   - 对比 Unity 原版 `FFTOcean_Shader.shader` 的 HS 部分

4. **Ocean_DS.hlsl** — 核心位移逻辑（对照 Unity 的 `vp()`）
   - 重心坐标插值
   - `SampleDisplacement()` 共享函数
   - 深度衰减（`clipDepth` 的线性化修正）
   - 世界空间位移应用

5. **Ocean_PS.hlsl** — 光照模型（对照 Unity 的 `frag()`）
   - 法线重建（slope → normal）
   - Beckmann BRDF + Fresnel
   - 次表面散射（4 项）
   - 环境反射 + 泡沫 + 雾

### 第 3 步：理解 C++ 调度端

1. **OceanEffect.cpp → Apply()** — 逐行理解管线状态设置顺序
2. **GameApp.cpp → DrawScene()** — 理解每帧渲染调用链
3. **EffectHelper.cpp**（Common 框架）— 理解框架的 Pass 管理和 cbuffer 机制

---

## 五、关键 Unity 对照

Unity 原文件位置：
- `FFTOcean_Compute.compute` — CS 端
- `FFTOcean_Shader.shader` — 渲染端（VS→HS→DS→GS→PS）
- `FFTOcean_Script.cs` — C# 调度

对照阅读建议：
- Unity `vert()` → 我们的 `Ocean_VS.hlsl`
- Unity `hull()` + `PatchFunction()` → 我们的 `Ocean_HS.hlsl`
- Unity `domain()` → 我们的 `Ocean_DS.hlsl`（注意：Unity 的 domain 调用了 `vp()`，位移在 `vp()` 里）
- Unity `frag()` → 我们的 `Ocean_PS.hlsl`
