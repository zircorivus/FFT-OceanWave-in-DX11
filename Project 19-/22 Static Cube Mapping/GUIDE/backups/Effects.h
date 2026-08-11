//***************************************************************************************
// Effects.h by X_Jun(MKXJun) (C) 2018-2022 All Rights Reserved.
// Licensed under the MIT License.
//
// 简易特效管理框架
// Simple effect management framework.
//***************************************************************************************

#ifndef EFFECTS_H
#define EFFECTS_H

#include <IEffect.h>
#include <Material.h>
#include <MeshData.h>
#include <LightHelper.h>

class BasicEffect : public IEffect, public IEffectTransform,
    public IEffectMaterial, public IEffectMeshData
{
public:
    BasicEffect();
    virtual ~BasicEffect() override;

    BasicEffect(BasicEffect&& moveFrom) noexcept;
    BasicEffect& operator=(BasicEffect&& moveFrom) noexcept;

    // 获取单例
    static BasicEffect& Get();

    // 初始化所需资源
    bool InitAll(ID3D11Device* device);

    //
    // IEffectTransform
    //

    void XM_CALLCONV SetWorldMatrix(DirectX::FXMMATRIX W) override;
    void XM_CALLCONV SetViewMatrix(DirectX::FXMMATRIX V) override;
    void XM_CALLCONV SetProjMatrix(DirectX::FXMMATRIX P) override;

    //
    // IEffectMaterial
    //

    void SetMaterial(const Material& material) override;

    //
    // IEffectMeshData
    //

    MeshDataInput GetInputData(const MeshData& meshData) override;


    //
    // BasicEffect
    //

    // 默认状态来绘制
    void SetRenderDefault();

    void SetTextureCube(ID3D11ShaderResourceView* textureCube);
    
    // 各种类型灯光允许的最大数目
    static const int maxLights = 5;

    void SetDirLight(uint32_t pos, const DirectionalLight& dirLight);
    void SetPointLight(uint32_t pos, const PointLight& pointLight);
    void SetSpotLight(uint32_t pos, const SpotLight& spotLight);

    void SetEyePos(const DirectX::XMFLOAT3& eyePos);

    void SetReflectionEnabled(bool enabled);

    // 应用常量缓冲区和纹理资源的变更
    void Apply(ID3D11DeviceContext* deviceContext) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

class SkyboxEffect : public IEffect, public IEffectTransform,
    public IEffectMaterial, public IEffectMeshData
{
public:
    SkyboxEffect();
    virtual ~SkyboxEffect() override;

    SkyboxEffect(SkyboxEffect&& moveFrom) noexcept;
    SkyboxEffect& operator=(SkyboxEffect&& moveFrom) noexcept;

    // 获取单例
    static SkyboxEffect& Get();

    // 初始化所需资源
    bool InitAll(ID3D11Device* device);

    //
    // IEffectTransform
    //

    // 无用
    void XM_CALLCONV SetWorldMatrix(DirectX::FXMMATRIX W) override;

    void XM_CALLCONV SetViewMatrix(DirectX::FXMMATRIX V) override;
    void XM_CALLCONV SetProjMatrix(DirectX::FXMMATRIX P) override;

    //
    // IEffectMaterial
    //

    void SetMaterial(const Material& material) override;

    //
    // IEffectMeshData
    //

    MeshDataInput GetInputData(const MeshData& meshData) override;

    //
    // SkyboxEffect
    //

    void SetRenderDefault();

    // 应用常量缓冲区和纹理资源的变更
    void Apply(ID3D11DeviceContext* deviceContext) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

class OceanEffect : public IEffect, public IEffectTransform,
    public IEffectMaterial, public IEffectMeshData
{
public:
    OceanEffect(); ~OceanEffect();

    //初始化所需资源
    bool InitAll(ID3D11Device* device);

    void SetRenderDefault();

    void XM_CALLCONV SetWorldMatrix(DirectX::FXMMATRIX W) override;

    void XM_CALLCONV SetViewMatrix(DirectX::FXMMATRIX V) override;
    void XM_CALLCONV SetProjMatrix(DirectX::FXMMATRIX P) override;

    void SetMaterial(const Material& material) override;

    MeshDataInput GetInputData(const MeshData& meshData) override;

    // 应用常量缓冲区和纹理资源的变更
    void Apply(ID3D11DeviceContext* deviceContext) override;

    // 纹理资源
    void SetDisplacementTextures(ID3D11ShaderResourceView* texs[4]);
    void SetSlopeTextures(ID3D11ShaderResourceView* texs[4]);
    void SetVariationMask(ID3D11ShaderResourceView* tex);
    void SetSkyboxTexture(ID3D11ShaderResourceView* tex);
    // 光源和相机
    void SetDirLight(uint32_t pos, const DirectionalLight& dirLight);
    void SetEyePos(const DirectX::XMFLOAT3& eyePos);
    // 分层参数
    void SetTiles(const float tiles[4]);
    void SetLayerContributes(const float layerContributions[4]);
    void SetHeightStrength(float heightStrength);
    void SetNormalStrength(float normalStrength);
    void SetDisplaceDepthAttenuation(float displaceDepthAttenuation);
    void SetFoamDepthAttenuation(float foamDepthAttenuation);
    // 材质
    void SetRoughness(float roughness, float foamRoughness);
    void SetAmbientDensity(float ambientDensity);
    void SetEnvironmentLightStrength(float environmentLightStrength);
    // 散射
    void SetScatterStrengths(float scatterStrength, float scatterShadowStrength, float wavePeakScatterStrength);
    void SetScatterColors(const DirectX::XMFLOAT4& scatterColor, const DirectX::XMFLOAT4& scatterPeakColor);
    // 泡沫
    void SetEdgeFoamPower(float edgeFoamPower);
    void SetShadowIntensity(float shadowIntensity);
    void SetFoamColor(const DirectX::XMFLOAT4& foamColor);
    // 雾
    void SetFogParams(float fogDensity, float fogPower, const DirectX::XMFLOAT4& fogColor);
    // 法线变化掩码
    void SetVariationMaskParams(float varMaskRange, float varMaskPower, float varMaskTexScale);
    // 曲面细分
    void SetTessellationEdgeLength(float tessEdgeLength);
    void SetTessellationNear(float tessNear);
    void SetTessellationFar(float tessFar);
    void SetTessellationEnabled(bool enabled);

private:
    class Impl; 
    std::unique_ptr<Impl> pImpl;
};


#endif
