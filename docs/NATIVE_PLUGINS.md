# HTML interfaces for native `.asi` mods

Your mod is already written, in C or C++. It hooks the game itself, it knows
what it wants to show, and the only thing missing is somewhere to show it —
without learning JavaScript, rewriting anything, or giving up the hooks you
already have.

SpeedLoader keeps a Chromium running over the game. The host API lends it to
you:

```c
#include "speedloader.h"

static const SL_Api* sl;
static SL_Panel*     panel;

void EveryFrame(void)                 /* your existing main-loop hook */
{
    if (!sl)    { sl = SL_Connect(); return; }
    if (!panel) { panel = sl->panel_open("mymod", "MyMod\\ui.html"); return; }

    sl->panel_send_number(panel, "rpm", CurrentRpm());
}
```

```html
<!-- scripts\MyMod\ui.html -->
<style> .hud { position: absolute; right: 28px; bottom: 28px; color: #fff; } </style>
<div class="hud"><span id="rpm">0</span> rpm</div>
<script>
  speedloader.on("rpm", (v) => root.getElementById("rpm").textContent = Math.round(v));
</script>
```

That is the whole integration. One header, no library to link, no DLL to load:
the header finds SpeedLoader in the process and hands you a table of function
pointers.

- [Getting the header](#getting-the-header)
- [Connecting](#connecting)
- [Panels](#panels)
- [Talking to the page](#talking-to-the-page)
- [Listening to the page](#listening-to-the-page)
- [Input, and clicking things](#input-and-clicking-things)
- [The console](#the-console)
- [Threads](#threads)
- [Shipping it](#shipping-it)
- [The whole API](#the-whole-api)
- [What this is not](#what-this-is-not)

## Getting the header

Copy [`sdk/speedloader.h`](../sdk/speedloader.h) into your project. It is plain
C, depends on nothing but `windows.h`, and works from C or C++ with any
compiler that builds a 32-bit DLL.

Nothing else is needed. There is no import library: SpeedLoader is found at
runtime, so your mod loads and runs normally on a machine that does not have it
— you just do not get a panel.

## Connecting

```c
const SL_Api* sl = SL_Connect();
```

`SL_Connect` looks for `SpeedLoader.asi` in the process and asks it for the
version of the API your header describes. It returns `NULL` when SpeedLoader is
not loaded, or is older than your header.

**Call it from your loop, not from `DllMain`.** ASI loaders load mods in
whatever order the filesystem hands them over, so SpeedLoader may arrive after
you. Retrying costs a pointer comparison per frame:

```c
if (!sl) { sl = SL_Connect(); return; }
```

Opening a panel before Chromium is up is fine, by the way — the panel is
remembered and mounted the moment the UI exists.

## Panels

```c
SL_Panel* panel = sl->panel_open("mymod", "MyMod\\ui.html");
```

A panel is one page, mounted over the game, in its own shadow root — your CSS
cannot leak into another mod's panel, and no one else's can leak into yours.

- **`id`** names it: letters, digits, `-` and `_`, up to 32 characters. It is
  the prefix on every message and the name shown in the console, so use your
  mod's name. `sl` is taken by the loader, and an id already in use is refused
  (`panel_open` returns `NULL` and says so in the log).
- **`html`** is a path. A relative one resolves against the game's `scripts\`
  folder — where your `.asi` lives — so `"MyMod\\ui.html"` means
  `scripts\MyMod\ui.html`. An absolute path is taken as it is.

The page is a fragment, not a document: no `<html>`, no `<body>`, just the
styles and markup you want on screen. Three things are in scope for its
`<script>` tags:

| | |
|---|---|
| `speedloader` | `.send(channel, data)`, `.on(channel, cb)`, `.off(channel, cb)` |
| `root` | the shadow root — use `root.getElementById`, not `document` |
| `mod` | `{ id, name, url }` |

It is a real Chromium: `fetch`, `<canvas>`, SVG, animations, web fonts,
DevTools. Relative paths inside the page resolve against its own folder, so
images and extra scripts sit next to `ui.html`.

`panel_show(panel, 0)` hides it and `panel_show(panel, 1)` brings it back,
which is what you bind your toggle key to. `panel_reload(panel)` fetches the
file again: edit the HTML, press your key, see it — no restart.
`panel_close(panel)` removes it for good and the handle dies with it.

## Talking to the page

```c
sl->panel_send_number(panel, "rpm", 4200.0);        /* a JS number */
sl->panel_send_text(panel, "gear", "3rd");          /* a JS string, escaped for you */
sl->panel_send(panel, "state", "{\"lap\":2,\"best\":91.4}");  /* raw JSON you built */
```

All three arrive as `speedloader.on(channel, data)` in the page. Use
`panel_send` when you already have JSON; the other two exist so that a C mod
does not have to carry a JSON writer to send one number.

Messages are queued and delivered on the next frame, in order. Sending every
frame is fine — it is one small string across a pipe — but sending 60 updates a
second to something the player reads at a glance is work nobody sees. Throttle
to 20 or 30 Hz and the panel looks identical.

## Listening to the page

```c
static void __cdecl OnMessage(const char* channel, const char* json, void* user)
{
    if (!strcmp(channel, "ready"))
        sl->panel_send_text(panel, "title", "MyMod 1.0");
}

sl->panel_on(panel, OnMessage, NULL);
```

`json` is the page's data, JSON-encoded, and `"null"` when it sent nothing. A
button in the page becomes `speedloader.send("reset")` there and a
`strcmp(channel, "reset")` here.

One ordering detail worth knowing before it costs you an afternoon: your mod is
running long before the page mounts, and a panel can be mounted again at any
time — the player presses F5, or another mod reloads the UI. So do not push
state once and assume it landed. Have the page announce itself with
`speedloader.send("ready")` and answer that, which is what the example does.

## Input, and clicking things

Every panel covers the screen with `pointer-events: none`, so it never eats
clicks meant for the game. For something clickable, put `pointer-events: auto`
on that element and hand the mouse over:

```c
sl->capture_input(1);    /* keyboard and mouse go to the UI */
sl->capture_input(0);    /* and back to the game */
```

This is the same switch **F1** flips. While it is on, the game stops seeing the
keyboard (SpeedLoader blocks DirectInput too), so typing in a field does not
also drive the car.

## The console

```c
sl->print("mymod", "tank filled: {orange}12.4 L{/}");
```

One line in the in-game console, the one `/` opens. The first argument is the
tag shown in front of the line. Colours are `{red}`, `{green}`, `{yellow}`,
`{orange}`, `{blue}`, `{gray}`, `{white}` and `{#rrggbb}`, closed with `{/}`.

## Threads

Every call in the API is safe from any thread: they are queued under a lock and
applied on the game thread, inside the frame.

Your callback is the other direction, and it is always invoked **on the game
thread** — the same place your hooks run. So reading the game's memory from it
is as safe as reading it from your own hook, and you need no synchronisation of
your own for state that only those two touch.

What you must not do is block in the callback. It runs inside the game's frame:
a `Sleep`, a file read or a network call there is a stutter the player sees.

## Shipping it

```
scripts\
  MyMod.asi              your mod
  MyMod\ui.html          its page (and whatever else it loads)
  SpeedLoader.asi        not yours to ship - the player installs it
```

Say in your readme that SpeedLoader is required for the interface. Your mod
should keep working without it, since `SL_Connect` simply keeps returning
`NULL` — a missing panel is not a reason to stop hooking the game.

## The whole API

| | |
|---|---|
| `SL_Connect()` | finds SpeedLoader, returns the table or `NULL` |
| `loader_version()` | SpeedLoader's version, as a string |
| `panel_open(id, html)` | opens a panel, returns a handle |
| `panel_close(panel)` | removes it; the handle dies |
| `panel_on(panel, fn, user)` | your callback for the page's messages |
| `panel_send(panel, channel, json)` | sends raw JSON |
| `panel_send_text(panel, channel, text)` | sends a string, escaped for you |
| `panel_send_number(panel, channel, value)` | sends a double |
| `panel_reload(panel)` | re-fetches the page from disk |
| `panel_show(panel, show)` | hides or shows this panel |
| `panel_ready(panel)` | is the page mounted? |
| `print(tag, text)` | a line in the in-game console |
| `capture_input(on)` / `capturing_input()` | keyboard and mouse to the UI |

The table is versioned: `SL_Connect` passes the `SL_API_VERSION` your header
was built with, and a newer SpeedLoader answers with a table that still means
what your header says it means. Fields are only ever added at the end.

## What this is not

This is a UI, not the JavaScript SDK. `speed.game.telemetry()`, memory reads,
native calls, per-car storage, sound and world drawing belong to mods written
in JavaScript, described in [MODDING.md](MODDING.md) — a native mod already has
all of that in C, which is the point.

If what you want is the SDK rather than the interface, write a JavaScript mod.
If you have a working `.asi` and want it to look like it belongs in 2026, this
is the door.

## The example

[`examples/asi-plugin/`](../examples/asi-plugin) is a complete mod in one file:
it connects, opens a panel, sends a counter, answers the page's `ready`, and
toggles with F9. It builds with the project (target `HelloPanel`), and it is
compiled on every build — which is also how the header stays honest.

```
build.bat nomod
copy build\Release\HelloPanel.asi   <game>\scripts\
mkdir <game>\scripts\HelloPanel
copy examples\asi-plugin\ui.html    <game>\scripts\HelloPanel\
```
