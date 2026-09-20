#include "Overlay.h"

#include "CefHost.h"
#include "D3D9Hook.h"
#include "core/Log.h"

namespace
{
    struct Vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    const DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    IDirect3DTexture9*    g_texture = 0;
    IDirect3DStateBlock9* g_stateBlock = 0;
    int  g_texW = 0, g_texH = 0;
    int  g_width = 0, g_height = 0;
    bool g_visible = true;
    bool g_testPattern = false;
    bool g_hasContent = false;   // has any frame arrived to show yet?

    void ReleaseResources()
    {
        if (g_texture)    { g_texture->Release();    g_texture = 0; }
        if (g_stateBlock) { g_stateBlock->Release(); g_stateBlock = 0; }
        g_texW = g_texH = 0;
        g_hasContent = false;   // the new texture starts empty
    }

    bool EnsureTexture(IDirect3DDevice9* device, int width, int height)
    {
        if (g_texture && g_texW == width && g_texH == height) return true;

        if (g_texture) { g_texture->Release(); g_texture = 0; }
        HRESULT hr = device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC,
                                           D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                           &g_texture, NULL);
        if (FAILED(hr))
        {
            LogGfx("CreateTexture %dx%d FAILED (0x%08X)", width, height, hr);
            g_texture = 0;
            return false;
        }
        g_texW = width;
        g_texH = height;
        g_hasContent = false;
        LogGfx("UI texture: %dx%d", width, height);
        return true;
    }

    void UploadBgra(const unsigned char* src, int width, int height)
    {
        D3DLOCKED_RECT lr;
        if (FAILED(g_texture->LockRect(0, &lr, NULL, D3DLOCK_DISCARD))) return;

        unsigned char* dst = (unsigned char*)lr.pBits;
        const size_t rowBytes = (size_t)width * 4;
        for (int y = 0; y < height; y++)
            memcpy(dst + (size_t)y * lr.Pitch, src + (size_t)y * rowBytes, rowBytes);

        g_texture->UnlockRect(0);
    }

    void UploadTestPattern(int width, int height)
    {
        D3DLOCKED_RECT lr;
        if (FAILED(g_texture->LockRect(0, &lr, NULL, D3DLOCK_DISCARD))) return;

        for (int y = 0; y < height; y++)
        {
            DWORD* row = (DWORD*)((unsigned char*)lr.pBits + (size_t)y * lr.Pitch);
            for (int x = 0; x < width; x++)
            {
                bool on = ((x / 64) + (y / 64)) & 1;
                // Premultiplied BGRA: half transparent over the game.
                row[x] = on ? 0x80000040 : 0x40200000;
            }
        }
        g_texture->UnlockRect(0);
    }

    void DrawQuad(IDirect3DDevice9* device, int width, int height)
    {
        const float w = (float)width;
        const float h = (float)height;

        // -0.5 aligns texel to pixel in D3D9.
        Vertex v[4] = {
            { -0.5f,     -0.5f,     0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
            { w - 0.5f,  -0.5f,     0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
            { -0.5f,     h - 0.5f,  0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
            { w - 0.5f,  h - 0.5f,  0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
        };

        device->SetVertexShader(NULL);
        device->SetPixelShader(NULL);
        device->SetFVF(VERTEX_FVF);
        device->SetTexture(0, g_texture);

        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        // CEF hands us premultiplied BGRA, hence SRCBLEND = ONE.
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    }
}

namespace Overlay
{
    void Init()
    {
        D3D9Hook::OnFrame(OnFrame);
        D3D9Hook::OnDeviceLost(OnDeviceLost);
        D3D9Hook::OnDeviceReset(OnDeviceReset);
    }

    void SetVisible(bool visible) { g_visible = visible; }
    bool IsVisible()              { return g_visible; }
    void SetTestPattern(bool on)  { g_testPattern = on; }

    void Size(int* width, int* height)
    {
        if (width)  *width  = g_width;
        if (height) *height = g_height;
    }

    void OnDeviceLost(IDirect3DDevice9*)
    {
        // Everything is in D3DPOOL_DEFAULT: release before Reset, rebuild after.
        ReleaseResources();
    }

    void OnDeviceReset(IDirect3DDevice9*)
    {
        int w = 0, h = 0;
        if (D3D9Hook::BackBufferSize(&w, &h))
        {
            g_width = w;
            g_height = h;
            CefHost::Resize(w, h);
        }
    }

    void OnFrame(IDirect3DDevice9* device)
    {
        if (!device) return;

        int w = 0, h = 0;
        if (D3D9Hook::BackBufferSize(&w, &h) && (w != g_width || h != g_height))
        {
            g_width = w;
            g_height = h;
            CefHost::Resize(w, h);
        }

        static DWORD lastDiag = 0;
        DWORD now = GetTickCount();
        bool diag = (now - lastDiag) > 5000;
        if (diag)
        {
            lastDiag = now;
            LogGfx("overlay: visible=%d %dx%d texture=%dx%d content=%d",
                   (int)g_visible, g_width, g_height, g_texW, g_texH,
                   (int)g_hasContent);
        }

        if (!g_visible || g_width <= 0 || g_height <= 0) return;

        // The texture follows the backbuffer; CEF paints at that same size.
        if (!EnsureTexture(device, g_width, g_height)) return;

        if (g_testPattern)
        {
            if (!g_hasContent)
            {
                UploadTestPattern(g_width, g_height);
                g_hasContent = true;
            }
        }
        else
        {
            const void* pixels = 0;
            int pw = 0, ph = 0;
            if (CefHost::LockFrame(&pixels, &pw, &ph))
            {
                if (pw == g_texW && ph == g_texH)
                {
                    UploadBgra((const unsigned char*)pixels, pw, ph);
                    g_hasContent = true;
                }
                else if (diag)
                {
                    LogGfx("CEF frame %dx%d does not match the texture %dx%d",
                           pw, ph, g_texW, g_texH);
                }
                CefHost::UnlockFrame();
            }
            else if (diag && !g_hasContent)
            {
                LogGfx("no new frame from CEF");
            }
        }

        // Before the first frame the texture is garbage: not drawing beats
        // flashing a screen of noise over the game.
        if (!g_hasContent) return;

        static bool firstDraw = true;
        if (firstDraw)
        {
            firstDraw = false;
            LogGfx("first quad drawn (%dx%d)", g_width, g_height);
        }

        if (!g_stateBlock)
            device->CreateStateBlock(D3DSBT_ALL, &g_stateBlock);

        if (g_stateBlock) g_stateBlock->Capture();
        DrawQuad(device, g_width, g_height);
        if (g_stateBlock) g_stateBlock->Apply();
    }
}
