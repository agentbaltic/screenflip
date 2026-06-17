// Full-screen triangle. Emits UVs covering the screen; the horizontal mirror is
// applied in the pixel shader. No vertex buffer — driven by SV_VertexID.
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2); // (0,0) (2,0) (0,2)
    o.uv  = uv;                                   // uv.y = 0 at top
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
