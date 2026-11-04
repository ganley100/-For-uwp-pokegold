// "Sharp bilinear": nearest-neighbour look with only the seams between
// source pixels interpolated, so non-integer scales stay crisp and even.
Texture2D    frameTex : register(t0);
SamplerState linearSm : register(s0);

cbuffer Params : register(b0)
{
   float2 texSize;   // 160, 144
   float2 scale;     // output pixels per source pixel (x, y)
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
   float2 texel       = uv * texSize;
   float2 texelFloor  = floor(texel);
   float2 s           = frac(texel);
   float2 regionRange = 0.5 - 0.5 / scale;
   float2 centerDist  = s - 0.5;
   float2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * scale + 0.5;
   float3 rgb = frameTex.Sample(linearSm, (texelFloor + f) / texSize).rgb;
   return float4(rgb, 1.0);
}
