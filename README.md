# FFT Ocean Wave in DirectX 11

> [!IMPORTANT]
> **实际项目位置：** [`Project 19-/22 Static Cube Mapping`](./Project%2019-/22%20Static%20Cube%20Mapping)
>
> 这不是 DirectX11 With Windows SDK 教程仓库的官方版本。本项目基于原教程第 22 课 `Static Cube Mapping` 修改得到，核心 FFT 水面模拟代码位于上面的目录中。
>
> 根目录中的其他 Project、`assimp`、`ImGui`、`MarkdownFiles` 等内容属于原框架，用于编译、依赖和学习参考，不是本项目的主要实现。

> **Project location:** [`Project 19-/22 Static Cube Mapping`](./Project%2019-/22%20Static%20Cube%20Mapping)
>
> This repository is a custom FFT ocean-wave project built on top of Lesson 22
> (`Static Cube Mapping`) of the DirectX11 With Windows SDK tutorial framework.
> The remaining lesson folders and third-party libraries are retained as part of
> the original framework.

## 项目简介

本项目在 DirectX 11 中实现了一套 GPU FFT 水面模拟与渲染流程，主要包含：

- 基于 JONSWAP 频谱的初始波浪生成
- 频谱共轭处理与随时间演化
- 横向、纵向 Cooley-Tukey IFFT
- 位移、坡度和辅助数据纹理生成
- 顶点、外壳、域、几何和像素着色器组成的曲面细分渲染管线
- 水面光照、波峰散射、泡沫、雾效和天空盒
- ImGui 参数面板与自由摄像机交互

## 代码入口

所有与 FFT 水面相关的核心代码都位于 [`Project 19-/22 Static Cube Mapping`](./Project%2019-/22%20Static%20Cube%20Mapping)：

| 路径 | 说明 |
| --- | --- |
| [`GameApp.cpp`](./Project%2019-/22%20Static%20Cube%20Mapping/GameApp.cpp) | 场景、摄像机、参数面板和绘制调度 |
| [`OceanEffect.cpp`](./Project%2019-/22%20Static%20Cube%20Mapping/OceanEffect.cpp) | 水面效果、常量和资源绑定、计算与渲染调度 |
| [`FFT_OceanWave_CS.hlsl`](./Project%2019-/22%20Static%20Cube%20Mapping/FFT_OceanWave_CS.hlsl) | 早期整合版本的 FFT 水面计算着色器 |
| [`Shaders/`](./Project%2019-/22%20Static%20Cube%20Mapping/Shaders) | 当前使用的 FFT 计算着色器和水面渲染着色器 |
| [`GUIDE/`](./Project%2019-/22%20Static%20Cube%20Mapping/GUIDE) | 曲面细分管线、代码结构和实现说明 |

FFT 计算阶段位于：

```text
InitializeSpectrum_CS.hlsl
        |
PackSpectrumConjugate_CS.hlsl
        |
UpdateSpectrum_CS.hlsl
        |
HorizontalIFFT_CS.hlsl
        |
VerticalIFFT_CS.hlsl
        |
AssembleTextures_CS.hlsl
```

水面渲染阶段位于：

```text
Ocean_VS.hlsl -> Ocean_HS.hlsl -> Ocean_DS.hlsl
       -> Ocean_GS.hlsl -> Ocean_PS.hlsl
```

## 构建与运行

建议环境：

- Windows 10 或 Windows 11
- Visual Studio 2022，安装 C++ 桌面开发工具和 Windows 10/11 SDK
- CMake 3.14 或更高版本
- 使用 `x64` 配置

在仓库根目录执行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target 22_Static_Cube_Mapping
```

构建完成后，将 `22_Static_Cube_Mapping` 设为 Visual Studio 启动项目并运行。

> 项目目标名称仍保留为原框架的 `22_Static_Cube_Mapping`，这是为了尽量少地改动原 CMake 结构。运行内容已经是本项目修改后的 FFT 水面场景。

## 原框架说明

本项目保留了原教程框架的部分代码、目录结构、依赖和许可证，以便直接编译和学习：

- 原框架：[MKXJun/DirectX11-With-Windows-SDK](https://github.com/MKXJun/DirectX11-With-Windows-SDK)
- 修改基底：教程第 22 课 `Static Cube Mapping`
- 许可证：[MIT License](./LICENSE)

除 `Project 19-/22 Static Cube Mapping` 中的 FFT 水面修改外，其余教程示例和第三方库的版权归原作者及对应项目所有。
