#include "GameApp.h"
#include <XUtil.h>
#include <DXTrace.h>
#include <d3dcompiler.h>
#include <algorithm>
using namespace DirectX;
#define PI 3.14159265358979

GameApp::GameApp(HINSTANCE hInstance, const std::wstring& windowName, int initWidth, int initHeight)
    : D3DApp(hInstance, windowName, initWidth, initHeight)
{
}

GameApp::~GameApp()
{
}

bool GameApp::Init()
{
    //初始化D3D应用
    if (!D3DApp::Init())
        return false;
    //初始化纹理和模型管家
    m_TextureManager.Init(m_pd3dDevice.Get());
    m_ModelManager.Init(m_pd3dDevice.Get());

    // 务必先初始化所有渲染状态，以供下面的特效使用
    RenderStates::InitAll(m_pd3dDevice.Get());

    // 创建该 Effect 内部需要的所有渲染状态对象（光栅化状态、深度模板状态、混合状态），并存入成员变量
    if (!m_BasicEffect.InitAll(m_pd3dDevice.Get())) //初始化普通渲染状态：正常写入深度/背面剔除/直接混合/线性采样纹理
        return false;

    if (!m_SkyboxEffect.InitAll(m_pd3dDevice.Get())) //初始化天空盒渲染状态：不写入深度/不剔除/直接混合/线性采样纹理
        return false;

    if (!m_OceanEffect.InitAll(m_pd3dDevice.Get()))
        return false;

    if (!InitEffect())
        return false;

    if (!InitResource())
        return false;

    return true;
}

void GameApp::OnResize()
{

    D3DApp::OnResize();
    
    //这是设置深度缓冲区吗（？）
    m_pDepthTexture = std::make_unique<Depth2D>(m_pd3dDevice.Get(), m_ClientWidth, m_ClientHeight);
    m_pDepthTexture->SetDebugObjectName("DepthTexture");

    // 摄像机变更显示
    if (m_pCamera != nullptr)
    {
        m_pCamera->SetFrustum(XM_PI / 3, AspectRatio(), 1.0f, 1000.0f);
        m_pCamera->SetViewPort(0.0f, 0.0f, (float)m_ClientWidth, (float)m_ClientHeight);//更新视口大小
        m_BasicEffect.SetProjMatrix(m_pCamera->GetProjMatrixXM());//更新投影矩阵
        m_SkyboxEffect.SetProjMatrix(m_pCamera->GetProjMatrixXM());
    }
}
/// <summary>
/// 每一帧的行为
/// </summary>
/// <param name="dt"></param>
void GameApp::UpdateScene(float dt)
{
    m_CameraController.Update(dt); //更新摄像机的矩阵什么的

    m_CBFFTOceanParams._FrameTime += dt * 0.5f; //常量缓冲区时间更新

    for (int step = 0; step < 4; step++)
    {
        DispatchInitializeSpectrum_CS(step); //初始化频谱
        DispatchPackSpectrumConjugate_CS(step); //打包共轭数据
        DispatchUpdateSpectrum_CS(step); //循环更新频谱
    }
    for (int step = 0; step < 8; step++)
    {
        DispatchHorizontalIFFT_CS(step); //水平逆傅里叶变换
        DispatchVerticalIFFT_CS(step); //垂直逆傅里叶变换
    }
    for (int step = 0; step < 4; step++)
    {
        DispatchAssembleTextures_CS(step); //组装好数据
    }
    // 更新观察矩阵
    m_BasicEffect.SetViewMatrix(m_pCamera->GetViewMatrixXM());
    m_BasicEffect.SetEyePos(m_pCamera->GetPosition());
    m_SkyboxEffect.SetViewMatrix(m_pCamera->GetViewMatrixXM());

    //下面这一段就是ImGui相关的代码，我们就不管了，放着也挺好的
    if (ImGui::Begin("Static Cube Mapping"))
    {
        static int skybox_item = 0;
        static const char* skybox_strs[] = {
            "Daylight",
            "Sunset",
            "Desert"
        };
        if (ImGui::Combo("Skybox", &skybox_item, skybox_strs, ARRAYSIZE(skybox_strs)))
        {
            Model* pModel = m_ModelManager.GetModel("Skybox");
            switch (skybox_item)
            {
            case 0: 
                m_BasicEffect.SetTextureCube(m_TextureManager.GetTexture("Daylight"));
                pModel->materials[0].Set<std::string>("$Skybox", "Daylight");
                break;
            case 1: 
                m_BasicEffect.SetTextureCube(m_TextureManager.GetTexture("Sunset"));
                pModel->materials[0].Set<std::string>("$Skybox", "Sunset");
                break;
            case 2: 
                m_BasicEffect.SetTextureCube(m_TextureManager.GetTexture("Desert")); 
                pModel->materials[0].Set<std::string>("$Skybox", "Desert");
                break;
            }
        }
    }
    ImGui::End();
    ImGui::Render();
}

// AI-GENERATED: 场景深度拷贝 — 水面渲染前，将主深度缓冲中的场景深度（球体）拷贝到可 SRV 读取的纹理
void GameApp::CopyDepthBeforeOcean()
{
    // 主深度缓冲 m_pDepthTexture 中的深度数据直接拷贝到 m_pSceneDepthCopyTex
    // 两者格式相同（R24G8_TYPELESS），CopySubresourceRegion 零开销
    m_pd3dImmediateContext->CopySubresourceRegion(
        m_pSceneDepthCopyTex.Get(), 0, 0, 0, 0,
        m_pDepthTexture->GetTexture(), 0, nullptr);
}

