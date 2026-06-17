// Samples the captured workspace texture HORIZONTALLY MIRRORED — the Direct3D
// analogue of the macOS CALayer scaleX:-1 transform.
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 mirrored = float2(1.0 - uv.x, uv.y); // flip left<->right
    return gTex.Sample(gSmp, mirrored);
}
