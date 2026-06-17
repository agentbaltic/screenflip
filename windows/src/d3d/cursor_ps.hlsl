// Draws the proxy-cursor sprite. The sprite texture comes from DrawIconEx(DI_NORMAL),
// which is PREMULTIPLIED alpha, so the overlay uses a premultiplied-over blend
// (SrcBlend=ONE, DestBlend=INV_SRC_ALPHA); this shader just passes the texel through.
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return gTex.Sample(gSmp, uv);
}
