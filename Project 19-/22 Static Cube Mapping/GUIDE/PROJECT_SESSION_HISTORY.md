# Project 19- FFT Ocean Wave — AI 会话完整记录

> 最后更新: 2026-06-18
> 项目路径: `F:\...\Project 19-\22 Static Cube Mapping\`
> 目标: 将第22课 Static Cube Mapping 改造为 FFT 水面模拟，复刻 Unity 项目 FFTOcean

---

## 一、项目背景

- **原始项目**: DirectX11-With-Windows-SDK 教程系列，第22课为 Static Cube Mapping
- **改造目标**: 实现 FFT 海洋波浪模拟（复刻 Unity 项目 `C:\Users\aa\Downloads\FFT-Ocean-Code-main\`）
- **Unity 参考文件**: FFTOcean_Compute.compute / FFTOcean_Shader.shader / FFTOcean_Script.cs
- **用户已有基础**: 完成了计算着色器(6个CS)和CPU端调度逻辑，但渲染管线（VS/PS）无法正常工作
- **初始症状**: 水面一片死黑，RenderDoc 显示常量缓冲区 b1 标红

---

## 二、发现的全部 Bug 及修复（按时间顺序）

### Bug 1: PS 返回调试代码而非计算结果
- **文件**: `Ocean_PS.hlsl:94-95`
- **现象**: `return float4(input.waveHeight.xxxx) * 10.0;` 替代了真正的颜色输出
- **用户已知**: 调试用代码，AI 后恢复为 `return float4(color, 1.0f);`

### Bug 2: HLSL cbuffer 数组元素 16 字节对齐 — **核心 Bug**
- **发现者**: 用户（通过 RenderDoc 观察）
- **根因**: HLSL cbuffer 默认打包规则中，数组每个元素 16 字节对齐
  - `float _Tile[4]` = 4×16 = 64B（不是 16B）
  - `float _LayerContribute[4]` = 4×16 = 64B
  - `float _Padding[3]` = 3×16 = 48B
  - 多出 128 字节，使 CBOceanParams 从 192B 膨胀到 320B
- **C++ 给 192B，GPU 要 320B** → RenderDoc b1 标红
- **修复**: 将所有 cbuffer 中的数组拆成独立 float 变量
  - `float _Tile[4]` → `float _Tile0, _Tile1, _Tile2, _Tile3`
  - `float _LayerContribute[4]` → 同理
  - `float _Padding[3]` → `float _Pad0, _Pad1, _Pad2`
  - `float _Padding[2]` → `float _Pad0, _Pad1` (CS cbuffer)
  - `float g_Pad[1]` → `float g_Pad` (b0 cbuffer)
- **涉及文件**: Ocean_Common.hlsli, FFT_Ocean_CS_Common.hlsli, Ocean_VS.hlsl, Ocean_PS.hlsl, OceanEffect.cpp, GameApp.h, GameApp.cpp

### Bug 3: CreateBuffer 缺少 HR() 检查
- **文件**: `OceanEffect.cpp` InitAll
- **现象**: 缓冲区创建失败时静默忽略，导致 b1 为 nullptr
- **修复**: 添加 `HR()` 宏检查（后因策略调整被还原）

### Bug 4: GameApp.h CB_OceanEffectParams 缺少 _Padding
- **用户已自行修复**: 添加 `float _Pad[3]` 然后改为独立变量

### Bug 5: 深度线性化错误 — **位移被完全抹平**
- **文件**: `Ocean_DS.hlsl`（原 VS 也有相同问题）
- **根因**: `clipDepth = 1.0 - saturate(z/w)` 用了非线性 NDC 深度
  - 摄像机距水面 10m，远平面 1000m，NDC_z ≈ 0.802
  - clipDepth = 0.198，pow(0.198, 10) ≈ 0 → 位移全被衰减
- **Unity 正确做法**: `clipDepth = 1 - Linear01Depth(z/w)` → clipDepth ≈ 0.99
- **修复**: 改用 `clipPosBase.w`（视图空间线性深度，单位米）
  - `linearDepth = saturate(clipPosBase.w / 1000.0f)`
  - `clipDepth = 1.0 - linearDepth` ≈ 0.99
  - pow(0.99, 10) ≈ 0.90 → 90% 位移保留

### Bug 6: _FogDensity 单位不匹配
- **根因**: Unity viewDepth = z_view / farPlane（归一化到[0,1]）
  我们的 viewDepth = world-space distance（米制）
- Unity _FogDensity=1.0 对应归一化深度；米制下 1.0 导致 100% 雾
- **修复**: 用户自行调整为 `_FogDensity = 0.005`

### Bug 7: _TessFar = 1.0f 导致零曲面细分
- **文件**: `GameApp.cpp` InitOceanEffectParams
- **现象**: 所有三角形距摄像机 >1m 就不做细分 → 等于没开 Tess
- **修复**: `_TessFar = 500.0f`

### Bug 8: 曲面细分管线拓扑冲突
- **文件**: `OceanEffect.cpp`
- **现象**: Apply 设 PATCHLIST_3 → GetInputData 覆盖为 TRIANGLELIST
- **修复**: SetRenderDefault 根据 `m_TessellationEnabled` 设置正确拓扑

### Bug 9: HS/DS 缺少 b0 和采样器
- **根因**: 框架的 EffectPass 只管理 VS/PS 的 cbuffer/sampler，HS/DS 不在 Pass 中
- **修复**: 为 HS/DS 手动创建 b0 (144B CBOceanPerFrame) + 手动绑定采样器
- VS/PS 的 b0 仍由框架管理（用户要求）

---

## 三、创建的新文件

| 文件 | 说明 |
|------|------|
| `AI_GENERATED_CODE_GUIDE.md` | AI 生成代码标注 + 学习路径指引 |
| `PROJECT_SESSION_HISTORY.md` | 本文件，会话完整历史 |

---

## 四、重写的文件（AI 完全生成）

| 文件 | 说明 |
|------|------|
| `Shaders/Ocean_VS.hlsl` | 纯透传 VS→HS，匹配 Unity `vert()` |
| `Shaders/Ocean_HS.hlsl` | 距离基曲面细分因子，匹配 Unity `hull()` + `PatchFunction()` |
| `Shaders/Ocean_DS.hlsl` | 重心插值→位移采样→裁剪变换，匹配 Unity `domain()` + `vp()` |

---

## 五、修改的现有文件

| 文件 | 改动摘要 |
|------|---------|
| `Shaders/Ocean_Common.hlsli` | cbuffer 数组拆分 + 添加 HS/DS 结构体 + SampleDisplacement() 共享函数 |
| `Shaders/Ocean_PS.hlsl` | 恢复颜色输出 + var_H 物理波高 + Fog 适配 |
| `Shaders/FFT_Ocean_CS_Common.hlsli` | _Padding[2] → _Pad0/_Pad1（用户后来加了 _Speed） |
| `OceanEffect.cpp` | Apply() 重构 + HS/DS b0/s0 手动管理 + SetRenderDefault 拓扑 + SetEyePos 双存 |
| `GameApp.h` | CB_FFTOceanParams/_Pad 拆分 + _Speed + static_assert |
| `GameApp.cpp` | InitOceanEffectParams 数组拆分 + FillOceanEffectParams 数组拆分 + _TessFar=500 |

---

## 六、最终管线架构

```
=== Compute Shader 端（用户写）===
InitializeSpectrum → PackConjugate → UpdateSpectrum → HorizIFFT → VertIFFT → AssembleTextures
     ↓                    ↓               ↓               ↓            ↓            ↓
  4张InitSpectrum   4张InitSpectrum   8张Spectrum    8张Spectrum  8张Spectrum  4张Displacement
                                        +VariationMask                          4张Slope
                                                                               +VariationMask

