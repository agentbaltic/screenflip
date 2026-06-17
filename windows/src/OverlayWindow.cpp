// Borderless, topmost, click-through overlay on P that renders the captured
// workspace horizontally flipped, plus the proxy cursor.
// Link: d3d11.lib dxgi.lib dcomp.lib user32.lib
//
// Presentation uses DirectComposition hosting a flip-model swap chain
// (CreateSwapChainForComposition) — the robust way to put a D3D surface on a
// borderless topmost window. The window is WS_EX_TRANSPARENT|WS_EX_NOACTIVATE so
// input falls through, WS_EX_NOREDIRECTIONBITMAP for DComp, and hidden from all
// capture via SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE).
#include "OverlayWindow.h"
#include "Capture.h"
#include "CursorWindow.h"
#include "DXShared.h"
#include "Log.h"

#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>

// Shader byte arrays produced by fxc at build time (see build.bat). They live in
// the build output dir, which build.bat puts on the include path (/I).
#include "flip_vs.h"
#include "flip_ps.h"
#include "cursor_vs.h"
#include "cursor_ps.h"

using Microsoft::WRL::ComPtr;

namespace sf {

static const wchar_t* kOverlayClass = L"ScreenFlipOverlayWindow";

struct OverlayWindow::Gfx {
    ComPtr<IDCompositionDevice>  dcompDevice;
    ComPtr<IDCompositionTarget>  dcompTarget;
    ComPtr<IDCompositionVisual>  dcompVisual;
    ComPtr<IDXGISwapChain1>      swap;
    ComPtr<ID3D11RenderTargetView> rtv;

    ComPtr<ID3D11VertexShader>  flipVS, cursorVS;
    ComPtr<ID3D11PixelShader>   flipPS, cursorPS;
    ComPtr<ID3D11SamplerState>  sampler;
    ComPtr<ID3D11BlendState>    blendOpaque, blendAlpha;
    ComPtr<ID3D11Buffer>        cursorCB;

    ComPtr<ID3D11Texture2D>          frameTex;
    ComPtr<ID3D11ShaderResourceView> frameSRV;
    SIZE frameSize{ 0, 0 };

    CursorSprite cursor;
    unsigned width = 0, height = 0;
};

OverlayWindow::~OverlayWindow() { Destroy(); }

LRESULT CALLBACK OverlayWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;   // belt-and-suspenders click-through
    return DefWindowProcW(h, m, w, l);
}

bool OverlayWindow::Create(HINSTANCE hInst, const RECT& monitorRect) {
    rect_ = monitorRect;
    originX_.store(monitorRect.left);
    originY_.store(monitorRect.top);
    targetW_.store((unsigned)RectWidth(monitorRect));
    targetH_.store((unsigned)RectHeight(monitorRect));

    static std::atomic<bool> classRegistered{ false };
    bool expected = false;
    if (classRegistered.compare_exchange_strong(expected, true)) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = &OverlayWindow::WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kOverlayClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    }

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                          WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    hwnd_ = CreateWindowExW(exStyle, kOverlayClass, L"ScreenFlip", WS_POPUP,
                            monitorRect.left, monitorRect.top,
                            RectWidth(monitorRect), RectHeight(monitorRect),
                            nullptr, nullptr, hInst, nullptr);
    if (!hwnd_) {
        Log::Linef(L"OverlayWindow: CreateWindowExW failed (%lu)", GetLastError());
        return false;
    }

    // Hide the overlay from every capture so it can never feed back (mac overlay
    // exclusion analogue). Available on Windows 10 2004+.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    if (!InitGraphics()) {
        Log::Line(L"OverlayWindow: InitGraphics failed; overlay will be black");
        // keep the window (covers P black) and keep running so reconcile still works
    }

    running_.store(true);
    render_ = std::thread([this] { RenderThread(); });
    Log::Linef(L"OverlayWindow: created on P at (%ld,%ld) %ldx%ld",
               monitorRect.left, monitorRect.top, RectWidth(monitorRect), RectHeight(monitorRect));
    return true;
}

