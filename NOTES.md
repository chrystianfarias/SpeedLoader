# SpeedLoader — engineering notes

A modding platform for NFS Underground 2: JavaScript calling native code, and a
Chromium layer composited over the game. Target: `SPEED2.EXE` v1.2 NTSC
(4,800,512 bytes, image base `0x400000`).

The game addresses came from the author's earlier `Pops` project, which in turn
built on NFSU2ExtraOptions. Those notes remain the reverse-engineering
reference; this file only covers decisions specific to this platform.

## Architecture decisions

**Everything in one process.** CEF renders off-screen (`OnPaint` hands us a
BGRA buffer) and the frame is composited as a full-screen quad inside the
game's `EndScene`. The alternative — a transparent window on top — would be far
less code, but it only works in windowed mode and disappears in fullscreen.

**QuickJS, not V8.** Mod code runs on the game thread, in sync with the frame.
That is what lets `speed.on("frame", ...)` react per frame and `speed.call`
enter a native function with no cross-process round trip. V8 on 32-bit would be
a build problem on its own.

**JSON between the two halves.** `main.js` (QuickJS) and the page (V8) share
nothing; the bridge is a `CefProcessMessage` carrying a JSON string. It is the
lowest common denominator between two different engines and avoids type
marshalling.

**Native mods borrow the UI, they do not get a second one.** The host API
(`src/host/HostApi.cpp`, exported as `SpeedLoader_GetApi`) hands an .asi mod a
panel mounted by the same shell, with the same `<id>:<channel>` convention a
JavaScript mod's page uses. The alternative - a separate surface for native
plugins - would have meant a second mounting path in the shell, a second set of
z-order rules and two ways for a page to be written. As it is, the shell gained
three messages (`sl:mount`, `sl:unmount`, `sl:panel-show`) and nothing else.

Native panels exist only in memory: they are not in `mods.json`, because the
mod that owns one is a DLL, not a folder. That is why the shell asks for them
back with `sl:panels` every time it loads, F5 included.

The API is queued on both sides. A plugin is somebody else's DLL and may call
from any thread, so calls land in a queue and `Host::Tick` drains it on the
game thread; messages the other way arrive through `Bridge::Drain`, which means
a plugin's callback runs where its own hooks run and can read game memory
without ceremony.

**The helper is a separate executable.** Chromium requires one for its
subprocesses (renderer, gpu, utility). `SpeedLoaderHelper.exe` is x86 like the
game and knows nothing about NFSU2: it only hosts the renderer side of the
bridge.

## What works

Confirmed running (log at `scripts\SpeedLoader.log`):

- IAT hook on `Direct3DCreate9` -> `CreateDevice` -> device vtable (`EndScene`
  42, `Reset` 16). The device is captured at 0.17 s, before the frontend.
- CEF starts inside the game with `multi_threaded_message_loop`, creates the
  off-screen browser and paints: `first OnPaint: 1280x800`.
- `shell.html` loads, reads `mods.json` and mounts each mod's UI in its own
  shadow root.
- QuickJS loads the mods, runs their `main.js` and delivers gameflow events
  (`3 -> 4 -> 5 -> 6` when entering a race).
- The bridge works both ways: the page sends `ready` and the mod receives it.
- The game's WndProc is hooked (`0x5CCD60`), with a configurable UI key.
- Chaining at `0x581475`: Pops keeps running alongside (its hook shows up in
  the log as the previous target, inside `Pops.asi`).

## The D3D9 vtable war — solved

The first frame drew and then the UI vanished. The log told the story:
`EndScene alive: 1 frames` and never again, while the main-loop hook kept
running. The diagnostic that cracked it was printing **which module** the
vtable slot had come to point at:

```
[gfx] vtable EndScene is now 0x6AC26720 (C:\WINDOWS\SYSTEM32\d3d9.dll),
      not ours 0x68E9AC90
```

So: something **restores** the device vtable to the original a few seconds
after we install — almost certainly `NFSUnderground2.WidescreenFix.asi` or
`NFSU2HDReflections.asi`, which load earlier and also touch D3D9.