void GameApp::DrawScene()
{
    // 创建后备缓冲区的渲染目标视图
    if (m_FrameCount < m_BackBufferCount) //这个
    {
        ComPtr<ID3D11Texture2D> pBackBuffer; //纹理
        m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(pBackBuffer.GetAddressOf()));
        CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); //设置RTV
        m_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), &rtvDesc, m_pRenderTargetViews[m_FrameCount].ReleaseAndGetAddressOf()); //创建RTV视图
    }

    float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_pd3dImmediateContext->ClearRenderTargetView(GetBackBufferRTV(), black); //清屏
    m_pd3dImmediateContext->ClearDepthStencilView(m_pDepthTexture->GetDepthStencil(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);//清空深度缓冲区
    ID3D11RenderTargetView* pRTVs[1] = { GetBackBufferRTV() }; //渲染的颜色输出目标——所有像素着色器的输出颜色写入这个 Buffer
    m_pd3dImmediateContext->OMSetRenderTargets(1, pRTVs, m_pDepthTexture->GetDepthStencil());
    D3D11_VIEWPORT viewport = m_pCamera->GetViewPort();
    m_pd3dImmediateContext->RSSetViewports(1, &viewport);

    // 绘制模型
    m_BasicEffect.SetRenderDefault();
    m_BasicEffect.SetReflectionEnabled(true);
    m_Sphere.Draw(m_pd3dImmediateContext.Get(), m_BasicEffect);
    m_BasicEffect.SetReflectionEnabled(false);

    // AI-GENERATED: 水面渲染前拷贝场景深度 → edge foam 用
    CopyDepthBeforeOcean();

    // 绘制水面
    m_OceanEffect.SetRenderDefault();
    m_OceanEffect.SetWorldMatrix(m_OceanSurface.GetTransform().GetLocalToWorldMatrixXM());
    m_OceanEffect.SetViewMatrix(m_pCamera->GetViewMatrixXM());
    m_OceanEffect.SetProjMatrix(m_pCamera->GetProjMatrixXM());
    m_OceanEffect.SetEyePos(m_pCamera->GetPosition());

    FillOceanEffectParams();
    FillOceanEffectSRVs();

    m_OceanSurface.Draw(m_pd3dImmediateContext.Get(), m_OceanEffect);

    // 绘制天空盒
    m_SkyboxEffect.SetRenderDefault();
    m_Skybox.Draw(m_pd3dImmediateContext.Get(), m_SkyboxEffect);


    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    HR(m_pSwapChain->Present(0, m_IsDxgiFlipModel ? DXGI_PRESENT_ALLOW_TEARING : 0));
}

bool GameApp::InitEffect()
{
    //我们这个计算着色器是独立的，与BasicEffect没法一起编译，那么我们就只能单独重写一遍编译着色器的代码
    ComPtr<ID3DBlob> blob; //blob本质就是一块内存块，将源码编译成功GPU能够执行的二进制字节码，存进这个blob
    ComPtr<ID3DBlob> errorBlob;
    
    //加载预编译的.cso（工作目录即 exe 所在目录，同 Build 目录下）
    CreateComputeShader(L"InitializeSpectrum_CS.cso", m_pCS_InitialSpectrumTex);
    CreateComputeShader(L"PackSpectrumConjugate_CS.cso", m_pCS_PackSpectrumConjugate);
    CreateComputeShader(L"UpdateSpectrum_CS.cso", m_pCS_UpdateSpectrumTex);
    CreateComputeShader(L"HorizontalIFFT_CS.cso", m_pCS_HorizontalIFFT);
    CreateComputeShader(L"VerticalIFFT_CS.cso", m_pCS_VerticalIFFT);
    CreateComputeShader(L"AssembleTextures_CS.cso", m_pCS_AssembleTextures);

    InitOceanEffectParams();

    return true;
}

void GameApp::CreateComputeShader(const std::wstring& fileName, ComPtr<ID3D11ComputeShader>& shader)
{
    ComPtr<ID3DBlob> blob;
    std::wstring filePath = L"Shaders\\" + fileName;

    HR(D3DReadFileToBlob(filePath.c_str(), blob.GetAddressOf()));
    HR(m_pd3dDevice->CreateComputeShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr,
        shader.GetAddressOf()
    ));
}

void GameApp::CreateVertexShader(const std::wstring& fileName, ComPtr<ID3D11VertexShader>& shader)
{
    ComPtr<ID3DBlob> blob;
    std::wstring filePath = L"Shaders\\" + fileName;

    HR(D3DReadFileToBlob(filePath.c_str(), blob.GetAddressOf()));
    HR(m_pd3dDevice->CreateVertexShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr,
        shader.GetAddressOf()
    ));
}

void GameApp::CreatePixelShader(const std::wstring& fileName, ComPtr<ID3D11PixelShader>& shader)
{
    ComPtr<ID3DBlob> blob;
    std::wstring filePath = L"Shaders\\" + fileName;

    HR(D3DReadFileToBlob(filePath.c_str(), blob.GetAddressOf()));
    HR(m_pd3dDevice->CreatePixelShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr,
        shader.GetAddressOf()
    ));
}

void GameApp::DispatchInitializeSpectrum_CS(int layer)
{
    //修改常量缓冲区
    m_CBFFTOceanParams._InitialSpectrumIndex = layer;
    //上传常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped;
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams));
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);
    //绑定SRV
    m_pd3dImmediateContext->CSSetConstantBuffers(0, 1, m_pCB_FFTOcean.GetAddressOf());
    m_pd3dImmediateContext->CSSetShaderResources(0, 1, m_JONSWAPSRV.GetAddressOf());
    //绑定UAV
    m_pd3dImmediateContext->CSSetShader(m_pCS_InitialSpectrumTex.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[1] = { m_InitialSpectrumUAVs[layer].Get() };
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    //启动线程组
    m_pd3dImmediateContext->Dispatch(128, 128, 1); //同一时刻只能启动一个Kernel吗
    //解绑资源
    ID3D11UnorderedAccessView* nullUAVs[1] = {};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
}

void GameApp::DispatchPackSpectrumConjugate_CS(int layer)
{
    //修改常量缓冲区
    m_CBFFTOceanParams._InitialSpectrumIndex = layer;
    //上传常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped;
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams));
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);
    //绑定SRV
    m_pd3dImmediateContext->CSSetConstantBuffers(0, 1, m_pCB_FFTOcean.GetAddressOf());
    //绑定UAV
    m_pd3dImmediateContext->CSSetShader(m_pCS_PackSpectrumConjugate.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[1] = { m_InitialSpectrumUAVs[layer].Get() };
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    //启动线程组
    m_pd3dImmediateContext->Dispatch(128, 128, 1); //同一时刻只能启动一个Kernel吗
    //解绑资源
    ID3D11UnorderedAccessView* nullUAVs[1] = {};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
}