bool OverlayWindow::InitGraphics() {
    auto& dx = DXShared::Get();
    if (!dx.Init()) return false;
    gfx_ = new Gfx();
    gfx_->width  = targetW_.load();
    gfx_->height = targetH_.load();
    if (gfx_->width == 0 || gfx_->height == 0) { gfx_->width = 1; gfx_->height = 1; }

    HRESULT hr = DCompositionCreateDevice(dx.dxgiDevice.Get(),
                                          IID_PPV_ARGS(gfx_->dcompDevice.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) { Log::Linef(L"Overlay: DCompositionCreateDevice hr=0x%08X", hr); DiscardGraphics(); return false; }
    if (FAILED(hr = gfx_->dcompDevice->CreateTargetForHwnd(hwnd_, TRUE, &gfx_->dcompTarget))) {
        Log::Linef(L"Overlay: CreateTargetForHwnd hr=0x%08X", hr); DiscardGraphics(); return false;
    }
    if (FAILED(hr = gfx_->dcompDevice->CreateVisual(&gfx_->dcompVisual))) {
        Log::Linef(L"Overlay: CreateVisual hr=0x%08X", hr); DiscardGraphics(); return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = gfx_->width; scd.Height = gfx_->height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;  // overlay is fully opaque over P
    if (FAILED(hr = dx.factory->CreateSwapChainForComposition(dx.device.Get(), &scd, nullptr, &gfx_->swap))) {
        Log::Linef(L"Overlay: CreateSwapChainForComposition hr=0x%08X", hr); DiscardGraphics(); return false;
    }
    gfx_->dcompVisual->SetContent(gfx_->swap.Get());
    gfx_->dcompTarget->SetRoot(gfx_->dcompVisual.Get());
    gfx_->dcompDevice->Commit();

    // RTV on the back buffer (reused across frames in the D3D11 flip model).
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(hr = gfx_->swap->GetBuffer(0, IID_PPV_ARGS(&bb))) ||
        FAILED(hr = dx.device->CreateRenderTargetView(bb.Get(), nullptr, &gfx_->rtv))) {
        Log::Linef(L"Overlay: RTV hr=0x%08X", hr); DiscardGraphics(); return false;
    }

    // Shaders (no input layout — full-screen triangle / quad via SV_VertexID).
    dx.device->CreateVertexShader(g_flip_vs,   sizeof(g_flip_vs),   nullptr, &gfx_->flipVS);
    dx.device->CreatePixelShader (g_flip_ps,   sizeof(g_flip_ps),   nullptr, &gfx_->flipPS);
    dx.device->CreateVertexShader(g_cursor_vs, sizeof(g_cursor_vs), nullptr, &gfx_->cursorVS);
    dx.device->CreatePixelShader (g_cursor_ps, sizeof(g_cursor_ps), nullptr, &gfx_->cursorPS);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dx.device->CreateSamplerState(&sd, &gfx_->sampler);

    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dx.device->CreateBlendState(&bo, &gfx_->blendOpaque);

    D3D11_BLEND_DESC ba{};
    ba.RenderTarget[0].BlendEnable = TRUE;
    // DrawIconEx(DI_NORMAL) yields PREMULTIPLIED alpha, so use ONE / INV_SRC_ALPHA
    // (premultiplied-over) — SRC_ALPHA would double-darken the cursor's AA edges.
    ba.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dx.device->CreateBlendState(&ba, &gfx_->blendAlpha);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;                 // one float4
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dx.device->CreateBuffer(&cbd, nullptr, &gfx_->cursorCB);

    return true;
}

void OverlayWindow::DiscardGraphics() {
    if (gfx_) { delete gfx_; gfx_ = nullptr; }
}

bool OverlayWindow::EnsureFrameTexture(SIZE size) {
    if (!gfx_) return false;
    if (size.cx <= 0 || size.cy <= 0) return false;
    if (gfx_->frameTex && gfx_->frameSize.cx == size.cx && gfx_->frameSize.cy == size.cy) return true;

    auto& dx = DXShared::Get();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)size.cx; td.Height = (UINT)size.cy;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dx.device->CreateTexture2D(&td, nullptr, &t))) return false;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dx.device->CreateShaderResourceView(t.Get(), nullptr, &srv))) return false;
    gfx_->frameTex = t;
    gfx_->frameSRV = srv;
    gfx_->frameSize = size;
    return true;
}

