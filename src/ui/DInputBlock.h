#pragma once

// Keeps the game's own keyboard reads away while the console is open.
//
// Capturing window messages is not enough: NFSU2 reads the keyboard through
// DirectInput, straight from the device, and never looks at WM_KEY*. So while
// the player types "/camera", the C still reaches the game and swings the
// camera - the two inputs happen in parallel, through different doors.
//
// The fix is at the door the game actually uses: IDirectInputDevice8::
// GetDeviceState is wrapped, and while the UI has the keyboard the buffer comes
// back zeroed. The game sees "no keys held", which is exactly the truth from
// its point of view.
namespace DInputBlock
{
    void Install();
}