void GameApp::DispatchUpdateSpectrum_CS(int layer)
{
    //修改常量缓冲区
    m_CBFFTOceanParams._InitialSpectrumIndex = layer;
    //上传常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped;
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams));
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);
    //绑定SRV
    m_pd3dImmediateContext->CSSetConstantBuffers(0, 1, m_pCB_FFTOcean.GetAddressOf());
    m_pd3dImmediateContext->CSSetShaderResources(0, 1, m_JONSWAPSRV.GetAddressOf());
    //绑定UAV
    m_pd3dImmediateContext->CSSetShader(m_pCS_UpdateSpectrumTex.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[4] = { 
        m_InitialSpectrumUAVs[layer].Get(),
        m_SpectrumUAVs[layer * 2].Get(),
        m_SpectrumUAVs[layer * 2 + 1].Get(),
        m_VariationMaskUAV.Get()};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    //启动线程组
    m_pd3dImmediateContext->Dispatch(128, 128, 1); //同一时刻只能启动一个Kernel吗
    //解绑资源
    ID3D11UnorderedAccessView* nullUAVs[4] = {};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 4, nullUAVs, nullptr);
}

void GameApp::DispatchHorizontalIFFT_CS(int layer)
{
    //修改常量缓冲区
    m_CBFFTOceanParams._SpectrumIndex = layer;
    //上传常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped;
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams));
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);
    //绑定SRV
    m_pd3dImmediateContext->CSSetConstantBuffers(0, 1, m_pCB_FFTOcean.GetAddressOf());
    m_pd3dImmediateContext->CSSetShaderResources(0, 1, m_JONSWAPSRV.GetAddressOf());
    //绑定UAV
    m_pd3dImmediateContext->CSSetShader(m_pCS_HorizontalIFFT.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[2] = { m_SpectrumUAVs[layer].Get(), m_VariationMaskUAV.Get()};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    //启动线程组
    m_pd3dImmediateContext->Dispatch(1, m_CBFFTOceanParams._Resolution, 1); //同一时刻只能启动一个Kernel吗
    //解绑资源
    ID3D11UnorderedAccessView* nullUAVs[2] = {};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
}

void GameApp::DispatchVerticalIFFT_CS(int layer)
{
    //修改常量缓冲区
    m_CBFFTOceanParams._SpectrumIndex = layer;
    //上传常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped;
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams));
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);
    //绑定SRV
    m_pd3dImmediateContext->CSSetConstantBuffers(0, 1, m_pCB_FFTOcean.GetAddressOf());
    m_pd3dImmediateContext->CSSetShaderResources(0, 1, m_JONSWAPSRV.GetAddressOf());
    //绑定UAV
    m_pd3dImmediateContext->CSSetShader(m_pCS_VerticalIFFT.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[2] = { m_SpectrumUAVs[layer].Get(), m_VariationMaskUAV.Get() };
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    //启动线程组
    m_pd3dImmediateContext->Dispatch(1, m_CBFFTOceanParams._Resolution, 1); //同一时刻只能启动一个Kernel吗
    //解绑资源
    ID3D11UnorderedAccessView* nullUAVs[2] = {};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
}

void GameApp::DispatchAssembleTextures_CS(int layer)
{
    //修改常量缓冲区
    m_CBFFTOceanParams._SpectrumIndex = layer;
    m_CBFFTOceanParams._DisplacementIndex = layer;
    m_CBFFTOceanParams._SlopeIndex = layer;
    //上传常量缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped;
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams));
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);
    //绑定SRV
    m_pd3dImmediateContext->CSSetConstantBuffers(0, 1, m_pCB_FFTOcean.GetAddressOf());
    m_pd3dImmediateContext->CSSetShaderResources(0, 1, m_JONSWAPSRV.GetAddressOf());
    //绑定UAV
    m_pd3dImmediateContext->CSSetShader(m_pCS_AssembleTextures.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[6] = { 
        m_SpectrumUAVs[layer * 2].Get(),
        m_SpectrumUAVs[layer * 2 + 1].Get(),
        m_VariationMaskUAV.Get(),
        m_DisplacementUAVs[layer].Get(),
        m_SlopeUAVs[layer].Get(),
        m_BuoyancyDataUAV.Get()
        };
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
    //启动线程组
    m_pd3dImmediateContext->Dispatch(128, 128, 1); //同一时刻只能启动一个Kernel吗
    //解绑资源
    ID3D11UnorderedAccessView* nullUAVs[6] = {};
    m_pd3dImmediateContext->CSSetUnorderedAccessViews(0, 6, nullUAVs, nullptr);
}

