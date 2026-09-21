/*
 * SpeedLoader host API - an HTML/CSS interface for a native .asi mod.
 *
 * This is for mods written in C or C++, that already hook the game themselves
 * and only want a real interface over it: a panel of HTML, rendered by the
 * Chromium layer SpeedLoader already keeps running, talking to your code by
 * message. No JavaScript mod, no mod.json, no QuickJS involved.
 *
 *     #include "speedloader.h"
 *
 *     static SL_Panel*   panel;
 *     static const SL_Api* sl;
 *
 *     static void __cdecl OnUi(const char* channel, const char* json, void* user)
 *     {
 *         if (!strcmp(channel, "ready"))
 *             sl->panel_send_text(panel, "hello", "MyMod 1.0");
 *     }
 *
 *     // From your main-loop hook, NOT from DllMain: SpeedLoader may not be
 *     // loaded yet when your DLL is, and its UI comes up a second later.
 *     void EveryFrame(void)
 *     {
 *         if (!sl) { sl = SL_Connect(); return; }
 *         if (!panel) {
 *             panel = sl->panel_open("mymod", "MyMod\\ui.html");
 *             sl->panel_on(panel, OnUi, 0);
 *         }
 *         sl->panel_send_number(panel, "rpm", CurrentRpm());
 *     }
 *
 * Inside the page, `speedloader`, `root` and `mod` are in scope exactly as they
 * are for a JavaScript mod:
 *
 *     <div class="hud">rpm <span id="v">0</span></div>
 *     <script>
 *       speedloader.on("rpm", (v) => root.getElementById("v").textContent = v);
 *       speedloader.send("ready");
 *     </script>
 *
 * Threading: every call here is safe from any thread. Your callback, though, is
 * always invoked on the GAME thread, inside the frame - the same place your
 * hooks run, so reading the game's memory from it is safe.
 *
 * SpeedLoader is CC BY-NC 4.0, but this header is the interface to it: use it
 * in your own mod freely.
 */
#ifndef SPEEDLOADER_H_
#define SPEEDLOADER_H_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL_API_VERSION 1

typedef struct SL_Panel SL_Panel;

/* A message from the page: `channel` is what the page passed to
 * speedloader.send, `json` is its data, JSON-encoded ("null" when it sent
 * nothing). Runs on the game thread. */
typedef void(__cdecl* SL_MessageFn)(const char* channel, const char* json, void* user);

typedef struct SL_Api
{
    unsigned version;                        /* SL_API_VERSION of this table */
    const char*(__cdecl* loader_version)(void);

    /* Opens a panel: `id` names it (letters, digits, '-' and '_'), `html` is
     * the page. A relative path resolves against the game's scripts\ folder,
     * where your .asi lives, so "MyMod\\ui.html" is the usual form.
     *
     * Returns NULL only if the id is unusable or already taken. The panel is
     * mounted as soon as the UI is up, so calling this early is fine. */
    SL_Panel*(__cdecl* panel_open)(const char* id, const char* html);

    /* Takes the panel off the screen and forgets it. The pointer dies here. */
    void(__cdecl* panel_close)(SL_Panel* panel);

    /* Your callback for speedloader.send from the page. One per panel; passing
     * NULL removes it. */
    void(__cdecl* panel_on)(SL_Panel* panel, SL_MessageFn fn, void* user);

    /* Sends to the page, where speedloader.on(channel, cb) receives it.
     *
     *   _send       `json` is raw JSON you built: "42", "{\"a\":1}", "null".
     *   _send_text  a string, escaped and quoted for you.
     *   _send_number a double, which arrives as a JavaScript number.
     */
    void(__cdecl* panel_send)(SL_Panel* panel, const char* channel, const char* json);
    void(__cdecl* panel_send_text)(SL_Panel* panel, const char* channel, const char* text);
    void(__cdecl* panel_send_number)(SL_Panel* panel, const char* channel, double value);

    /* Re-fetches the page from disk - the loop for iterating on HTML without
     * restarting the game. */
    void(__cdecl* panel_reload)(SL_Panel* panel);

    /* Shows or hides this panel alone. Panels start visible. */
    void(__cdecl* panel_show)(SL_Panel* panel, int show);

    /* Is the panel on screen, with its page mounted? */
    int(__cdecl* panel_ready)(SL_Panel* panel);

    /* A line in the in-game console (the one "/" opens). `tag` is the name
     * shown in front of it; NULL uses the panel ids you opened, or "sl". */
    void(__cdecl* print)(const char* tag, const char* text);

    /* Hands keyboard and mouse to the UI, and takes them back - the same thing
     * F1 does. Anything the player must click needs this on. */
    void(__cdecl* capture_input)(int on);
    int(__cdecl* capturing_input)(void);
} SL_Api;

typedef const SL_Api*(__cdecl* SL_GetApiFn)(unsigned version);

/* Finds SpeedLoader in the process. Returns NULL when it is not loaded (yet),
 * or when it is older than the version this header describes - so call it
 * every frame until it answers, rather than once at startup. */
static __inline const SL_Api* SL_Connect(void)
{
    HMODULE module = GetModuleHandleA("SpeedLoader.asi");
    SL_GetApiFn get;

    if (!module) return 0;

    get = (SL_GetApiFn)GetProcAddress(module, "SpeedLoader_GetApi");
    return get ? get(SL_API_VERSION) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SPEEDLOADER_H_ */
