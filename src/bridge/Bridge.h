#pragma once

// The queue between the CEF thread and the game thread.
//
// QuickJS is not reentrant and may only be touched by the game thread; CEF
// delivers messages on its own. So nothing calls into JS directly: whatever
// comes from the UI goes into this queue and comes out on the next frame.
namespace Bridge
{
    void Install();
    void Drain();     // called once per frame, on the game thread
    void Shutdown();
}