void OverlayWindow::RenderThread() {
    while (running_.load()) {
        if (!gfx_ || !gfx_->swap) { Sleep(16); continue; }

        bool present = false;
        {
            // Hold the shared GPU lock for the whole record sequence (resize + copy +
            // draw) so concurrent overlay threads / the capture copy can't interleave
            // pipeline state on the single shared immediate context.
            std::lock_guard<std::recursive_mutex> rl(DXShared::Get().renderLock);

            // Match the swap chain to the current monitor size (after Reposition).
            unsigned w = targetW_.load(), h = targetH_.load();
            if (w && h && (w != gfx_->width || h != gfx_->height)) {
                gfx_->rtv.Reset();
                if (SUCCEEDED(gfx_->swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
                    ComPtr<ID3D11Texture2D> bb;
                    if (SUCCEEDED(gfx_->swap->GetBuffer(0, IID_PPV_ARGS(&bb))))
                        DXShared::Get().device->CreateRenderTargetView(bb.Get(), nullptr, &gfx_->rtv);
                    gfx_->width = w; gfx_->height = h;
                }
            }

            if (gfx_->rtv) {
                Capture* cap = capture_.load();
                if (cap && cap->HasFrame()) {
                    SIZE fs = cap->FrameSize();
                    if (EnsureFrameTexture(fs) && gfx_->frameTex) cap->CopyLatestInto(gfx_->frameTex.Get());
                }
                DrawScene();
                present = true;
            }
        }
        // Present OUTSIDE the shared lock — Present(1,0) can block up to a vsync and
        // must not stall other overlays' draws or the capture copy. Don't present a
        // back buffer we didn't render to (e.g. just after a failed RTV recreate).
        if (present) gfx_->swap->Present(1, 0);
        else Sleep(16);
    }
}

void OverlayWindow::DrawScene() {
    auto ctx = DXShared::Get().context.Get();

    // Snapshot proxy state.
    bool visible; POINT pos; bool flip;
    { std::lock_guard<std::mutex> lk(proxyMx_); visible = proxyVisible_; pos = proxyPos_; flip = proxyFlipped_; }

    const float black[4] = { 0, 0, 0, 1 };
    const float zero[4]  = { 0, 0, 0, 0 };
    ID3D11RenderTargetView* rtv = gfx_->rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)gfx_->width, (float)gfx_->height, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->ClearRenderTargetView(rtv, black);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);

    // Flip pass: the captured workspace mirrored across the whole monitor.
    if (gfx_->frameSRV) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(gfx_->flipVS.Get(), nullptr, 0);
        ctx->PSSetShader(gfx_->flipPS.Get(), nullptr, 0);
        ID3D11SamplerState* smp = gfx_->sampler.Get();
        ID3D11ShaderResourceView* srv = gfx_->frameSRV.Get();
        ctx->PSSetSamplers(0, 1, &smp);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->OMSetBlendState(gfx_->blendOpaque.Get(), zero, 0xffffffff);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);  // unbind (frameTex is also a copy dest)
    }

    // Proxy-cursor pass.
    if (visible) {
        gfx_->cursor.SetFlipped(flip);
        gfx_->cursor.Refresh();
        if (gfx_->cursor.Ready()) {
            ComPtr<ID3D11ShaderResourceView> csrv = gfx_->cursor.SRV();
            SIZE  ssz = gfx_->cursor.Size();
            POINT hot = gfx_->cursor.HotSpot();
            const double W = gfx_->width ? gfx_->width : 1;
            const double H = gfx_->height ? gfx_->height : 1;
            double lx = (double)(pos.x - originX_.load());
            double ly = (double)(pos.y - originY_.load());
            double left = lx - hot.x, top = ly - hot.y;
            double right = left + ssz.cx, bottom = top + ssz.cy;
            float rectNDC[4] = {
                (float)((left   / W) * 2.0 - 1.0),
                (float)(1.0 - (top    / H) * 2.0),
                (float)((right  / W) * 2.0 - 1.0),
                (float)(1.0 - (bottom / H) * 2.0),
            };
            ctx->UpdateSubresource(gfx_->cursorCB.Get(), 0, nullptr, rectNDC, 0, 0);

            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->VSSetShader(gfx_->cursorVS.Get(), nullptr, 0);
            ID3D11Buffer* cb = gfx_->cursorCB.Get();
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetShader(gfx_->cursorPS.Get(), nullptr, 0);
            ID3D11SamplerState* smp = gfx_->sampler.Get();
            ID3D11ShaderResourceView* srv = csrv.Get();
            ctx->PSSetSamplers(0, 1, &smp);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->OMSetBlendState(gfx_->blendAlpha.Get(), zero, 0xffffffff);
            ctx->Draw(4, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(0, 1, &none);
        }
    }
}

void OverlayWindow::SetProxy(bool visible, POINT hotspotPos, bool flipped) {
    std::lock_guard<std::mutex> lk(proxyMx_);
    proxyVisible_ = visible;
    proxyPos_ = hotspotPos;
    proxyFlipped_ = flipped;
}

void OverlayWindow::HideProxy() {
    std::lock_guard<std::mutex> lk(proxyMx_);
    proxyVisible_ = false;
}

void OverlayWindow::Reposition(const RECT& monitorRect) {
    rect_ = monitorRect;
    originX_.store(monitorRect.left);
    originY_.store(monitorRect.top);
    targetW_.store((unsigned)RectWidth(monitorRect));
    targetH_.store((unsigned)RectHeight(monitorRect));
    if (hwnd_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, monitorRect.left, monitorRect.top,
                     RectWidth(monitorRect), RectHeight(monitorRect),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void OverlayWindow::Destroy() {
    if (running_.exchange(false)) {
        if (render_.joinable()) render_.join();
    }
    DiscardGraphics();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

} // namespace sf
