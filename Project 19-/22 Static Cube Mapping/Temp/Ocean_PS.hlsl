struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float height : TEXCOORD1;
};

float4 PS(PSInput input) : SV_TARGET
{
    return float4(1, 0, 0, 1); // 红色代表 Y 位移强度
}