Fighting over a vtable slot is a losing game: last writer wins, and that writer
keeps rewriting. The fix was to **detour the function body inside d3d9.dll**,
which nobody restores:

```
[gfx] EndScene detoured in code (0x6AC26720, 7 bytes), immune to vtable swaps
[gfx] Reset detoured in code (0x6ACA09D0, 5 bytes), immune to vtable swaps
[gfx] first quad drawn (1280x800)
[gfx] EndScene alive: 3346 frames
```

Two consequences:

- The Windows 11 `d3d9.dll` does **not** have the classic hot-patch prologue
  (`8B FF 55 8B EC`); it starts with `push 0x14; mov eax, imm32`. Hence
  `CopyLength` in `core/Hook.h`: it sums whole instructions until it covers the
  5 bytes of the JMP and gives up on any opcode outside the prologue
  vocabulary — copying half a relative jump would be worse than not hooking.
- A code detour applies to **every** device in the process, including ones
  other mods create. That is why `EndSceneHook` only draws when the device is
  the one captured in `CreateDevice`.

The vtable hook stays in the code as a fallback, for when the prologue cannot
be copied.

## Speed, RPM and throttle — the car mirror

Pops looked for telemetry in the physics object and only found the gearbox
snapshot, which freezes between shifts. The good path is the car **mirror**:

```
player   = *(DWORD*)0x8900AC        (direct, not an array)
mirror   = *(player + 0x04)
  +0x42C  speed (m/s)
  +0x438  throttle (0 / 0.7 / 1.0 on keyboard)
  +0x440  RPM (800 at idle up to ~7000)
  +0x4E8  distance travelled
```

Great to READ, **useless to write**: `+0x440` is the output of a low-pass
filter (written at `0x5A5A51`, source in global `0x7FBED8`), so writing there
holds the dial and the engine sound while physics ignores it. The real
authority is the physics object, captured by the hook at `0x5A5540`.

The gearbox snapshot (`mirror+0x34 -> +0x18`, gear at `+0x34`) is still the
only source of gear, and `speed.game.telemetry()` merges the two. **The count
is off by one: 1 is neutral**, 2 is first gear, 0 is reverse. The SDK reports
the raw number; whoever displays it applies the shift.

## Turbo, and the vehicle-hook trap

Boost lives in the engine object, reached through the vehicle physics — which
is not stored in any global. The way to get it is to wait for the game to call
`0x5A5540` (`__thiscall`, the vehicle in `ecx`):

```
physics + 0x48   engine object          physics + 0x58   the car mirror
engine  + 0x9C   current pressure       engine  + 0xA4   car ceiling (0 = naturally aspirated)
engine  + 0xAC   vacuum floor
```

**`0x5A5540` runs for every vehicle in the world**, traffic and opponents
included. Storing `ecx` from any call made the object flip between cars several
times per frame, and the boost needle in the HUD turned into noise. The filter
is the mirror: only the vehicle whose `physics+0x58` matches the player's
mirror counts. The read re-checks, because the stored pointer goes stale when
the player changes car.

Pops hooks `0x5A5540` too. Since an `E9` cannot be copied into a trampoline (a
copied rel32 points at the wrong place), SpeedLoader installs in front and
jumps to the previous hook — both run, in installation order.

## Hiding the stock HUD — the frontend package switch

The in-race HUD is a frontend package, just like the menu screens:
`HUD_SingleRace.fng`, `HUD_Drag.fng`, `HUD_Drift.fng`, `HUD_Short_Track.fng`,
`2PHudTop/Bot.fng`. They live in the same 28-byte table format as the screens,
at `0x7F8228` and neighbours (the menu screen table is at `0x7F9138`).

The engine keeps a registry of those packages at `0x8637B0` and decides whether
one counts by testing a word at `+0x1E`:

| Address | What it does |
|---|---|
| `0x51CE80` | `__cdecl (const char* name, short value)` -> writes `[entry+0x1E] = value` |
| `0x51BB70` | `__thiscall` lookup of the entry by name in registry `0x8637B0` |
| `0x51C6D0` | returns `[entry+0x1E] != 0` (the test the engine performs) |

