// Link: user32.lib gdi32.lib  (cursor/icon + DIB)  d3d11.lib (texture)
#include "CursorWindow.h"
#include "DXShared.h"
#include "Log.h"

#include <cstdint>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sf {

void CursorSprite::SetFlipped(bool on) { flipped_ = on; }

bool CursorSprite::Refresh() {
    CURSORINFO ci{ sizeof(CURSORINFO) };
    HCURSOR cur = nullptr;
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) cur = ci.hCursor;

    if (cur == lastCursor_ && flipped_ == lastFlipped_ && srv_) return false;
    if (!cur) {
        // Cursor hidden: keep whatever sprite we have; seed an arrow if we have none.
        if (srv_) { lastCursor_ = cur; return false; }
        cur = LoadCursorW(nullptr, IDC_ARROW);
    }

    bool ok = Rebuild(cur);
    lastCursor_  = cur;
    lastFlipped_ = flipped_;
    return ok;
}

bool CursorSprite::Rebuild(HCURSOR cursor) {
    if (!cursor) return false;

    ICONINFO ii{};
    if (!GetIconInfo(cursor, &ii)) {
        Log::Linef(L"CursorSprite: GetIconInfo failed (%lu)", GetLastError());
        return false;
    }
    // Make sure the bitmaps are released no matter how we exit.
    struct Guard { HBITMAP a, b; ~Guard(){ if(a) DeleteObject(a); if(b) DeleteObject(b);} } g{ ii.hbmColor, ii.hbmMask };

    BITMAP bm{};
    HBITMAP measured = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (!measured || !GetObjectW(measured, sizeof(BITMAP), &bm)) return false;

    const int w = bm.bmWidth;
    // A monochrome (no-color) cursor stores AND+XOR stacked, so the mask is 2x tall.
    const int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;

    // Render the cursor into a top-down 32bpp BGRA DIB.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDC = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);
    if (!dib || !memDC || !bits) {
        if (dib) DeleteObject(dib);
        if (memDC) DeleteDC(memDC);
        return false;
    }
    HGDIOBJ old = SelectObject(memDC, dib);
    memset(bits, 0, (size_t)w * h * 4);       // fully transparent
    DrawIconEx(memDC, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();

    uint8_t* px = static_cast<uint8_t*>(bits);

    // Fallback for legacy monochrome cursors that DrawIconEx leaves with zero alpha:
    // if nothing is opaque, synthesize alpha from any non-black pixel.
    bool anyAlpha = false;
    for (int i = 0; i < w * h; ++i) { if (px[i * 4 + 3] != 0) { anyAlpha = true; break; } }
    if (!anyAlpha) {
        for (int i = 0; i < w * h; ++i) {
            uint8_t b = px[i*4+0], gg = px[i*4+1], r = px[i*4+2];
            px[i*4+3] = (b | gg | r) ? 255 : 0;
        }
    }

    POINT hot{ (LONG)ii.xHotspot, (LONG)ii.yHotspot };

    // Optional horizontal mirror of the sprite (and its hotspot) to read correctly
    // through the glass — the macOS "Flip cursor to match mirror" toggle.
    if (flipped_) {
        for (int y = 0; y < h; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(px + (size_t)y * w * 4);
            for (int x = 0; x < w / 2; ++x) std::swap(row[x], row[w - 1 - x]);
        }
        hot.x = (w - 1) - hot.x;
    }

    // Upload to a BGRA texture + SRV.
    auto& dx = DXShared::Get();
    if (!dx.device) { SelectObject(memDC, old); DeleteObject(dib); DeleteDC(memDC); return false; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = bits;
    srd.SysMemPitch = (UINT)w * 4;

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = dx.device->CreateTexture2D(&td, &srd, &tex);

    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);

    if (FAILED(hr)) { Log::Linef(L"CursorSprite: CreateTexture2D failed hr=0x%08X", hr); return false; }

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(hr = dx.device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        Log::Linef(L"CursorSprite: CreateSRV failed hr=0x%08X", hr);
        return false;
    }

    tex_     = tex;
    srv_     = srv;
    size_    = SIZE{ w, h };
    hotSpot_ = hot;
    return true;
}

} // namespace sf