=== 渲染 Shader 端（AI 写的曲面细分管线）===
VS(Ocean_VS)          → 纯透传 object-space 数据
HS(Ocean_HS)          → 距离基细分因子（近处细，远处粗）
DS(Ocean_DS)          → 重心插值 + SampleDisplacement() + 线性深度衰减 + 裁剪变换
PS(Ocean_PS)          → 法线重建 + Beckmann BRDF + 散射 + 天空盒反射 + 泡沫 + 雾

=== C++ 调度端 ===
OceanEffect::Apply():
  1. 上传 b1(OceanParams) + b2(ChangeRarely)
  2. Pass::Apply() → 框架管理 VS/PS b0/shader/sampler
  3. 手动上传 b0(PerFrame) → HS/DS（144B，保证正确）
  4. 手动绑定 s0 → HS/DS
  5. 覆盖 b1/b2 → VS/HS/DS/PS（框架版本是空的）
  6. 手动设置 HS/DS shader
  7. 手动设置 SRV(t0-t9) → 全部 stage
```

---

## 七、关键设计决策

1. **VS/PS b0 由框架管理，HS/DS b0 手动管理**
   - 框架的 EffectPass 不包含 HS/DS，无法自动管理
   - 手动创建 144B CBOceanPerFrame 缓冲区，保证与 HLSL 布局一致

2. **深度衰减用线性 view-space 深度**
   - 匹配 Unity `Linear01Depth()`，避免 NDC 非线性导致位移全被抹平

3. **数组拆分为独立变量**
   - HLSL cbuffer 数组元素 16B 对齐是 D3D11 的硬性规则
   - VS/PS 中通过局部数组 `float tiles[4] = {_Tile0, _Tile1, _Tile2, _Tile3}` 恢复

4. **曲面细分默认开启**
   - `m_TessellationEnabled = true`，拓扑自动切换 PATCHLIST_3

---

## 八、Compute Shader 常量缓冲区布局（最终版）

```
FFTOceanParams : register(b0) = 96 bytes
  _Depth(4) _Gravity(4) _FrameTime(4) _RepeatTime(4)
  _LowCutOff(4) _HighCutOff(4) _WaveSharpX(4) _WaveSharpY(4)
  _Resolution(4) _LengthScale0(4) _LengthScale1(4) _LengthScale2(4) _LengthScale3(4)
  _Seed(4) _FoamBias(4) _FoamPower(4) _FoamAdd(4) _FoamDecayRate(4)
  _InitialSpectrumIndex(4) _SpectrumIndex(4) _DisplacementIndex(4) _SlopeIndex(4)
  _Speed(4) _Pad1(4)
