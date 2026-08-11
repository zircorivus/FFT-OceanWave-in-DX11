#include "Effects.h"           // OceanEffect 类声明
#include <EffectHelper.h>      // EffectHelper::GetConstantBufferVariable 等
#include <RenderStates.h>      // RSDefault, DSSDefault 等
#include <Vertex.h>            // VertexPosNormalTex::GetInputLayout()
#include <DXTrace.h>           // HR() 宏
using namespace DirectX;

class OceanEffect::Impl
{
public:
    // 必须显式指定
    Impl() {
        XMStoreFloat4x4(&m_World, XMMatrixIdentity());
        XMStoreFloat4x4(&m_View, XMMatrixIdentity());
        XMStoreFloat4x4(&m_Proj, XMMatrixIdentity());
		memset(&m_CacheOceanParams, 0, sizeof(m_CacheOceanParams));
		memset(&m_CacheChangeRarely, 0, sizeof(m_CacheChangeRarely));
    }
    ~Impl() = default;
public:
    template<class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    std::unique_ptr<EffectHelper> m_pEffectHelper; //Effect助手

    std::shared_ptr<IEffectPass> m_pCurrEffectPass; //渲染Pass
    ComPtr<ID3D11InputLayout> m_pCurrInputLayout; //输入布局 
    D3D11_PRIMITIVE_TOPOLOGY m_CurrTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    ComPtr<ID3D11InputLayout> m_pVertexPosNormalTexLayout; //输入布局

    XMFLOAT4X4 m_World, m_View, m_Proj; //转换矩阵
    DirectX::XMFLOAT3 m_EyePos;

public:
    // ---- b0 for HS/DS only（框架不管理 HS/DS，需手动创建）----
    struct alignas(16) CBOceanPerFrame
    {
        DirectX::XMFLOAT4X4 g_World;
        DirectX::XMFLOAT4X4 g_ViewProj;
        DirectX::XMFLOAT3   g_EyePosW;
        float               g_Pad;
    };
    ComPtr<ID3D11Buffer> m_pCBPerFrame_HSDS; //专门提供给HS/DS的b0缓冲区

    // ---- b1 / b2 ----
    ComPtr<ID3D11Buffer> m_pCBOceanParams;
    ComPtr<ID3D11Buffer> m_pCBChangeRarely;

    // HLSL cbuffer 中数组元素按 16 字节对齐，必须拆成独立 float 变量
    struct alignas(16) CBOceanParams
    {
        float _Tile0; float _Tile1; float _Tile2; float _Tile3;
        float _LayerContribute0; float _LayerContribute1; float _LayerContribute2; float _LayerContribute3;
        float _HeightStrength;
        float _NormalStrength;
        float _DisplaceDepthAttenuation;
        float _FoamDepthAttenuation;
        float _Roughness;
        float _FoamRoughness;
        float _AmbientDensity;
        float _EnvironmentLightStrength;
        float _ScatterStrength;
        float _ScatterShadowStrength;
        float _WavePeakScatterStrength;
        float _EdgeFoamPower;
        float _ShadowIntensity;
        float _FogDensity;
        float _FogPower;
        float _VarMaskRange;
        float _VarMaskPower;
        float _VarMaskTexScale;
        float _TessEdgeLength;
        float _TessNear;
        float _TessFar;
        float _Pad0; float _Pad1; float _Pad2;
        XMFLOAT4 _ScatterColor;
        XMFLOAT4 _ScatterPeakColor;
        XMFLOAT4 _FoamColor;
        XMFLOAT4 _FogColor;
    };
    struct alignas(16) CBOceanChangeRarely
    {
        DirectionalLight g_DirLight[5];
        PointLight g_PointLight[5];
        SpotLight g_SpotLight[5];
    };
    //C++端的缓存数据
    CBOceanParams m_CacheOceanParams;
    CBOceanChangeRarely m_CacheChangeRarely;