bool GameApp::InitResource()
{
    //预分配vector的空间
    m_InitialSpectrumTexs.resize(4);
    m_InitialSpectrumUAVs.resize(4);
    m_SpectrumTexs.resize(8);
    m_SpectrumUAVs.resize(8);
    m_DisplacementTexs.resize(4);
    m_DisplacementUAVs.resize(4);
    m_DisplacementSRVs.resize(4);
    m_SlopeTexs.resize(4);
    m_SlopeUAVs.resize(4);
    m_SlopeSRVs.resize(4);
    //创建纹理的循环，折叠起来了
    for (int i = 0; i < 4; ++i)
    {
        CreateTextureUAVSRV(
            1024, 1024,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            m_InitialSpectrumTexs[i].GetAddressOf(),
            m_InitialSpectrumUAVs[i].GetAddressOf(),
            nullptr,
            false, false
        );
    }
    for (int i = 0; i < 8; ++i)
    {
        CreateTextureUAVSRV(
            1024, 1024,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            m_SpectrumTexs[i].GetAddressOf(),
            m_SpectrumUAVs[i].GetAddressOf(),
            nullptr,
            false, false
        );
    }
    for (int i = 0; i < 4; ++i)
    {
        CreateTextureUAVSRV(
            1024, 1024,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            m_DisplacementTexs[i].GetAddressOf(),
            m_DisplacementUAVs[i].GetAddressOf(),
            m_DisplacementSRVs[i].GetAddressOf(),
            true, true
        );
    }
    for (int i = 0; i < 4; ++i)
    {
        CreateTextureUAVSRV(
            1024, 1024,
            DXGI_FORMAT_R32G32_FLOAT,
            m_SlopeTexs[i].GetAddressOf(),
            m_SlopeUAVs[i].GetAddressOf(),
            m_SlopeSRVs[i].GetAddressOf(),
            true, true
        );
    }
    {
        CreateTextureUAVSRV(
            1024, 1024,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            m_VariationMaskTex.GetAddressOf(),
            m_VariationMaskUAV.GetAddressOf(),
            m_VariationMaskSRV.GetAddressOf(),
            true, false
        );
        CreateTextureUAVSRV(
            1024, 1024,
            DXGI_FORMAT_R32_FLOAT,
            m_BuoyancyDataTex.GetAddressOf(),
            m_BuoyancyDataUAV.GetAddressOf(),
            nullptr,
            true, false
        );
    }

    // 清空位移和斜率 UAV，避免随机初始值导致 NaN
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 4; ++i) 
    {
        m_pd3dImmediateContext->ClearUnorderedAccessViewFloat(
            m_DisplacementUAVs[i].Get(), clearColor);
        m_pd3dImmediateContext->ClearUnorderedAccessViewFloat(
            m_SlopeUAVs[i].Get(), clearColor);
    }

    //创建用于水面效果的常量缓冲区
    D3D11_BUFFER_DESC cbd;
    ZeroMemory(&cbd, sizeof(cbd));
    cbd.Usage = D3D11_USAGE_DYNAMIC; //CPU每帧都会写，默认好像是只有GPU读吗
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; //bindFlag描述其用途：常量缓冲区
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = sizeof(CB_FFTOceanParams); //描述常量缓冲区的长度
    HR(m_pd3dDevice->CreateBuffer(&cbd, nullptr, m_pCB_FFTOcean.GetAddressOf())); //创建缓冲区：缓冲区描述/初始值/缓冲区本体
    // ============================================================
    // CS 端物理参数初始化（控制波浪的物理特性）
    // 这些参数传给 b0 (FFTOceanParams) ，6 个 Compute Shader 使用
    // ============================================================
    m_CBFFTOceanParams._Depth     = 10.0f;    // 水深（米），影响色散关系：浅水浪慢、深水浪快
    m_CBFFTOceanParams._Gravity   = 9.81f;    // 重力加速度（m/s²），决定波浪频率
    m_CBFFTOceanParams._FrameTime = 0.0f;     // 累计时间（秒），每帧 UpdateScene 更新，驱动波浪动画
    m_CBFFTOceanParams._RepeatTime = 200.0f;  // 波浪循环周期（秒），超过后相位回卷
    m_CBFFTOceanParams._LowCutOff  = 0.0001f; // 最低频率截止，过滤掉超长波（波长>几十公里的潮汐波）
    m_CBFFTOceanParams._HighCutOff = 9000.0f; // 最高频率截止，过滤掉超短毛细波
    m_CBFFTOceanParams._WaveSharpX = 0.5f;     // X 方向波浪锐度（波峰尖锐程度，0=圆润正弦，1=尖锐）
    m_CBFFTOceanParams._WaveSharpY = 0.5f;     // Y 方向波浪锐度

    m_CBFFTOceanParams._Resolution = 1024;            // FFT 分辨率（纹理尺寸 1024x1024），越高波浪细节越多
    m_CBFFTOceanParams._LengthScale0 = 4;             // 第 0 层频谱尺度（最小 tile=0.04 → 覆盖 25m 范围的低频涌浪）
    m_CBFFTOceanParams._LengthScale1 = 4;             // 第 1 层频谱尺度
    m_CBFFTOceanParams._LengthScale2 = 4;             // 第 2 层频谱尺度
    m_CBFFTOceanParams._LengthScale3 = 4;             // 第 3 层频谱尺度（最大 tile=0.18 → 覆盖 5.5m 范围的高频细浪）

    m_CBFFTOceanParams._Seed = 28;                    // 随机种子，改变波浪的随机相位（不同种子 = 不同波浪形状）

    m_CBFFTOceanParams._FoamBias      = 0.2f;         // 泡沫偏移：Jacobian 低于此值不产生泡沫（越高泡沫越少）
    m_CBFFTOceanParams._FoamPower     = 2.0f;         // 泡沫强度指数：Jacobian 的幂次（越高泡沫边缘越锐利）
    m_CBFFTOceanParams._FoamAdd       = 0.2f;         // 每帧泡沫增量：浪尖处每次叠加的泡沫量
    m_CBFFTOceanParams._FoamDecayRate = 0.07f;        // 泡沫衰减率：每帧自然消散的比例（越大消散越快）

    m_CBFFTOceanParams._Speed = 0.5f;                 // 波浪动画速度倍率（1.0=自然速度，>1 加快，<1 减慢）

    //接下来的是需要每一帧维护的索引
    m_CBFFTOceanParams._InitialSpectrumIndex = 0; //0~3
    m_CBFFTOceanParams._SpectrumIndex = 0; //0~7
    m_CBFFTOceanParams._DisplacementIndex = 0; //0~3
    m_CBFFTOceanParams._SlopeIndex = 0; //0~3

    //将常量缓冲区上传一次
    D3D11_MAPPED_SUBRESOURCE mapped; //这个是什么
    HR(m_pd3dImmediateContext->Map(m_pCB_FFTOcean.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)); //DISCARD可以边写边读
    memcpy(mapped.pData, &m_CBFFTOceanParams, sizeof(CB_FFTOceanParams)); //内存拷贝：这是什么/获取缓冲区指针/缓冲区长度
    m_pd3dImmediateContext->Unmap(m_pCB_FFTOcean.Get(), 0);

    // ============================================================
    // JONSWAP 波浪频谱参数（ 8 组，每 2 组对应一个渲染层）
    // FillJonswapStruct(index, scale, windSpeed, windDirection, fetch, spreadBlend, swell, peakEnhancement, shortWavesFade)
    //
    // Layer 0: 低频涌浪（tile=0.04，波长最长～25m）- 两组交叉涌浪
    // ============================================================
    FillJonswapStruct(0, 0.4f, 1200.0f, 50.0f, 600.0f, 1.0f, 0.9f, 5, 0.8f);
    //                     │      │        │       │       │      │      │   │
    //      scale=0.4 ─────┘      │        │       │       │      │      │   │  波浪整体能量（越大浪越高）
    //      windSpeed=1200cm/s ───┘        │       │       │      │      │   │  风速（cm/s），12m/s≈6级强风
    //      windDirection=50° ─────────────┘       │       │      │      │   │  风向（度），决定波浪传播方向
    //      fetch=600m ────────────────────────────┘       │      │      │   │  风区长度（m），风吹过的距离
    //      spreadBlend=1.0 ───────────────────────────────┘      │      │   │  方向扩散混合（1=纯Cos2s方向集中）
    //      swell=0.9 ────────────────────────────────────────────┘      │   │  涌浪占比（接近1=长涌浪）
    //      peakEnhancement=5 ───────────────────────────────────────────┘   │  JONSWAP 峰度增强因子（3~5）
    //      shortWavesFade=0.8 ──────────────────────────────────────────────┘  短波衰减（越大高频衰减越快）
    FillJonswapStruct(1, 0.4f, 1000.0f, 0.0f, 500.0f, 1.0f, 0.9f, 5, 0.8f); // 与#0交叉叠加

    // Layer 1: 中频浪（tile=0.06）- 两组
    FillJonswapStruct(2, 0.2f, 800.0f, 60.0f, 400.0f, 0.98f, 0.9f, 5, 0.4f); // 能量减半，中等风浪
    FillJonswapStruct(3, 0.2f, 800.0f, 120.0f, 350.0f, 0.98f, 0.9f, 5, 0.4f);

    // Layer 2: 高频浪（tile=0.12）- 两组
    FillJonswapStruct(4, 0.04f, 100.0f, 260.0f, 100.0f, 0.95f, 0.8f, 3, 0.4f); // 微风涟漪
    FillJonswapStruct(5, 0.04f, 50.0f, 280.0f, 100.0f, 0.95f, 0.8f, 3, 0.4f);

    // Layer 3: 最高频细浪（tile=0.18）- 两组
    FillJonswapStruct(6, 0.1f, 10.0f, 0.0f, 40.0f, 0.8f, 0.6f, 1, 0.2f);  // 极细微波纹
    FillJonswapStruct(7, 0.1f, 10.0f, 0.0f, 20.0f, 0.6f, 0.4f, 1, 0.2f);  // 最小尺度涟漪

    //创建用于JONSWAP的结构体缓冲区
    D3D11_BUFFER_DESC sbd;
    ZeroMemory(&sbd, sizeof(sbd));
    sbd.Usage = D3D11_USAGE_IMMUTABLE; //初始化之后就不变了
    sbd.BindFlags = D3D11_BIND_SHADER_RESOURCE; //为什么是shaderResource？不是结构体缓冲区：结构体缓冲区也是以SRV的形式作为资源的
    sbd.CPUAccessFlags = 0;
    sbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    sbd.StructureByteStride = sizeof(JONSWAP);
    sbd.ByteWidth = sizeof(JONSWAP) * 8;
    //这个数据该怎么填：SUBRESOURCE又是什么
    D3D11_SUBRESOURCE_DATA initData; 
    initData.pSysMem = jonswapData; //指向我们在上面已经准备好的数组
    initData.SysMemPitch = 0;
    initData.SysMemSlicePitch = 0;
    //正式创建缓冲区，同时上传数据
    HR(m_pd3dDevice->CreateBuffer(&sbd, &initData, m_pJONSWAPBuffer.GetAddressOf())); //怎么感觉这里有点不对劲，我们怎么新建这个数据
    //创建SRV并绑定
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_UNKNOWN; //结构体缓冲区的格式类型就不是什么RGBA了，必须填UNKONWN
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = 8;
    //就像之前创建SRV那样，依次是：要传输的数据的地址/SRV描述的地址/SRV成员的地址
    HR(m_pd3dDevice->CreateShaderResourceView(m_pJONSWAPBuffer.Get(), &srvDesc, m_JONSWAPSRV.GetAddressOf()));

    // ******************
    // 初始化天空盒相关
    
    ComPtr<ID3D11Texture2D> pTex; //临时加载单张纹理
    D3D11_TEXTURE2D_DESC texDesc; //描述纹理的宽、高、格式等属性
    std::string filenameStr;
    std::vector<ID3D11ShaderResourceView*> pCubeTextures; //6 个面的 SRV
    std::unique_ptr<TextureCube> pTexCube; //立方体纹理对象，最终生成 Cubemap SRV
    // Daylight
    {
        filenameStr = "..\\Texture\\daylight0.png"; //设置加载纹理的路径
        for (size_t i = 0; i < 6; ++i)
        {
            filenameStr[19] = '0' + (char)i;
            pCubeTextures.push_back(m_TextureManager.CreateFromFile(filenameStr)); //将纹理从文件里面加载出来
        }
        //下面这几行是加载天空盒纹理的操作吗，是什么意思
        pCubeTextures[0]->GetResource(reinterpret_cast<ID3D11Resource**>(pTex.ReleaseAndGetAddressOf()));
        pTex->GetDesc(&texDesc);
        pTexCube = std::make_unique<TextureCube>(m_pd3dDevice.Get(), texDesc.Width, texDesc.Height, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        pTexCube->SetDebugObjectName("Daylight");
        for (uint32_t i = 0; i < 6; ++i)
        {
            pCubeTextures[i]->GetResource(reinterpret_cast<ID3D11Resource**>(pTex.ReleaseAndGetAddressOf()));
            m_pd3dImmediateContext->CopySubresourceRegion(pTexCube->GetTexture(), D3D11CalcSubresource(0, i, 1), 0, 0, 0, pTex.Get(), 0, nullptr);
        }
        m_TextureManager.AddTexture("Daylight", pTexCube->GetShaderResource());
    }
    
    // Sunset
    {
        filenameStr = "..\\Texture\\sunset0.bmp";
        pCubeTextures.clear();
        for (size_t i = 0; i < 6; ++i)
        {
            filenameStr[17] = '0' + (char)i;
            pCubeTextures.push_back(m_TextureManager.CreateFromFile(filenameStr));
        }
        pCubeTextures[0]->GetResource(reinterpret_cast<ID3D11Resource**>(pTex.ReleaseAndGetAddressOf()));
        pTex->GetDesc(&texDesc);
        pTexCube = std::make_unique<TextureCube>(m_pd3dDevice.Get(), texDesc.Width, texDesc.Height, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        pTexCube->SetDebugObjectName("Sunset");
        for (uint32_t i = 0; i < 6; ++i)
        {
            pCubeTextures[i]->GetResource(reinterpret_cast<ID3D11Resource**>(pTex.ReleaseAndGetAddressOf()));
            m_pd3dImmediateContext->CopySubresourceRegion(pTexCube->GetTexture(), D3D11CalcSubresource(0, i, 1), 0, 0, 0, pTex.Get(), 0, nullptr);
        }
        m_TextureManager.AddTexture("Sunset", pTexCube->GetShaderResource());
    }
    
    // Desert
    m_TextureManager.AddTexture("Desert", m_TextureManager.CreateFromFile("..\\Texture\\desertcube1024.dds", false, true));

    m_BasicEffect.SetTextureCube(m_TextureManager.GetTexture("Daylight"));
    
    // ******************
    // 初始化游戏对象
    //
    
    // 球体
    {
        Model* pModel = m_ModelManager.CreateFromGeometry("Sphere", Geometry::CreateSphere());
        pModel->SetDebugObjectName("Sphere");
        m_TextureManager.CreateFromFile("..\\Texture\\stone.dds");
        pModel->materials[0].Set<std::string>("$Diffuse", "..\\Texture\\stone.dds");
        pModel->materials[0].Set<XMFLOAT4>("$AmbientColor", XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f));
        pModel->materials[0].Set<XMFLOAT4>("$DiffuseColor", XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f));
        pModel->materials[0].Set<XMFLOAT4>("$SpecularColor", XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f));
        pModel->materials[0].Set<float>("$SpecularPower", 16.0f);
        pModel->materials[0].Set<XMFLOAT4>("$ReflectColor", XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f));
        m_Sphere.SetModel(pModel);
        m_Sphere.GetTransform().SetPosition(0.0f, -1.5f, 0.0f);
    }
    //水面
    {
        GeometryData surfaceData = Geometry::CreateGrid(
            XMFLOAT2(256.0f, 256.0f),
            XMUINT2(64, 64),
            XMFLOAT2(4.0f, 4.0f),
            [](float x, float z) { return 0.0f; },  // 高度函数 暂时为0
            [](float x, float z) { return XMFLOAT3(0.0f, 1.0f, 0.0f); }, // 法线向上
            [](float x, float z) { return XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f); }// 白色
        );
        Model* pModel = m_ModelManager.CreateFromGeometry("oceanSurface", surfaceData);
        pModel->SetDebugObjectName("oceanSurface");
        m_TextureManager.CreateFromFile("..\\Texture\\stone.dds");
        pModel->materials[0].Set<std::string>("$Diffuse", "..\\Texture\\stone.dds");
        pModel->materials[0].Set<XMFLOAT4>("$AmbientColor", XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f));
        pModel->materials[0].Set<XMFLOAT4>("$DiffuseColor", XMFLOAT4(0.4f, 0.4f, 0.8f, 1.0f));
        pModel->materials[0].Set<XMFLOAT4>("$SpecularColor", XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f));
        pModel->materials[0].Set<float>("$SpecularPower", 16.0f);
        pModel->materials[0].Set<XMFLOAT4>("$ReflectColor", XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f));
        m_OceanSurface.GetTransform().SetPosition(0.0f, -2.0f, 0.0f);
        m_OceanSurface.SetModel(pModel);
    }
    // 天空盒立方体
    Model* pModel = m_ModelManager.CreateFromGeometry("Skybox", Geometry::CreateBox());
    pModel->SetDebugObjectName("Skybox");
    pModel->materials[0].Set<std::string>("$Skybox", "Daylight");
    m_Skybox.SetModel(pModel);
    // ******************
    // 初始化摄像机
    //
    auto camera = std::make_shared<FirstPersonCamera>();
    m_pCamera = camera;
    m_CameraController.InitCamera(camera.get());
    camera->SetViewPort(0.0f, 0.0f, (float)m_ClientWidth, (float)m_ClientHeight);
    camera->SetFrustum(XM_PI / 3, AspectRatio(), 1.0f, 1000.0f);
    camera->LookTo(XMFLOAT3(0.0f, 0.0f, -10.0f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f));

    m_BasicEffect.SetViewMatrix(camera->GetViewMatrixXM());
    m_BasicEffect.SetProjMatrix(camera->GetProjMatrixXM());
    m_SkyboxEffect.SetViewMatrix(camera->GetViewMatrixXM());
    m_SkyboxEffect.SetProjMatrix(camera->GetProjMatrixXM());



    // ******************
    // 初始化不会变化的值
    //

    // 方向光
    DirectionalLight dirLight[4]{};
    dirLight[0].ambient = XMFLOAT4(0.15f, 0.15f, 0.15f, 1.0f);
    dirLight[0].diffuse = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
    dirLight[0].specular = XMFLOAT4(0.1f, 0.1f, 0.1f, 1.0f);
    dirLight[0].direction = XMFLOAT3(-0.577f, -0.577f, 0.577f);
    dirLight[1] = dirLight[0];
    dirLight[1].direction = XMFLOAT3(0.577f, -0.577f, 0.577f);
    dirLight[2] = dirLight[0];
    dirLight[2].direction = XMFLOAT3(0.577f, -0.577f, -0.577f);
    dirLight[3] = dirLight[0];
    dirLight[3].direction = XMFLOAT3(-0.577f, -0.577f, -0.577f);
    for (int i = 0; i < 4; ++i)
    {
        m_BasicEffect.SetDirLight(i, dirLight[i]);
        m_OceanEffect.SetDirLight(i, dirLight[i]);
    }

    //初始化渲染着色器的参数
    InitOceanEffectParams();

    // AI-GENERATED: 创建场景深度拷贝纹理（边缘泡沫用 — 水面渲染前从主深度缓冲拷贝）
    {
        D3D11_TEXTURE2D_DESC depthCopyDesc;
        ZeroMemory(&depthCopyDesc, sizeof(depthCopyDesc));
        depthCopyDesc.Width  = m_ClientWidth;
        depthCopyDesc.Height = m_ClientHeight;
        depthCopyDesc.MipLevels = 1;
        depthCopyDesc.ArraySize = 1;
        depthCopyDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;  // 与主深度缓冲相同格式
        depthCopyDesc.SampleDesc.Count = 1;
        depthCopyDesc.SampleDesc.Quality = 0;
        depthCopyDesc.Usage = D3D11_USAGE_DEFAULT;
        depthCopyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // 只需要 SRV 读取
        depthCopyDesc.CPUAccessFlags = 0;
        depthCopyDesc.MiscFlags = 0;
        HR(m_pd3dDevice->CreateTexture2D(&depthCopyDesc, nullptr, m_pSceneDepthCopyTex.GetAddressOf()));

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;  // R24G8_TYPELESS 的可读等效格式
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        HR(m_pd3dDevice->CreateShaderResourceView(m_pSceneDepthCopyTex.Get(), &srvDesc, m_pSceneDepthCopySRV.GetAddressOf()));
    }

    return true;
}

