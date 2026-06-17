// Proxy-cursor sprite source (port of macOS CursorWindow.swift).
//
// On macOS the proxy is a tiny layered window; on Windows we draw the proxy as a
// textured quad inside the overlay's own swap chain (so it is always above the
// flipped video, always on P, and never itself captured). This class is therefore
// the *sprite source*: it snapshots the live system cursor into a D3D11 texture
// (with hotspot), optionally mirrored horizontally to read correctly through the
// glass. It is touched only by the owning overlay's render thread.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace sf {

class CursorSprite {
public:
    // Mirror the proxy image to match the flipped content (off = normal-facing).
    void SetFlipped(bool on);

    // Re-snapshot the system cursor if it (or the flip state) changed since last
    // call. Returns true if the texture/hotspot was rebuilt. Cheap when unchanged.
    bool Refresh();

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> SRV() const { return srv_; }
    SIZE  Size()    const { return size_; }      // sprite pixel size
    POINT HotSpot() const { return hotSpot_; }   // effective hotspot (flip-adjusted)
    bool  Ready()   const { return srv_ != nullptr; }

private:
    bool Rebuild(HCURSOR cursor);

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    HCURSOR lastCursor_ = nullptr;
    bool    flipped_    = false;
    bool    lastFlipped_ = false;
    SIZE    size_{32, 32};
    POINT   hotSpot_{0, 0};
};

} // namespace sf