So hiding the HUD is writing 0, and restoring it is writing 1 — the same switch
the game itself uses, with no code patching. A mod does it from JavaScript, on
a key.

Two things learned in practice:

- **It must be reapplied.** The package only accepts the switch once it exists,
  and it is born slightly after the gameflow turns 6. Rewrite it twice per
  second while the race runs: six word writes are cheaper than mapping
  every moment the game rebuilds the package.
- **The rear-view mirror is not part of it.** The mirror strip at the top of
  the screen keeps showing — it is not an FEng package. It is probably
  `HUD_FEATURE_RVM` from the feature table (below), drawn by another path.

### The HUD_FEATURE_* table, and why it went unused

There is a table of 64 names at `0x7F6728` (`HUD_FEATURE_TRACK_MAP`,
`HUD_FEATURE_GAUGES_1`, `HUD_FEATURE_NITROUS`, `HUD_FEATURE_RVM`...) that looks
like a 64-bit mask giving fine-grained HUD control. **Nothing in the code
references that table** — they are debug names left over in the build. The mask
itself probably exists, but finding what tests it is a separate job; the
package switch solved the case for far less.

## Open

**The rear-view mirror.** Still on screen after the HUD is hidden. Likely path:
the `HUD_FEATURE_RVM` mask, or the function that draws the mirror.

**CEF popups** (`PET_POPUP`): `<select>` and friends are not composited yet.

Diagnostics that stay in the binary, useful when something stops showing up:

- `[gfx] EndScene alive: N frames` every 5 s — if it stops, we lost the detour.
- `[gfx] overlay: visible=1 1280x800 texture=1280x800 content=1` — where
  `OnFrame` is bailing out, if it is.
- `[UI] TestPattern = 1` in the ini swaps CEF's content for a checkerboard: it
  separates a compositing problem from a page problem.

## Details that cost time

**`libcef.dll` failing with error 126.** It lives in `scripts\SpeedLoader\`,
away from `SPEED2.EXE`. Windows resolves a DLL's dependencies from the
*executable's* directory, so `chrome_elf.dll` sitting next to it was not found.
Fixed with `LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH)`, which makes the
DLL's own folder the search root. `libcef.dll` is delay-loaded (`/DELAYLOAD` in
CMakeLists) precisely so that manual load happens before the first call.

**`static` in a header.** `Config::g_ownDir` was a `static char[]` inside the
namespace in a `.h`: every `.cpp` got its own copy and only `dllmain.cpp`'s was
filled in. `CefHost.cpp` was building paths from an empty string
(`\SpeedLoader\libcef.dll`). It is `inline` now (C++17). Worth checking `Pops`,
which has the same pattern — it just never shows there because Config is used
from a single file.

**`CRITICAL_SECTION` before CEF.** The game draws long before Chromium starts,
and `Overlay` was already calling `LockFrame` on the first frame — entering an
uninitialised critical section, which killed the game at 0.17 s. The lock is
now born with the DLL (a global object), not in `Init`.

**CEF initialises on the first frame, not in `DllMain`.** Starting threads and
loading DLLs under the loader lock is asking for a deadlock. `DllMain` only
checks the executable version and plants the hooks; CEF, QuickJS and the mods
come up a second later, from the main loop.

**The game runs with `scripts\` as its working directory.** The log lands
there, not in the game root — same behaviour as Pops.

**CEF 152 no longer has `CefEnableHighDPISupport`**, and the *minimal*
distribution ships no `cef_sandbox.lib` (hence `no_sandbox = true` and
`--no-sandbox`).

**`/MT` everywhere.** CEF requires the static runtime; QuickJS defaults to
`/MD`. Without a global `CMAKE_MSVC_RUNTIME_LIBRARY`, the link breaks on
`__imp__acosh` and friends.

## Next steps

1. The rear-view mirror, the last piece of stock HUD still on screen.
2. CEF popups (`PET_POPUP`): `<select>` and friends are not composited yet.
3. A custom scheme (`speedloader://`) instead of `file://` plus
   `--allow-file-access-from-files`.