void GameApp::CreateTextureUAVSRV(UINT width, UINT height, DXGI_FORMAT format,
    ID3D11Texture2D** ppTex, ID3D11UnorderedAccessView** ppUAV, ID3D11ShaderResourceView** ppSRV,
    bool needSRV, bool needMips)
{
    D3D11_TEXTURE2D_DESC texDesc; // 创建纹理描述
    ZeroMemory(&texDesc, sizeof(texDesc));
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = needMips ? 0 : 1; //允许采样多少层Mip
    texDesc.ArraySize = 1; //单张纹理
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT; //GPU的读写属性
    texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS; //这个Bind Flag就是描述后续绑定用途的
    if (needSRV)
        texDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE; //中间这个符号是位运算
    if (needMips)
        texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    texDesc.CPUAccessFlags = 0; //CPU是否直接读写
    texDesc.MiscFlags = needMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0; //特殊行为（什么东西）
    //创建纹理数据（无内容）
    m_pd3dDevice->CreateTexture2D(&texDesc, nullptr, ppTex);
    //如果需要的话创建SRV
    if (needSRV && ppSRV)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = needMips ? -1 : 1;
        m_pd3dDevice->CreateShaderResourceView(*ppTex, &srvDesc, ppSRV);
    }
    //如果需要的话创建UAV
    if (ppUAV)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
        ZeroMemory(&uavDesc, sizeof(uavDesc));
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0; //允许CS写哪一层Mip
        m_pd3dDevice->CreateUnorderedAccessView(*ppTex, &uavDesc, ppUAV);
    }
}

