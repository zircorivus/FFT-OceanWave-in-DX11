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
    //为常量缓冲区填入初始化数据：
    m_CBFFTOceanParams._Depth = 10.0f;
    m_CBFFTOceanParams._Gravity = 9.81f;
    m_CBFFTOceanParams._FrameTime = 0.0f; //这个要单独放在Update里面更新维护
    m_CBFFTOceanParams._RepeatTime = 200.0f;
    m_CBFFTOceanParams._LowCutOff = 0.0001f;
    m_CBFFTOceanParams._HighCutOff = 9000.0f;
    m_CBFFTOceanParams._WaveSharpX = 0.5f;
    m_CBFFTOceanParams._WaveSharpY = 0.5f;

    m_CBFFTOceanParams._Resolution = 1024;
    m_CBFFTOceanParams._LengthScale0 = 4;
    m_CBFFTOceanParams._LengthScale1 = 4;
    m_CBFFTOceanParams._LengthScale2 = 4;
    m_CBFFTOceanParams._LengthScale3 = 4;

    m_CBFFTOceanParams._Seed = 28;

    m_CBFFTOceanParams._FoamBias = 0.2f;
    m_CBFFTOceanParams._FoamPower = 2.0f;
    m_CBFFTOceanParams._FoamAdd = 0.2f;
    m_CBFFTOceanParams._FoamDecayRate = 0.05f;

    m_CBFFTOceanParams._Speed = 0.5f;

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

    //填充CPU端数据（先把占位数据给写死）
    FillJonswapStruct(0, 0.4f, 1200.0f, 50.0f, 600.0f, 1.0f, 0.9f, 5, 0.8f);
    FillJonswapStruct(1, 0.4f, 1000.0f, 0.0f, 500.0f, 1.0f, 0.9f, 5, 0.8f);
    FillJonswapStruct(2, 0.2f, 800.0f, 60.0f, 400.0f, 0.98f, 0.9f, 5, 0.4f);
    FillJonswapStruct(3, 0.2f, 800.0f, 120.0f, 350.0f, 0.98f, 0.9f, 5, 0.4f);
    FillJonswapStruct(4, 0.04f, 100.0f, 260.0f, 100.0f, 0.95f, 0.8f, 3, 0.4f);
    FillJonswapStruct(5, 0.04f, 50.0f, 280.0f, 100.0f, 0.95f, 0.8f, 3, 0.4f);
    FillJonswapStruct(6, 0.1f, 10.0f, 0.0f, 40.0f, 0.8f, 0.6f, 1, 0.2f);
    FillJonswapStruct(7, 0.1f, 10.0f, 0.0f, 20.0f, 0.6f, 0.4f, 1, 0.2f);

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
}

void GameApp::InitOceanEffectParams()
{
    m_pCB_OceanEffectParams._Tile0 = 0.04f;
    m_pCB_OceanEffectParams._Tile1 = 0.06f;
    m_pCB_OceanEffectParams._Tile2 = 0.12f;
    m_pCB_OceanEffectParams._Tile3 = 0.18f;
    m_pCB_OceanEffectParams._LayerContribute0 = 0.8f;
    m_pCB_OceanEffectParams._LayerContribute1 = 0.8f;
    m_pCB_OceanEffectParams._LayerContribute2 = 0.6f;
    m_pCB_OceanEffectParams._LayerContribute3 = 0.4f;
    m_pCB_OceanEffectParams._HeightStrength = 1.0f;     // 物理正确位移（±1-2米）
    m_pCB_OceanEffectParams._NormalStrength = 1.0f;     // 加强法线，让波浪在光照下更明显
    m_pCB_OceanEffectParams._DisplaceDepthAttenuation = 10;
    m_pCB_OceanEffectParams._FoamDepthAttenuation = 20;
    m_pCB_OceanEffectParams._Roughness = 0.05f;         // 降低粗糙度，增强高光反射
    m_pCB_OceanEffectParams._FoamRoughness = 0.1f;
    m_pCB_OceanEffectParams._AmbientDensity = 0.15f;
    m_pCB_OceanEffectParams._EnvironmentLightStrength = 1.0f;  // 开启天空盒反射
    m_pCB_OceanEffectParams._ScatterStrength = 0.1f;
    m_pCB_OceanEffectParams._ScatterShadowStrength = 0.1f;
    m_pCB_OceanEffectParams._WavePeakScatterStrength = 2.0f;
    m_pCB_OceanEffectParams._ScatterColor = XMFLOAT4(0.0f, 0.67f, 1.0f, 1.0f);
    m_pCB_OceanEffectParams._ScatterPeakColor = XMFLOAT4(0.0f, 0.67f, 1.0f, 1.0f);
    m_pCB_OceanEffectParams._EdgeFoamPower = 0.2f;
    m_pCB_OceanEffectParams._ShadowIntensity = 0.2f;
    m_pCB_OceanEffectParams._FoamColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    m_pCB_OceanEffectParams._FogDensity = 0.01f;
    m_pCB_OceanEffectParams._FogPower = 2.0f;
    m_pCB_OceanEffectParams._FogColor = XMFLOAT4(0.4f, 0.45f, 0.5f, 1.0f);
    m_pCB_OceanEffectParams._VarMaskRange = 10;
    m_pCB_OceanEffectParams._VarMaskPower = 0.4f;
    m_pCB_OceanEffectParams._VarMaskTexScale = 6.0f;
    m_pCB_OceanEffectParams._TessEdgeLength = 16;
    m_pCB_OceanEffectParams._TessNear = 1.0f;
    m_pCB_OceanEffectParams._TessFar = 500.0f;   // 超过 500m 不细分（原来是 1.0 = 完全没有细分！）
}