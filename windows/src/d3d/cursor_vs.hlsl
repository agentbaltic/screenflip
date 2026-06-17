// Positions the proxy-cursor quad. The CPU passes the sprite's destination
// rectangle in normalized device coordinates (left, top, right, bottom); we emit
// a triangle-strip quad with straight UVs (any image mirroring is baked into the
// sprite texture itself, so UVs stay normal). Driven by SV_VertexID.
cbuffer CursorCB : register(b0) {
    float4 gRectNDC; // (left, top, right, bottom) in NDC; top > bottom
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID) {
    // strip order: 0=(L,T) 1=(R,T) 2=(L,B) 3=(R,B)
    float2 uv = float2(vid & 1, (vid >> 1) & 1);
    float x = lerp(gRectNDC.x, gRectNDC.z, uv.x);
    float y = lerp(gRectNDC.y, gRectNDC.w, uv.y);
    VSOut o;
    o.pos = float4(x, y, 0, 1);
    o.uv  = uv; // uv.y = 0 at sprite top
    return o;
}