4. `speed.hook.*`: today you can patch bytes and redirect a `call`, but there
   is no "run my JS when the game reaches this address".
5. Reloading a mod without restarting the game (the `JSContext` is already per
   mod, so it is a matter of dropping and rebuilding it).

## Engine, turbo and the ignition cut

All confirmed at run time, not read off the disassembly. Brought over from the
Pops ASI that predates this platform.

### The mirror is not the car

`player+0x04` is a telemetry MIRROR, filled by 0x5A5540 copying physics into
`[esi+0x58]`. Read it freely; writing to it moves nothing. Its RPM (+0x440) is
the output of a low-pass filter (written at 0x5A5A51, fed from the global
0x7FBED8), so it also lags.

The physics object is the `this` of 0x5A5540. Capture it by filtering on the
mirror (see Vehicle.h) — 0x5A5540 runs for every vehicle in the world, and
keeping the last `this` gives you an opponent's car. That mistake cost a whole
investigation here: an effects array that "never changed" was simply someone
else's.

### Engine fields (physics+0x48)

    +0x14  engine speed, in RAD/S   (rpm = rad/s / 0.1047197551)
    +0x28  idle        (800 or 850, per car)
    +0x2C  rev limit   (7000 or 7500, per car)
    +0x78  multiplier applied to the throttle
    +0x80  input throttle (0 / 0.7 / 1.0 on a keyboard)
    +0x9C  boost pressure, negative under vacuum
    +0xA4  the instantaneous TARGET pressure from the engine map. NOT the car's
           ceiling: it falls to ~0.09 while the throttle is cut. Reading it as a
           maximum is a trap.
    +0xAC  vacuum floor

Naturally aspirated cars keep +0x9C near zero and +0xA4 at zero.

### Order inside a frame

    input writes +0x80  ->  physics consumes  ->  0x5A5540  ->  mods run

Anything written from a frame callback arrives after the physics has already
read it. Three separate attempts failed on this, each looking like a different
bug:

* pinning the RPM made the speed climb without limit — RPM is state, and fixing
  a state variable does not cut power, it breaks the integrator that derives
  everything from it;
* zeroing the throttle from the loop did nothing — the input rewrites it first;
* writing the boost from outside produced a sawtooth — it is the output of an
  integrator that runs at the start of the frame.

The rule that came out of it: inputs can be written, but only from inside the
pass; state must not be written at all.

### The ignition cut the game already has

At 0x5A1629, right after the input writes the throttle:

    fld   [esi+0x5C]        ; timestamp of when the cut began
    fcomp [0x7D7D3C]        ; 0.0 — anything else means "cut"
    mov   [ecx+0x80], ebx   ; throttle = 0
    fild  [0x86518C]        ; the game's clock, an int counter
    fmul  [0x784268]        ; * 0.00025 -> seconds
    fsub  [esi+0x5C]
    fcomp [0x7A18C8]        ; 0.45s, then it clears the stamp itself

So a launch limiter is: stamp `*(int*)0x86518C * 0.00025` to cut, and write 0 to
stop cutting. The 0.45s is tuned for a gear change, where dropping the revs is
the point; for anything else, clear it early or the engine falls away.

A gear change does NOT go through this timer — that cut is another mechanism.
To detect a shift, use the gearbox snapshot (`*(mirror+0x34) + 0x18`, gear at
+0x34), which the game writes only on a shift and leaves frozen in between.

### Turbo

The model is one function, 0x5A0F30, with a single call site at 0x5AA7F0
(__thiscall, one stack argument, `ret 4`). Its integrator is at 0x5A1094.

Anti-lag, done right: wrap the call and let the function run seeing the throttle
at 1.0, then put the real value back. The game's own model then fills the turbo
with its own curve. Holding the pressure by writing it never works, for the
reason above. The same trick held boost across gear changes, which is worth more
over a lap than any launch trick.

### Car effects (CARFX) — mapped, not solved

The effect table is at 0x8027C0, 8 bytes per entry, and 0x430C90 (name -> id)
confirms the numbering: 10 NITRO, 11 EXHAUST_SMOKE, 12 EXHAUST_BLOWOFF,
13 NOS_BLOWOFF.

