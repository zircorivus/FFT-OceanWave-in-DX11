#ifndef GAMEAPP_H
#define GAMEAPP_H

#include <random>
#include <WinMin.h>
#include "d3dApp.h"
#include "Effects.h"
#include <CameraController.h>
#include <RenderStates.h>
#include <GameObject.h>
#include <Texture2D.h>
#include <Buffer.h>
#include <Collision.h>
#include <ModelManager.h>
#include <TextureManager.h>

class GameApp : public D3DApp
{
public:
    GameApp(HINSTANCE hInstance, const std::wstring& windowName, int initWidth, int initHeight);
    ~GameApp();

    bool Init();
    void OnResize();
    void UpdateScene(float dt);
    void DrawScene();

private:
    bool InitEffect();
    bool InitResource(); //这个是纹理的格式 一般就是RGBA32位
    void CreateTextureUAVSRV(UINT width, UINT height, DXGI_FORMAT format, 
        ID3D11Texture2D** ppTex, ID3D11UnorderedAccessView** ppUAV, ID3D11ShaderResourceView** ppSRV, 
        bool needSRV, bool needMips);
    void CreateComputeShader(const std::wstring& fileName, ComPtr<ID3D11ComputeShader>& shader);
    void CreateVertexShader(const std::wstring& fileName, ComPtr<ID3D11VertexShader>& shader);
    void CreatePixelShader(const std::wstring& fileName, ComPtr<ID3D11PixelShader>& shader);
    void DispatchInitializeSpectrum_CS(int layer);
    void DispatchPackSpectrumConjugate_CS(int layer);
    void DispatchUpdateSpectrum_CS(int layer);
    void DispatchHorizontalIFFT_CS(int layer);
    void DispatchVerticalIFFT_CS(int layer);
    void DispatchAssembleTextures_CS(int layer);

    void FillJonswapStruct(int index, float scale, float windSpeed, float windDirection, float fetch,
        float spreadBlend, float swell, float peakEnhancement, float shortWavesFade);
    float JonswapPeakFrequency(float fetch, float windSpeed);
    float JonswapAlpha(float fetch, float windSpeed);

    void FillOceanEffectParams();
    void FillOceanEffectSRVs();
    void InitOceanEffectParams();
private:

    TextureManager m_TextureManager;
    ModelManager m_ModelManager;

    BasicEffect m_BasicEffect;		            			    // 对象渲染特效管理
    SkyboxEffect m_SkyboxEffect;							    // 天空盒特效管理
    OceanEffect m_OceanEffect;

    std::unique_ptr<Depth2D> m_pDepthTexture;                   // 深度缓冲区

    GameObject m_Sphere;										// 球
    GameObject m_Skybox;                                        // 天空盒
    GameObject m_OceanSurface;                              // 水面

    std::shared_ptr<FirstPersonCamera> m_pCamera;			    // 摄像机
    FirstPersonCameraController m_CameraController;             // 摄像机控制器 

    // 纹理对象（每组纹理的底层ID3D11Texture2D）
    std::vector<ComPtr<ID3D11Texture2D>> m_InitialSpectrumTexs; //4张
    std::vector<ComPtr<ID3D11Texture2D>> m_SpectrumTexs; //8张
    std::vector<ComPtr<ID3D11Texture2D>> m_DisplacementTexs; //4张
    std::vector<ComPtr<ID3D11Texture2D>> m_SlopeTexs; //4张
    ComPtr<ID3D11Texture2D> m_VariationMaskTex; 
    ComPtr<ID3D11Texture2D> m_BuoyancyDataTex;
    // UAV（只给CS使用的纹理组）
    std::vector<ComPtr<ID3D11UnorderedAccessView>> m_InitialSpectrumUAVs; //4张
    std::vector<ComPtr<ID3D11UnorderedAccessView>> m_SpectrumUAVs; //8张
    std::vector<ComPtr<ID3D11UnorderedAccessView>> m_DisplacementUAVs; //4张
    std::vector<ComPtr<ID3D11UnorderedAccessView>> m_SlopeUAVs; //4张
    ComPtr<ID3D11UnorderedAccessView> m_VariationMaskUAV;
    ComPtr<ID3D11UnorderedAccessView> m_BuoyancyDataUAV;
    // SRV（给渲染shader，将Displacement/Slope/VariationMask传进去渲染shader）
    std::vector<ComPtr<ID3D11ShaderResourceView>> m_DisplacementSRVs; //4张
    std::vector<ComPtr<ID3D11ShaderResourceView>> m_SlopeSRVs; //4张
    ComPtr<ID3D11ShaderResourceView> m_VariationMaskSRV; //1张

