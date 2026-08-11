struct VSInput
{
    float3 position : POSITION; // ← 注意：输入用 POSITION，不是 POSITIONT
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct v2h
{
    float4 vertex : POSITIONT;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 shadow : TEXCOORD1;
};

struct h2d
{
    float4 vertex : INTERNALTESSPOS;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 shadow : TEXCOORD1;
};

struct d2g
{
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float clipDepth : TEXCOORD3;
    float viewDepth : TEXCOORD4;
    float2 screenUV : TEXCOORD5;
    float4 shadow : TEXCOORD6;
};

struct g2p
{
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float clipDepth : TEXCOORD3;
    float viewDepth : TEXCOORD4;
    float2 screenUV : TEXCOORD5;
    float4 shadow : TEXCOORD6;
    float2 barycentricCoordinates : TEXCOORD7;
};