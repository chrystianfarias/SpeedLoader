#pragma once

#include <d3d9.h>

// Hooks the game's Direct3D 9 so we can draw on top of it.
//
// The path is the most stable one available: SPEED2.EXE imports
// d3d9.dll!Direct3DCreate9 statically, so we swap that import-table entry
// before the game calls it. From there we follow the chain:
//
//   Direct3DCreate9 -> IDirect3D9::CreateDevice -> IDirect3DDevice9
//
// and hook EndScene (drawing) and Reset (recreating resources when the
// resolution changes or the device is lost).
namespace D3D9Hook
{
    typedef void (*FrameFn)(IDirect3DDevice9* device);
    typedef void (*DeviceFn)(IDirect3DDevice9* device);

    // Called from inside EndScene, on the game thread: the place to draw.
    void OnFrame(FrameFn fn);
    // Before and after Reset: D3DPOOL_DEFAULT resources have to go away and
    // come back, or Reset fails.
    void OnDeviceLost(DeviceFn fn);
    void OnDeviceReset(DeviceFn fn);

    bool Install();
    IDirect3DDevice9* Device();

    // Called from the main loop: checks that our EndScene is still in the
    // device vtable and, when it is not, says who took its place.
    void Tick();

    // Current backbuffer size, which is where the UI size comes from.
    bool BackBufferSize(int* width, int* height);
}