The player's effects object is `*(mirror+0x554)` (confirmed both ways: the
mirror points at it, and it points back at +0x00). It holds two arrays of 30
circular list heads, at +0x458 and +0x548.

What does NOT work, so nobody repeats it: those lists never change when an
effect plays (~1000 samples at 20 Hz, with nitrous, drifting and a crash), and
that object is not the container of the effect rules either. The rules are
loaded by 0x432CA4 into `container+0x18`, 28 bytes each, count at
`container+0x14` — the container itself has not been found yet, and with it the
evaluator that actually fires an effect.

### Still unmapped

The BRAKE. Not in the engine's input block (+0x84/+0x88 stay zero while
braking), not in the mirror at 0x430..0x45C either. It will need the same
behaviour-driven hunt that found the tachometer: sample while the pedal moves,
then look for the field that follows it.

## Career: the current car

`0x863480` holds the ID of the career car in use. Not a slot index — an id, and
that is better: it survives the garage being reordered.

The disassembly confirms the role. In 29 places the shape is the same:

    mov eax, [0x863480]     ; the id
    push eax
    mov ecx, 0x83AD90       ; the garage container
    call 0x503680           ; look up by id

It is valid in menus as well as in a race, because it is career state, not race
state. That distinction is what sank two earlier attempts: the car mirror
(player+0x04) is built when a race starts and carries nothing about the garage —
a field there that looked like a car index (+0x28) turned out to be the player's
index among the racers, and read the same for two different cars.

### How it was found

By memory scanning, with the tools now in the SDK:

1. `speed.mem.scan(value)` on a number the player knows (career money), then
   `scanNext` after spending some: converged to `0x861E74`, the money itself.
   Useful, but money is a lone global - there is no struct around it to walk.
2. `speed.mem.scanSnapshot()` over the module globals, then alternating
   `scanDiff(true)` after switching cars and `scanDiff(false)` after a few
   seconds of doing nothing.

The alternation is the part that matters. Filtering only by "changed" stalls at
around 800 survivors - clocks, frame counters and RNG change on every pass and
have nothing to do with cars. They die in an interval where nothing happened, so
"unchanged" is what removes them. Two rounds of that left a single address.

### Storage

Per-car data (tank and odometer) lives under that id, in
`scripts\SpeedLoader\data\pops.json`. The data folder sits outside `mods\`
because installing a mod copies `mods\` over the top, and a save in there would
be wiped by an update of the mod that wrote it.

## Career: the car's model

The garage car entry does not hold a model index. It holds a TYPE HASH at
`+0x20`, and the model table is searched by that hash. Treating the number as an
index is what made a 350Z report itself as "MIATA" - two unrelated values.

The game's own lookup, at `0x610130`:

    mov esi, [0x8A1CCC]       ; the model table
    lea ecx, [esi + 0xD0]     ; each entry's type hash
    cmp [ecx], edx            ; the hash being looked for
    add ecx, 0x890            ; entry size
    cmp eax, 0x2E             ; 46 models

So:

    model table   [0x8A1CCC], 46 entries of 0x890 bytes
    entry + 0x00  the model name, as a string ("350Z", "RX8", "SKYLINE")
    entry + 0xD0  the type hash
    car   + 0x20  the same hash, on the garage entry

And the garage entry itself comes from the game's own lookup:

    speed.call(0x503680, [0x83AD90, carId], { conv: "thiscall", ret: "int" })

which is the call the game makes in 29 places. Entries are 0x7F8 apart, in a
static array; `+0x08` is the car id and `+0x0C` looks like a top speed (274,
196, 317, 309 as floats, one per car).

The name is a better key for per-model data than the index: a mod that changes
the car list would shuffle indices, while a table keyed by name still reads.

### What this is used for

Tank capacity and thirst are per MODEL (a 350Z has 76 litres in any garage);
litres in the tank and the odometer are per CAR (`speed.store.car`). Getting
that split wrong would make a refuel follow the model rather than the car.