    // AI-GENERATED: 场景深度拷贝纹理（用于边缘泡沫 — 水面渲染前从主深度缓冲拷贝场景深度）
    ComPtr<ID3D11Texture2D> m_pSceneDepthCopyTex;          // 与主深度同格式（R24G8_TYPELESS）
    ComPtr<ID3D11ShaderResourceView> m_pSceneDepthCopySRV;  // 可被 PS 读取为 R24_UNORM_X8_TYPELESS
    void CopyDepthBeforeOcean();                            // 在水面渲染前执行深度拷贝

    // 定义水面效果常量缓冲区的结构
    struct CB_FFTOceanParams
    {
        float _Depth;
        float _Gravity;
        float _FrameTime;
        float _RepeatTime;
        float _LowCutOff;
        float _HighCutOff;
        float _WaveSharpX;
        float _WaveSharpY;
        int _Resolution;
        int _LengthScale0;
        int _LengthScale1;
        int _LengthScale2;
        int _LengthScale3;
        int _Seed;
        float _FoamBias;
        float _FoamPower;
        float _FoamAdd;
        float _FoamDecayRate;

        int _InitialSpectrumIndex;
        int _SpectrumIndex;
        int _DisplacementIndex;
        int _SlopeIndex;

        float _Speed;

        float _Pad1;
    };

    CB_FFTOceanParams m_CBFFTOceanParams; //水面效果的常量缓冲区成员
    static_assert(sizeof(CB_FFTOceanParams) == 96, "CB_FFTOceanParams size must be 96 bytes");
    ComPtr<ID3D11Buffer> m_pCB_FFTOcean; //常量缓冲区对象

    //定义JONSWAP参数的结构体
    struct JONSWAP
    {
        float scale;
        float angle;
        float spreadBlend;
        float swell;
        float alpha;
        float peakOmega;
        float gamma;
        float shortWavesFade;
    };
    JONSWAP jonswapData[8] = {}; //结构体成员
    ComPtr<ID3D11Buffer> m_pJONSWAPBuffer; //结构体缓冲区
    ComPtr<ID3D11ShaderResourceView> m_JONSWAPSRV; //结构体缓冲区的SRV

    //计算着色器对象成员
    ComPtr<ID3D11ComputeShader> m_pCS_InitialSpectrumTex;
    ComPtr<ID3D11ComputeShader> m_pCS_UpdateSpectrumTex;
    ComPtr<ID3D11ComputeShader> m_pCS_PackSpectrumConjugate;
    ComPtr<ID3D11ComputeShader> m_pCS_HorizontalIFFT;
    ComPtr<ID3D11ComputeShader> m_pCS_VerticalIFFT;
    ComPtr<ID3D11ComputeShader> m_pCS_AssembleTextures;

    // 水面特效类结构体定义（仅 CPU 端缓存，最终通过 OceanEffect::CBOceanParams 上传 GPU）
    struct CB_OceanEffectParams
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
        DirectX::XMFLOAT4 _ScatterColor;
        DirectX::XMFLOAT4 _ScatterPeakColor;
        DirectX::XMFLOAT4 _FoamColor;
        DirectX::XMFLOAT4 _FogColor;
    };
    // 水面特效类结构体缓存
    CB_OceanEffectParams m_pCB_OceanEffectParams;
    static_assert(sizeof(CB_OceanEffectParams) == 192, "CB_OceanEffectParams size");
};


#endif