    //SRV纹理
    ComPtr<ID3D11ShaderResourceView> m_pDisplacementSRVs[4]; //位移纹理SRV
    ComPtr<ID3D11ShaderResourceView> m_pSlopeSRVs[4]; //法线纹理SRV
    ComPtr<ID3D11ShaderResourceView> m_pVariationMaskSRV; //泡沫纹理SRV
    ComPtr<ID3D11ShaderResourceView> m_pSkyboxSRV; //天空盒纹理SRV

    //曲面细分状态
    bool m_TessellationEnabled = true;

    //着色器
    ComPtr<ID3D11HullShader> m_pHullShader;
    ComPtr<ID3D11DomainShader> m_pDomainShader;
    ComPtr<ID3D11GeometryShader> m_pGeometryShader;
};

OceanEffect::OceanEffect()
{
    pImpl = std::make_unique<OceanEffect::Impl>();
}

OceanEffect::~OceanEffect()
{
}

bool OceanEffect::InitAll(ID3D11Device* device)
{
    if (!device)
        return false;

    if (!RenderStates::IsInit())
        throw std::exception("RenderStates need to be initialized first!");

    pImpl->m_pEffectHelper = std::make_unique<EffectHelper>();

    Microsoft::WRL::ComPtr<ID3DBlob> blob;

    // 创建顶点着色器
    HR(pImpl->m_pEffectHelper->CreateShaderFromFile("OceanVS", L"Shaders\\Ocean_VS.cso",
        device, "VS", "vs_5_0", nullptr, blob.GetAddressOf()));
    // 创建顶点布局
    HR(device->CreateInputLayout(VertexPosNormalTex::GetInputLayout(), ARRAYSIZE(VertexPosNormalTex::GetInputLayout()),
        blob->GetBufferPointer(), blob->GetBufferSize(), pImpl->m_pVertexPosNormalTexLayout.ReleaseAndGetAddressOf()));
    // 创建像素着色器
    HR(pImpl->m_pEffectHelper->CreateShaderFromFile("OceanPS", L"Shaders\\Ocean_PS.cso", device));
    // 创建外壳着色器：因为框架不能自动创建HS/DS/GS，所以需要我们手动创建
    if (SUCCEEDED(D3DReadFileToBlob(L"Shaders\\Ocean_HS.cso", blob.GetAddressOf())))
        device->CreateHullShader(blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, pImpl->m_pHullShader.GetAddressOf());
	// 创建域着色器：同上
    if (SUCCEEDED(D3DReadFileToBlob(L"Shaders\\Ocean_DS.cso", blob.GetAddressOf())))
        device->CreateDomainShader(blob->GetBufferPointer(), blob->GetBufferSize(),
            nullptr, pImpl->m_pDomainShader.GetAddressOf());

	// 创建常量缓冲区：b0 for HS/DS + b1 + b2
	// VS/PS 的 b0 由框架管理，但 HS/DS 不在框架 Pass 中，需要手动创建
	static_assert(sizeof(Impl::CBOceanPerFrame) == 144, "CBOceanPerFrame size");
	D3D11_BUFFER_DESC cbd;
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.MiscFlags = 0;
	cbd.StructureByteStride = 0;
	cbd.ByteWidth = sizeof(Impl::CBOceanPerFrame);
	device->CreateBuffer(&cbd, nullptr, pImpl->m_pCBPerFrame_HSDS.GetAddressOf());

	cbd.ByteWidth = sizeof(Impl::CBOceanParams);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.MiscFlags = 0;
	cbd.StructureByteStride = 0;
	cbd.ByteWidth = sizeof(Impl::CBOceanParams);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.MiscFlags = 0;
	cbd.StructureByteStride = 0;
	cbd.ByteWidth = sizeof(Impl::CBOceanParams);
	device->CreateBuffer(&cbd, nullptr, pImpl->m_pCBOceanParams.GetAddressOf());
	cbd.ByteWidth = sizeof(Impl::CBOceanChangeRarely);
	device->CreateBuffer(&cbd, nullptr, pImpl->m_pCBChangeRarely.GetAddressOf());
	//采样器
    pImpl->m_pEffectHelper->SetSamplerStateByName("g_Sampler", 
		RenderStates::SSLinearWrap.Get());
    // 创建通道
    EffectPassDesc passDesc;
    passDesc.nameVS = "OceanVS";
    passDesc.namePS = "OceanPS";
    HR(pImpl->m_pEffectHelper->AddEffectPass("Ocean", device, &passDesc));
    {
        auto pPass = pImpl->m_pEffectHelper->GetEffectPass("Ocean");
        pPass->SetRasterizerState(RenderStates::RSNoCull.Get());
        pPass->SetDepthStencilState(RenderStates::DSSLessEqual.Get(), 0);
    }
    return true;
}

