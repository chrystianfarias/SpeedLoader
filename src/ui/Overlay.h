#pragma once

#include <d3d9.h>

// Gets the frame CEF painted onto the game's screen: a dynamic texture drawn
// as a full-screen quad inside EndScene.
namespace Overlay
{
    void Init();
    void OnFrame(IDirect3DDevice9* device);   // called by D3D9Hook
    void OnDeviceLost(IDirect3DDevice9* device);
    void OnDeviceReset(IDirect3DDevice9* device);

    void SetVisible(bool visible);
    bool IsVisible();

    // Test pattern: a checkerboard in place of CEF's content. It proves the
    // compositing path (hook, texture, blending) before you blame Chromium.
    void SetTestPattern(bool on);

    void Size(int* width, int* height);
}
