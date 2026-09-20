#pragma once

#include <windows.h>

// Decides who gets keyboard and mouse: the game or the UI.
//
// NFSU2 does not read the keyboard through any window message (see the Pops
// notes), which works in our favour here: we can intercept WM_KEY* without
// taking anything away from the game. It does use the mouse through
// DirectInput, so while the UI has focus, mouse events are swallowed.
namespace InputRouter
{
    void Install(int toggleVirtualKey);
    void Update();          // per frame: hooks the window as soon as it exists
    void Shutdown();

    bool IsCapturing();
    void SetCapturing(bool capturing);

    // Keys pressed while the UI is NOT capturing, for mods to listen to
    // (`speed.on("keydown", ...)`). Returns 0 when the queue drains.
    //
    // The queue exists because a WndProc is no place to run JavaScript: a slow
    // mod there would freeze the window. The main loop drains it once a frame.
    int PopKey();
}