void GameApp::FillJonswapStruct(int index, float scale, float windSpeed, float windDirection, float fetch,
    float spreadBlend, float swell, float peakEnhancement, float shortWavesFade)
{
    jonswapData[index].scale = scale;
    jonswapData[index].angle = windDirection / 180 * PI; //将度数转化为弧度
    jonswapData[index].spreadBlend = spreadBlend;
    jonswapData[index].swell = std::max(0.01f, std::min(swell, 1.0f));
    jonswapData[index].alpha = JonswapAlpha(fetch, windSpeed);
    jonswapData[index].peakOmega = JonswapPeakFrequency(fetch, windSpeed);
    jonswapData[index].gamma = peakEnhancement;
    jonswapData[index].shortWavesFade = shortWavesFade;
}

float GameApp::JonswapPeakFrequency(float fetch, float windSpeed)
{
    return 22.0f * pow(windSpeed * fetch / m_CBFFTOceanParams._Gravity / m_CBFFTOceanParams._Gravity, -0.33f);
}

float GameApp::JonswapAlpha(float fetch, float windSpeed)
{
    return 0.076f * pow(m_CBFFTOceanParams._Gravity * fetch / windSpeed / windSpeed, -0.22f);
}

void GameApp::FillOceanEffectParams()
{
    {
        float tiles[4] = { m_pCB_OceanEffectParams._Tile0, m_pCB_OceanEffectParams._Tile1, m_pCB_OceanEffectParams._Tile2, m_pCB_OceanEffectParams._Tile3 };
        m_OceanEffect.SetTiles(tiles);
    }
    {
        float layerContributes[4] = { m_pCB_OceanEffectParams._LayerContribute0, m_pCB_OceanEffectParams._LayerContribute1, m_pCB_OceanEffectParams._LayerContribute2, m_pCB_OceanEffectParams._LayerContribute3 };
        m_OceanEffect.SetLayerContributes(layerContributes);
    }
    m_OceanEffect.SetHeightStrength(m_pCB_OceanEffectParams._HeightStrength);
    m_OceanEffect.SetNormalStrength(m_pCB_OceanEffectParams._NormalStrength);
    m_OceanEffect.SetDisplaceDepthAttenuation(m_pCB_OceanEffectParams._DisplaceDepthAttenuation);
    m_OceanEffect.SetFoamDepthAttenuation(m_pCB_OceanEffectParams._FoamDepthAttenuation);
    m_OceanEffect.SetRoughness(m_pCB_OceanEffectParams._Roughness,
        m_pCB_OceanEffectParams._FoamRoughness);
    m_OceanEffect.SetAmbientDensity(m_pCB_OceanEffectParams._AmbientDensity);
    m_OceanEffect.SetEnvironmentLightStrength(m_pCB_OceanEffectParams._EnvironmentLightStrength);
    m_OceanEffect.SetScatterStrengths(m_pCB_OceanEffectParams._ScatterStrength,
        m_pCB_OceanEffectParams._ScatterShadowStrength,
        m_pCB_OceanEffectParams._WavePeakScatterStrength);
    m_OceanEffect.SetScatterColors(m_pCB_OceanEffectParams._ScatterColor,
        m_pCB_OceanEffectParams._ScatterPeakColor);
    m_OceanEffect.SetEdgeFoamPower(m_pCB_OceanEffectParams._EdgeFoamPower);
    m_OceanEffect.SetShadowIntensity(m_pCB_OceanEffectParams._ShadowIntensity);
    m_OceanEffect.SetFoamColor(m_pCB_OceanEffectParams._FoamColor);
    m_OceanEffect.SetFogParams(m_pCB_OceanEffectParams._FogDensity,
        m_pCB_OceanEffectParams._FogPower,
        m_pCB_OceanEffectParams._FogColor);
    m_OceanEffect.SetVariationMaskParams(m_pCB_OceanEffectParams._VarMaskRange,
        m_pCB_OceanEffectParams._VarMaskPower,
        m_pCB_OceanEffectParams._VarMaskTexScale);
    m_OceanEffect.SetTessellationEdgeLength(m_pCB_OceanEffectParams._TessEdgeLength);
    m_OceanEffect.SetTessellationNear(m_pCB_OceanEffectParams._TessNear);
    m_OceanEffect.SetTessellationFar(m_pCB_OceanEffectParams._TessFar);
}