```

## 九、渲染常量缓冲区布局（最终版）

```
CBOceanUpdatePerFrame : register(b0) = 144 bytes
  g_World(64) g_ViewProj(64) g_EyePosW(12) g_Pad(4)

CBOceanParams : register(b1) = 192 bytes
  _Tile0~3(16) _LayerContribute0~3(16)
  _HeightStrength~_TessFar(84) _Pad0~2(12)
  _ScatterColor(16) _ScatterPeakColor(16) _FoamColor(16) _FogColor(16)

CBOceanChangeRarely : register(b2) = 1200 bytes
  g_DirLight[5](320) g_PointLight[5](400) g_SpotLight[5](480)
```

---

## 十、关键参数默认值

| 参数 | 值 | 位置 |
|------|-----|------|
| _HeightStrength | 1.0f | GameApp.cpp InitOceanEffectParams |
| _NormalStrength | 0.8f | 同上 |
| _Roughness | 0.05f | 同上 |
| _FogDensity | 0.005f | 同上（米制适配） |
| _FogPower | 2.0f | 同上 |
| _TessEdgeLength | 16 | 同上 |
| _TessFar | 500.0f | 同上 |
| _EnvironmentLightStrength | 1.0f | 同上 |
| JONSWAP scale | 0.4f | FillJonswapStruct |
| JONSWAP windSpeed | 1200.0f (cm/s = 12m/s) | 同上 |
| JONSWAP fetch | 600.0f (m) | 同上 |
| 网格 | 256×256m, 64×64 顶点 | CreateGrid |

---

## 十一、会话关键节点

1. **初始诊断**: 读取全部 CS/Shader/C++ 代码，发现 b1 标红
2. **Bug 2 定位**: 用户通过 RenderDoc 发现数组 16B 对齐问题
3. **深度衰减修复**: AI 发现 clipDepth 非线性导致位移被抹平
4. **曲面细分实现**: AI 重写 VS/HS/DS，实现完整 HS/DS 管线
5. **HS/DS b0 问题**: 多次尝试后确定框架不管理 HS/DS → 手动创建缓冲区
6. **参数调优**: 波浪速度调 `_FrameTime += dt * speedFactor`

---

## 十二、未实现的功能（后续可做）

- [x] ~~边缘泡沫（需要深度缓冲 intersection 计算）~~ → 2026-06-22 会话实现
- [x] ~~线框模式（GS 生成重心坐标）~~ → GS 已重写输出 barycentric，PS 中加 smoothstep 即可启用
- [ ] 水下效果（折射）
- [ ] 浮力系统（_BuoyancyData 已生成，未接入）
- [ ] ImGui 实时调参面板

---

## 十三、2026-06-22 ~ 2026-06-24 会话记录

### 阶段一：全代码阅读 + 架构理解

- 阅读全部 15 个源文件（6 CS + 5 渲染 Shader + 3 C++）
- 阅读 3 个 Unity 参考文件（`FFTOcean_Compute.compute` / `FFTOcean_Shader.shader` / `FFTOcean_Script.cs`）
- 对比发现缺失功能：GS 是占位符、阴影管线完全缺失、边缘泡沫缺失

### 阶段二：GS 完整重写

- **`Shaders/Ocean_GS.hlsl`** 完全重写：从仅透传 SV_POSITION 的占位符 → 完整 GS
  - 输入 `triangle VSOutput[3]`，输出 `GSOutput`
  - 生成重心坐标 `(1,0)(0,1)(0,0)` 匹配 Unity `geo()`
  - 透传全部 DS 输出字段（clipPos, worldPos, uv, clipDepth, viewDepth, screenUV, foam, waveHeight, ndcDepth）
- **`Shaders/Ocean_Common.hlsli`** 添加 `GSOutput` 结构体（VSOutput 超集 + `barycentric:TEXCOORD8`）
- **PS 无需修改**：GSOutput 是 VSOutput 超集，PS 按语义匹配自动忽略多余 barycentric 字段
- **`OceanEffect.cpp`**：`InitAll()` 加载 `Ocean_GS.cso`，`Apply()` 中 `GSSetShader` 绑定/解绑
- 设计要点：GS 当前无可见效果（barycentric 未被 PS 使用），保留它为了匹配 Unity 架构 + 线框模式基建

### 阶段三：边缘泡沫实现

- **深度拷贝方案**：D3D11 不允许同一子资源同时绑定 DSV 和 SRV → 用 `CopySubresourceRegion` 将主深度缓冲拷贝到独立纹理
- **`GameApp.h/cpp`**：新增 `m_pSceneDepthCopyTex`/`m_pSceneDepthCopySRV` + `CopyDepthBeforeOcean()`
  - 纹理格式 `R24G8_TYPELESS`（与主深度同格式）
  - SRV 格式 `R24_UNORM_X8_TYPELESS`（可被 PS 读取）
  - DrawScene 中球体渲染后、水面渲染前插入拷贝
- **`Shaders/Ocean_Common.hlsli`**：VSOutput/GSOutput 新增 `ndcDepth:TEXCOORD7`；声明 `g_SceneDepthMap:t10`
- **`Shaders/Ocean_DS.hlsl`**：输出 `ndcDepth = clipPos.z / clipPos.w`
- **`Shaders/Ocean_PS.hlsl`**：边缘泡沫 intersect 计算（场景 NDC 深度 vs 水面 NDC 深度 → smoothstep → 泡沫加成）
- **`OceanEffect`**：新增 `SetSceneDepthMap()` 接口 + `Apply()` 中绑定 t10
- 设计要点：NDC 深度比较（两个深度在同一空间），阈值 0.001 ≈ 1m@1000m 远平面

### 阶段四：学习文档编写

- **`GUIDE/TESSELLATION_PIPELINE_GUIDE.md`**（10 章 + 2 附录）
  - 第 1 章：D3D11 曲面细分管线全阶段概览
  - 第 2 章：结构体链数据流追踪（VSInput→HSInput→HSCPOutput→VSOutput→GSOutput）
  - 第 3-8 章：每个 Shader 逐行详解（VS/HS/DS/GS/PS + C++ Apply()）
  - 第 9 章：Q&A 回答 GUIDE 中所有 Review 重点问题
  - 第 10 章：边缘泡沫原理（含数学推导）
  - 附录 A：关键参数速查表
  - 附录 B：Shader 文件依赖关系图

- **`GUIDE/QA_SESSION.md`**（11 个 Q&A）
  - Q1: DS 的作用和原理（一对多误解纠正）
  - Q2: HS 双函数结构（面片常量 vs 控制点函数视野差异）
  - Q3: 硬件细分器的激活机制（三个条件：PATCHLIST_3 + HSSetShader + DSSetShader）
  - Q4: 深度衰减 vs 曲面细分（两个独立 LOD 机制的分工）
  - Q5: GS 的必要性分析（当前是基建，重心坐标未被使用）
  - Q6: 次表面散射在水体渲染中的核心地位
  - Q7: HS→Tessellator→DS 完整数据流（"胎位"概念澄清）
  - Q8: PS 6 层视觉成分拆解（Fresnel/高光/散射/反射/泡沫/雾）
  - Q9: BRDF 是什么（Beckmann(D)×Smith(G)×Fresnel(F)）
  - Q10: D3D11 每 Stage 独立资源槽位模型
  - Q11: 为什么所有 Stage 都绑 b1/b2（框架空版本扩散 + HS/DS 实际需要 b1）

- **`GUIDE/PROJECT_RETROSPECTIVE.md`**（10 章）
  - 项目一句话描述、两大子系统架构图、数据流全景 ASCII 图
  - **重点：8 个 Unity→DX11 "曲线救国"决策**（面试价值最高）
    1. cbuffer 16B 对齐（RenderDoc b1 标红）
    2. HS/DS b0 手动管理（框架不支持细分 Stage）
    3. 4 张独立 Texture2D 替代 Texture2DArray
    4. 深度缓冲拷贝实现边缘泡沫（DSV/SRV 不能同时绑定）
    5. clipPosBase.w 替代 Linear01Depth（透视投影数学）
    6. GS 是基建（匹配 Unity 架构但承认暂未使用）
    7. PATCHLIST_3 拓扑切换（细分器激活三条件）
    8. 雾密度单位换算（世界米制 vs Unity 归一化深度）

### 阶段五：GameApp.cpp 美术参数注释

- CS 频谱参数 19 项全注释（物理含义 + 调参方向）
- JONSWAP 8 组参数按 4 频率层分组注释
- 渲染参数 35 项按 9 组分类注释（波浪叠加/几何强度/深度衰减/材质/散射/泡沫/雾效/法线变化/曲面细分）

### 关键设计主题

- 所有新增代码标注 `// AI-GENERATED`
- 修改前原文件备份至 `GUIDE/backups/`
- 保持 b0 大小不变（144B），不添加阴影矩阵
- PS 输入类型无需修改（GSOutput 是 VSOutput 的超集，语义匹配自动处理）
- 曲面细分管线激活条件：PATCHLIST_3 拓扑 + HSSetShader + DSSetShader（无 HLSL 调用语句）
