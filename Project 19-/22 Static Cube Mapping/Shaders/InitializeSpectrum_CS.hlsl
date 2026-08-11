#include "FFT_Ocean_CS_Common.hlsli"

RWTexture2D<float4> _InitializeSpectrumTex : register(u0);

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    float seed = id.x + id.y * _Resolution + _Resolution;
    seed += _Seed;
    
    float LengthScale[] = { _LengthScale0, _LengthScale1, _LengthScale2, _LengthScale3 };
    
    {
        float halfN = _Resolution / 2.0f; //获取纹理中心坐标
        float deltaK = 2.0f * PI / LengthScale[_InitialSpectrumIndex]; //像素单位之间的频率差
        float2 k = (id.xy - halfN) * deltaK;
        float kLength = length(k);
        
        seed += _InitialSpectrumIndex + hash(seed) * 10;
        float2 gauss1 = gaussian(hash(seed), hash(seed * 2));
        float2 gauss2 = gaussian(hash(seed * 3), hash(seed * 4));

        if (_LowCutOff <= kLength && kLength <= _HighCutOff)
        {
            float omega = Dispersion(kLength);
            float kAngle = atan2(k.y, k.x);
            
            float dOmegadk = DispersionDerivative(kLength);
            
            float JONSpectrum = JONSWAP(omega, _JonswapParameters[_InitialSpectrumIndex * 2])
                * DirectionSpectrum(kAngle, omega, _JonswapParameters[_InitialSpectrumIndex * 2])
                * ShortWaveFade(kLength, _JonswapParameters[_InitialSpectrumIndex * 2]);
                
            if (_JonswapParameters[2 * _InitialSpectrumIndex + 1].scale > 0)
            {
                JONSpectrum += JONSWAP(omega, _JonswapParameters[_InitialSpectrumIndex * 2 + 1])
                    * DirectionSpectrum(kAngle, omega, _JonswapParameters[_InitialSpectrumIndex * 2 + 1])
                    * ShortWaveFade(kLength, _JonswapParameters[_InitialSpectrumIndex * 2 + 1]);
            }
            float2 H0 = float2(gauss1.x, gauss2.y) * sqrt(JONSpectrum * 2 * abs(dOmegadk) / kLength * deltaK * deltaK);
            _InitializeSpectrumTex[id.xy] = float4(H0, 0, 0);
        }
        else
        {
            _InitializeSpectrumTex[id.xy] = 0.0f;
        }
    }
}