void GameApp::FillOceanEffectSRVs()
{
    ID3D11ShaderResourceView* dispSRVs[4] = 
    {
        m_DisplacementSRVs[0].Get(), m_DisplacementSRVs[1].Get(),
        m_DisplacementSRVs[2].Get(), m_DisplacementSRVs[3].Get()
    };
    m_OceanEffect.SetDisplacementTextures(dispSRVs);
    ID3D11ShaderResourceView* slopeSRVs[4] = 
    {
        m_SlopeSRVs[0].Get(), m_SlopeSRVs[1].Get(),
        m_SlopeSRVs[2].Get(), m_SlopeSRVs[3].Get()
    };
    m_OceanEffect.SetSlopeTextures(slopeSRVs);
    m_OceanEffect.SetVariationMask(m_VariationMaskSRV.Get());
    m_OceanEffect.SetSkyboxTexture(m_TextureManager.GetTexture("Daylight"));
    m_OceanEffect.SetSceneDepthMap(m_pSceneDepthCopySRV.Get());  // AI-GENERATED: 场景深度纹理（边缘泡沫用）
}

void GameApp::InitOceanEffectParams()
{
    // ============================================================
    // 渲染着色器美术参数初始化（传给 OceanEffect → PS/DS 使用）
    // 调节这些值直接影响水面的视觉外观
    // ============================================================

    // ---- 4 层波浪叠加 ----
    m_pCB_OceanEffectParams._Tile0 = 0.04f;             // 第 0 层 UV 缩放：最小 → 低频涌浪（波长 ~25m）
    m_pCB_OceanEffectParams._Tile1 = 0.06f;             // 第 1 层 UV 缩放：中低频
    m_pCB_OceanEffectParams._Tile2 = 0.12f;             // 第 2 层 UV 缩放：中高频
    m_pCB_OceanEffectParams._Tile3 = 0.18f;             // 第 3 层 UV 缩放：最大 → 高频细浪（波长 ~5.5m）
    m_pCB_OceanEffectParams._LayerContribute0 = 0.8f;   // 第 0 层混合权重：涌浪贡献最大 → 视觉主体
    m_pCB_OceanEffectParams._LayerContribute1 = 0.8f;   // 第 1 层混合权重
    m_pCB_OceanEffectParams._LayerContribute2 = 0.6f;   // 第 2 层混合权重：高频贡献递减
    m_pCB_OceanEffectParams._LayerContribute3 = 0.4f;   // 第 3 层混合权重：最细浪贡献最小

    // ---- 波浪几何强度 ----
    m_pCB_OceanEffectParams._HeightStrength = 1.0f;     // 位移缩放因子：>1=波浪更剧烈，<1=更平缓（1.0=物理正确）
    m_pCB_OceanEffectParams._NormalStrength = 1.2f;     // 法线强度因子：>1=波浪光照更"锐"，<1=光照更柔和

    // ---- 深度衰减（远处水面平坦化）----
    m_pCB_OceanEffectParams._DisplaceDepthAttenuation = 10;    // 位移深度衰减指数：越大远处波浪消失越快（10=快速衰减）
    m_pCB_OceanEffectParams._FoamDepthAttenuation     = 20;    // 泡沫深度衰减指数：越大远处泡沫消失越快

    // ---- 水面材质 ----
    m_pCB_OceanEffectParams._Roughness     = 0.05f;     // 水面粗糙度：0=镜面反射，1=完全漫反射（0.05=光滑的镜面感）
    m_pCB_OceanEffectParams._FoamRoughness = 0.1f;      // 泡沫粗糙度：泡沫区域更粗糙，高光更散

    // ---- 环境光与反射 ----
    m_pCB_OceanEffectParams._AmbientDensity           = 0.15f;   // 环境散射密度：阴天/阴影下水的暗蓝绿色亮度
    m_pCB_OceanEffectParams._EnvironmentLightStrength = 1.0f;    // 天空盒反射强度：>1=倒影更亮，<1=倒影更暗

    // ---- 次表面散射（水体蓝绿色的来源）----
    m_pCB_OceanEffectParams._ScatterStrength        = 0.1f;      // 散射强度（k2/k3 项）：越大水体越亮、越不透明
    m_pCB_OceanEffectParams._ScatterShadowStrength  = 0.1f;      // 阴影区散射强度（k3 项）：光源散射补偿
    m_pCB_OceanEffectParams._WavePeakScatterStrength = 2.0f;     // 浪尖散射强度（k1 项）：浪尖处光穿透最深，散射最强
    // 散射颜色（水体色相）
    m_pCB_OceanEffectParams._ScatterColor     = XMFLOAT4(0.0f, 0.67f, 1.0f, 1.0f);  // 基础散射色（偏蓝）
    m_pCB_OceanEffectParams._ScatterPeakColor = XMFLOAT4(0.0f, 0.67f, 1.0f, 1.0f);  // 浪尖散射色（更亮的蓝绿色）

    // ---- 泡沫 ----
    m_pCB_OceanEffectParams._EdgeFoamPower = 0.2f;            // 边缘泡沫强度：物体与水面交界处的泡沫加成（越大越多）
    m_pCB_OceanEffectParams._ShadowIntensity = 0.2f;          // 阴影环境光补偿：阴影区域泡沫的最低亮度（0=阴影区完全无泡沫）
    m_pCB_OceanEffectParams._FoamColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f); // 泡沫颜色（白色）

    // ---- 雾效（大气散射模拟）----
    m_pCB_OceanEffectParams._FogDensity = 0.015f;              // 雾密度：越大远处水面越快融入雾色（米制适配，0.01≈轻微远处雾）
    m_pCB_OceanEffectParams._FogPower   = 2.0f;               // 雾曲线指数：>1=雾在远处突然出现，<1=线性渐变
    m_pCB_OceanEffectParams._FogColor   = XMFLOAT4(0.8f, 0.9f, 0.95f, 1.0f); // 雾颜色（淡蓝灰 = 水天线融合色）

    // ---- 法线细节变化 ----
    m_pCB_OceanEffectParams._VarMaskRange   = 10;             // 变化掩码距离范围：越大远距离法线混合越多
    m_pCB_OceanEffectParams._VarMaskPower   = 0.4f;           // 变化掩码对比度：越大高频区域越突出
    m_pCB_OceanEffectParams._VarMaskTexScale = 6.0f;          // 变化掩码纹理缩放：越大掩码图案越密

    // ---- 曲面细分 ----
    m_pCB_OceanEffectParams._TessEdgeLength = 16;             // 细分密度：屏幕像素边长阈值，越小=越密（16=平衡画质/性能）
    m_pCB_OceanEffectParams._TessNear       = 1.0f;           // 细分最近距离（暂未使用）
    m_pCB_OceanEffectParams._TessFar        = 500.0f;         // 细分最远距离（米），超过不细分 = 节省性能
}