void XM_CALLCONV OceanEffect::SetWorldMatrix(DirectX::FXMMATRIX W)
{
    XMStoreFloat4x4(&pImpl->m_World, W);
}

void XM_CALLCONV OceanEffect::SetViewMatrix(DirectX::FXMMATRIX V)
{
    XMStoreFloat4x4(&pImpl->m_View, V);
}

void XM_CALLCONV OceanEffect::SetProjMatrix(DirectX::FXMMATRIX P)
{
    XMStoreFloat4x4(&pImpl->m_Proj, P);
}

void OceanEffect::SetRenderDefault()
{
    pImpl->m_pCurrEffectPass = pImpl->m_pEffectHelper->GetEffectPass("Ocean");
    pImpl->m_pCurrInputLayout = pImpl->m_pVertexPosNormalTexLayout;
    pImpl->m_CurrTopology = pImpl->m_TessellationEnabled
        ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
        : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void OceanEffect::SetMaterial(const Material& material)
{
    
}

MeshDataInput OceanEffect::GetInputData(const MeshData& meshData)
{
    MeshDataInput input;
    input.pInputLayout = pImpl->m_pCurrInputLayout.Get();
    input.topology = pImpl->m_CurrTopology;
    input.pVertexBuffers = {
        meshData.m_pVertices.Get(),
        meshData.m_pNormals.Get(),
        meshData.m_pTexcoordArrays.empty() ? nullptr : meshData.m_pTexcoordArrays[0].Get(),
        nullptr
    };
    input.strides = { 12, 12, 8 };
    input.offsets = { 0, 0, 0 };

    input.pIndexBuffer = meshData.m_pIndices.Get();
    input.indexCount = meshData.m_IndexCount;

    return input;
}

// 纹理资源
void OceanEffect::SetDisplacementTextures(ID3D11ShaderResourceView* texs[4]) 
{
    for (int i = 0; i < 4; ++i) pImpl->m_pDisplacementSRVs[i] = texs[i];
}

void OceanEffect::SetSlopeTextures(ID3D11ShaderResourceView* texs[4])
{
    for (int i = 0; i < 4; ++i) pImpl->m_pSlopeSRVs[i] = texs[i];
}

void OceanEffect::SetVariationMask(ID3D11ShaderResourceView* tex)
{
    pImpl->m_pVariationMaskSRV = tex;
}

void OceanEffect::SetSkyboxTexture(ID3D11ShaderResourceView* tex)
{
    pImpl->m_pSkyboxSRV = tex;
}

void OceanEffect::SetDirLight(uint32_t pos, const DirectionalLight& dirLight)
{
    //pImpl->m_pEffectHelper->GetConstantBufferVariable("g_DirLight")->SetRaw(&dirLight, (sizeof dirLight) * pos, sizeof dirLight);
    if (pos < 5)
    {
        pImpl->m_CacheChangeRarely.g_DirLight[pos] = dirLight;
    }
}

void OceanEffect::SetEyePos(const DirectX::XMFLOAT3& eyePos)
{
    pImpl->m_EyePos = eyePos;  // 给手动 HS/DS b0
    pImpl->m_pEffectHelper->GetConstantBufferVariable("g_EyePosW")->SetFloatVector(3, reinterpret_cast<const float*>(&eyePos));
}
// 分层参数
void OceanEffect::SetTiles(const float tiles[4])
{
    pImpl->m_CacheOceanParams._Tile0 = tiles[0];
    pImpl->m_CacheOceanParams._Tile1 = tiles[1];
    pImpl->m_CacheOceanParams._Tile2 = tiles[2];
    pImpl->m_CacheOceanParams._Tile3 = tiles[3];
}

void OceanEffect::SetLayerContributes(const float layerContributions[4])
{
    pImpl->m_CacheOceanParams._LayerContribute0 = layerContributions[0];
    pImpl->m_CacheOceanParams._LayerContribute1 = layerContributions[1];
    pImpl->m_CacheOceanParams._LayerContribute2 = layerContributions[2];
    pImpl->m_CacheOceanParams._LayerContribute3 = layerContributions[3];
}

void OceanEffect::SetHeightStrength(float heightStrength)
{
    pImpl->m_CacheOceanParams._HeightStrength = heightStrength;
}

void OceanEffect::SetNormalStrength(float normalStrength)
{
    pImpl->m_CacheOceanParams._NormalStrength = normalStrength;
}

void OceanEffect::SetDisplaceDepthAttenuation(float displaceDepthAttenuation)
{
    pImpl->m_CacheOceanParams._DisplaceDepthAttenuation = displaceDepthAttenuation;
}

void OceanEffect::SetFoamDepthAttenuation(float foamDepthAttenuation)
{
    pImpl->m_CacheOceanParams._FoamDepthAttenuation = foamDepthAttenuation;
}
// 材质
void OceanEffect::SetRoughness(float roughness, float foamRoughness)
{
    pImpl->m_CacheOceanParams._Roughness = roughness;
    pImpl->m_CacheOceanParams._FoamRoughness = foamRoughness;
}

void OceanEffect::SetAmbientDensity(float ambientDensity)
{
    pImpl->m_CacheOceanParams._AmbientDensity = ambientDensity;
}

void OceanEffect::SetEnvironmentLightStrength(float environmentLightStrength)
{
    pImpl->m_CacheOceanParams._EnvironmentLightStrength = environmentLightStrength;

}
// 散射
void OceanEffect::SetScatterStrengths(float scatterStrength, float scatterShadowStrength, float wavePeakScatterStrength)
{
    pImpl->m_CacheOceanParams._ScatterStrength = scatterStrength;
    pImpl->m_CacheOceanParams._ScatterShadowStrength = scatterShadowStrength;
    pImpl->m_CacheOceanParams._WavePeakScatterStrength = wavePeakScatterStrength;
}

void OceanEffect::SetScatterColors(const DirectX::XMFLOAT4& scatterColor, const DirectX::XMFLOAT4& scatterPeakColor)
{
    pImpl->m_CacheOceanParams._ScatterColor = scatterColor;
    pImpl->m_CacheOceanParams._ScatterPeakColor = scatterPeakColor;
}
// 泡沫
void OceanEffect::SetEdgeFoamPower(float edgeFoamPower)
{
    pImpl->m_CacheOceanParams._EdgeFoamPower = edgeFoamPower;
}

void OceanEffect::SetShadowIntensity(float shadowIntensity)
{
    pImpl->m_CacheOceanParams._ShadowIntensity = shadowIntensity;
}

void OceanEffect::SetFoamColor(const DirectX::XMFLOAT4& foamColor)
{
    pImpl->m_CacheOceanParams._FoamColor = foamColor;
}
// 雾
void OceanEffect::SetFogParams(float fogDensity, float fogPower, const DirectX::XMFLOAT4& fogColor)
{
    pImpl->m_CacheOceanParams._FogDensity = fogDensity;
    pImpl->m_CacheOceanParams._FogPower = fogPower;
    pImpl->m_CacheOceanParams._FogColor = fogColor;
}
// 法线变化掩码
void OceanEffect::SetVariationMaskParams(float varMaskRange, float varMaskPower, float varMaskTexScale)
{
    pImpl->m_CacheOceanParams._VarMaskRange = varMaskRange;
    pImpl->m_CacheOceanParams._VarMaskPower = varMaskPower;
    pImpl->m_CacheOceanParams._VarMaskTexScale = varMaskTexScale;
}
// 曲面细分
void OceanEffect::SetTessellationEdgeLength(float tessEdgeLength)
{
    pImpl->m_CacheOceanParams._TessEdgeLength = tessEdgeLength;
}

void OceanEffect::SetTessellationNear(float tessNear)
{
    pImpl->m_CacheOceanParams._TessNear = tessNear;
}

void OceanEffect::SetTessellationFar(float tessFar)
{
    pImpl->m_CacheOceanParams._TessFar = tessFar;
}

void OceanEffect::SetTessellationEnabled(bool enabled)
{
    pImpl->m_TessellationEnabled = enabled;
}

// ============================================================
// AI-REFACTORED: Apply() 管线调度
// 框架管 VS/PS 的 b0/shader/sampler；
// HS/DS 的 b0/s0/shader 全部手动（框架 Pass 不含 HS/DS）
// ============================================================
void OceanEffect::Apply(ID3D11DeviceContext* deviceContext)
{
    

	D3D11_MAPPED_SUBRESOURCE mapped;

    // ---- 计算矩阵 ----
    XMMATRIX W = XMLoadFloat4x4(&pImpl->m_World);
    XMMATRIX V = XMLoadFloat4x4(&pImpl->m_View);
    XMMATRIX P = XMLoadFloat4x4(&pImpl->m_Proj);
    XMMATRIX VP = V * P;
    W  = XMMatrixTranspose(W);
    VP = XMMatrixTranspose(VP);
    auto pEffectHelper = pImpl->m_pEffectHelper.get();
    pEffectHelper->GetConstantBufferVariable("g_ViewProj")->SetFloatMatrix(4, 4, (FLOAT*)&VP);
    pEffectHelper->GetConstantBufferVariable("g_World")->SetFloatMatrix(4, 4, (FLOAT*)&W);

	// ---- 1. 上传 b1 / b2 ----
	deviceContext->Map(pImpl->m_pCBOceanParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	memcpy(mapped.pData, &pImpl->m_CacheOceanParams, sizeof(Impl::CBOceanParams));
	deviceContext->Unmap(pImpl->m_pCBOceanParams.Get(), 0);

	deviceContext->Map(pImpl->m_pCBChangeRarely.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	memcpy(mapped.pData, &pImpl->m_CacheChangeRarely, sizeof(Impl::CBOceanChangeRarely));
	deviceContext->Unmap(pImpl->m_pCBChangeRarely.Get(), 0);

    // ---- 2. Pass::Apply：框架上传 b0 并绑定 VS/PS 的着色器 + cbuffer + 采样器 ----
    if (pImpl->m_pCurrEffectPass)
        pImpl->m_pCurrEffectPass->Apply(deviceContext);

	// ---- 3. 手动上传 b0 给 HS/DS（框架不管理这两个 stage 的 cbuffer）----
	{
		Impl::CBOceanPerFrame perFrame;
		XMStoreFloat4x4(&perFrame.g_World, W);
		XMStoreFloat4x4(&perFrame.g_ViewProj, VP);
		perFrame.g_EyePosW = pImpl->m_EyePos;
		perFrame.g_Pad = 0;

		D3D11_MAPPED_SUBRESOURCE m;
		deviceContext->Map(pImpl->m_pCBPerFrame_HSDS.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
		memcpy(m.pData, &perFrame, sizeof(perFrame));
		deviceContext->Unmap(pImpl->m_pCBPerFrame_HSDS.Get(), 0);

		deviceContext->HSSetConstantBuffers(0, 1, pImpl->m_pCBPerFrame_HSDS.GetAddressOf());
		deviceContext->DSSetConstantBuffers(0, 1, pImpl->m_pCBPerFrame_HSDS.GetAddressOf());
	}
	// ---- 4. 手动绑 s0 给 HS/DS ----
	{
		ID3D11SamplerState* sampler = RenderStates::SSLinearWrap.Get();
		deviceContext->HSSetSamplers(0, 1, &sampler);
		deviceContext->DSSetSamplers(0, 1, &sampler);
	}

	// ---- 5. 手动绑 b1/b2 到全部 stage（框架的 Pass::Apply 只绑了它们的空版本）----
	ID3D11Buffer* cbs[2] = { pImpl->m_pCBOceanParams.Get(), pImpl->m_pCBChangeRarely.Get() };
	deviceContext->VSSetConstantBuffers(1, 2, cbs);
	deviceContext->HSSetConstantBuffers(1, 2, cbs);
	deviceContext->DSSetConstantBuffers(1, 2, cbs);
	deviceContext->PSSetConstantBuffers(1, 2, cbs);
	
	// 7. 曲面细分：设置 HS/DS 着色器（拓扑由 SetRenderDefault → GetInputData 控制）
	if (pImpl->m_TessellationEnabled)
	{
		deviceContext->HSSetShader(pImpl->m_pHullShader.Get(), nullptr, 0);
		deviceContext->DSSetShader(pImpl->m_pDomainShader.Get(), nullptr, 0);
	}
	else
	{
		deviceContext->HSSetShader(nullptr, nullptr, 0);
		deviceContext->DSSetShader(nullptr, nullptr, 0);
	}

    //⚡ SRV 绑定必须放在 Pass::Apply 之后，否则会被框架清空
    // 4. 绑定位移纹理 SRV（t0-t3）到 VS 和 DS
    ID3D11ShaderResourceView* dispSRVs[4] = {
        pImpl->m_pDisplacementSRVs[0].Get(), pImpl->m_pDisplacementSRVs[1].Get(),
        pImpl->m_pDisplacementSRVs[2].Get(), pImpl->m_pDisplacementSRVs[3].Get()
    };
    if (pImpl->m_TessellationEnabled)
        deviceContext->DSSetShaderResources(0, 4, dispSRVs);
    deviceContext->VSSetShaderResources(0, 4, dispSRVs);
    deviceContext->PSSetShaderResources(0, 4, dispSRVs);

    // 5. 绑定斜率纹理 SRV（t4-t7）
    ID3D11ShaderResourceView* slopeSRVs[4] = {
        pImpl->m_pSlopeSRVs[0].Get(), pImpl->m_pSlopeSRVs[1].Get(),
        pImpl->m_pSlopeSRVs[2].Get(), pImpl->m_pSlopeSRVs[3].Get()
    };
    deviceContext->PSSetShaderResources(4, 4, slopeSRVs);


    if (pImpl->m_TessellationEnabled)
        deviceContext->DSSetShaderResources(4, 4, slopeSRVs);
    // 6. 绑定 VariationMask（t8）+ 天空盒（t9）
    deviceContext->PSSetShaderResources(8, 1, pImpl->m_pVariationMaskSRV.GetAddressOf());
    // PS 侧绑定天空盒
    deviceContext->PSSetShaderResources(9, 1, pImpl->m_pSkyboxSRV.GetAddressOf());

    ID3D11Buffer* pCheckVS = nullptr;
    deviceContext->VSGetConstantBuffers(1, 1, &pCheckVS);
    if (pCheckVS == nullptr)
        OutputDebugStringA("ERROR: VS b1 is NULL after binding!\n");
    else
        OutputDebugStringA("OK: VS b1 is bound.\n");
    if (pCheckVS) pCheckVS->Release();
    ID3D11Buffer* pCheckPS = nullptr;
    deviceContext->PSGetConstantBuffers(1, 1, &pCheckPS);
    if (pCheckPS == nullptr)
        OutputDebugStringA("ERROR: PS b1 is NULL after binding!\n");
    else
        OutputDebugStringA("OK: PS b1 is bound.\n");
    if (pCheckPS) pCheckPS->Release();
}