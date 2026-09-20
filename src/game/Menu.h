#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "core/Hook.h"
#include "core/Log.h"
#include "game/Game.h"
#include "js/JsRuntime.h"
#include "quickjs.h"

// speed.menu - adds real items to the game's own main menu.
//
// Not a drawing trick: this is the engine's own API. UI_Main builds its items
// one by one in its Setup (0x4AEE30, the vtable slot at 0x797524), and the
// disassembly of that function is the whole recipe:
//
//     push 0x48                 ; 72 bytes
//     call 0x575620             ; operator new
//     push 0 / push <stringHash> / push <textureHash>
//     call 0x51F670             ; IconOption_Create
//     mov dword [edi], 0x790C50 ; the option's vtable - slot 1 is its React
//     call dword ptr [eax+0x18] ; IconScrollerMenu::AddOption
//
// NFSU2ExtraOptions builds its language screen the same way, which is where the
// names came from. We wrap Setup, let the game build its menu, then append ours
// with a vtable of our own so the React lands here.
//
// The React is why this cannot live in JavaScript: a vtable slot needs a code
// address, and a JS function has none. The mod side gets an event instead.
namespace Menu
{
    // ---- the game's side of the recipe ----
    //
    // The function that builds the main menu. Patching UI_Main's vtable slot
    // for it did nothing: the log showed the slot swapped and the item
    // registered, and no item ever appeared. The screen calls this directly,
    // not through its own vtable, so the detour has to be in the code.
    #define UI_MAIN_SETUP        0x4AEE30
    #define ICON_OPTION_SIZE     0x48
    #define OPTION_ACTIVATED     0x0C407210 // the hash an option gets when chosen

    typedef void* (__cdecl* tOperatorNew)(size_t);
    typedef void  (__thiscall* tIconOptionCreate)(void* opt, DWORD textureHash,
                                                  DWORD stringHash, DWORD unk);
    typedef void  (__fastcall* tSetup)(DWORD* screen, void* edx);
    typedef void  (__thiscall* tAddOption)(DWORD* screen, void* opt);

    static const tOperatorNew      OperatorNew      = (tOperatorNew)0x575620;
    static const tIconOptionCreate IconOptionCreate = (tIconOptionCreate)0x51F670;

    // Slot 0 of an option's vtable is its destructor; we reuse the game's.
    #define ICON_OPTION_FREE     0x4AEB70

    struct Item
    {
        int    id;
        DWORD  textureHash;
        DWORD  stringHash;
        void*  live;        // the option object of the menu currently on screen
    };

    inline std::vector<Item> g_items;
    inline int      g_nextId = 1;
    inline tSetup   g_origSetup = 0;
    inline bool     g_installed = false;

    // { destructor, React } - the same shape as the game's option vtables.
    inline DWORD g_optionVtable[2] = { ICON_OPTION_FREE, 0 };

    inline void __fastcall OptionReact(void* opt, void*, const char*,
                                       unsigned int data, DWORD*,
                                       unsigned int, unsigned int)
    {
        if (data != OPTION_ACTIVATED) return;

        for (size_t i = 0; i < g_items.size(); i++)
        {
            if (g_items[i].live != opt) continue;

            // Emitting straight from here runs the mod's handler inside the
            // frontend's call stack. That is the same thread the JS runtime
            // lives on and no JS is running at this point (the game called us,
            // not a mod), so it is safe - and it keeps the menu feeling
            // immediate, which a queue drained next frame would not.
            char json[64];
            _snprintf(json, sizeof(json), "{\"id\":%d}", g_items[i].id);
            Log("menu: item %d chosen", g_items[i].id);
            Js::Emit("menu", json);
            return;
        }
    }

    inline void __fastcall SetupHook(DWORD* screen, void* edx)
    {
        g_origSetup(screen, edx);

        Log("menu: Setup ran for screen 0x%p, appending %d item(s)",
            screen, (int)g_items.size());

        // Setup runs every time the screen is built, so the items are appended
        // to a fresh menu each time you come back to it.
        for (size_t i = 0; i < g_items.size(); i++)
        {
            void* opt = OperatorNew(ICON_OPTION_SIZE);
            if (!opt) continue;

            IconOptionCreate(opt, g_items[i].textureHash, g_items[i].stringHash, 0);
            *(DWORD*)opt = (DWORD)g_optionVtable;
            g_items[i].live = opt;

            // AddOption is slot 6 of the screen's own vtable.
            tAddOption addOption = (tAddOption)(*(DWORD**)screen)[6];
            addOption(screen, opt);
        }
    }

    inline void Install()
    {
        if (g_installed) return;
        g_installed = true;

        g_optionVtable[1] = (DWORD)OptionReact;

        // The prologue is `push -1; push <handler>` - seven bytes of SEH setup
        // with no relative operand, so it copies cleanly into a trampoline.
        uint8_t* p = (uint8_t*)UI_MAIN_SETUP;
        size_t len = CopyLength(p, 5);
        if (len == 0)
        {
            Log("menu: prologue at 0x%08X I cannot copy"
                " (%02X %02X %02X %02X %02X) - no menu items",
                UI_MAIN_SETUP, p[0], p[1], p[2], p[3], p[4]);
            return;
        }

        static Trampoline tramp;
        g_origSetup = (tSetup)tramp.Install(UI_MAIN_SETUP, (void*)SetupHook, len);

        Log("menu: main menu Setup detoured at 0x%08X (%u bytes)",
            UI_MAIN_SETUP, (unsigned)len);
    }

    inline int Add(DWORD textureHash, DWORD stringHash)
    {
        Install();

        Item item;
        item.id = g_nextId++;
        item.textureHash = textureHash;
        item.stringHash = stringHash;
        item.live = 0;
        g_items.push_back(item);

        Log("menu: item %d registered (texture 0x%08X, string 0x%08X)",
            item.id, textureHash, stringHash);
        return item.id;
    }

    // ---- the JS side: speed.menu.add({ label, labelHash, icon }) ----
    //
    // The label is the one piece of this that is not clean. An option's text
    // and icon are hashes, resolved against the language file and the texture
    // table - so a word of our own would mean adding a string to
    // Languages\*.bin. Until that is worked out, `label` is hashed as a key
    // (it shows up if the game has that key) and `icon` defaults to one the
    // main menu already uses, so the item is at least visible.
    inline JSValue AddJs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1 || !JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx,
                "speed.menu.add({ label | labelHash, icon })");

        // The texture the first main-menu item uses; a real hash beats a hash
        // that resolves to nothing at all.
        DWORD icon = 0x03704F3D;
        DWORD text = 0;

        JSValue v = JS_GetPropertyStr(ctx, argv[0], "icon");
        if (JS_IsNumber(v))
        {
            double d = 0;
            JS_ToFloat64(ctx, &d, v);
            icon = (DWORD)d;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[0], "labelHash");
        if (JS_IsNumber(v))
        {
            double d = 0;
            JS_ToFloat64(ctx, &d, v);
            text = (DWORD)d;
        }
        JS_FreeValue(ctx, v);

        if (!text)
        {
            v = JS_GetPropertyStr(ctx, argv[0], "label");
            const char* label = JS_IsString(v) ? JS_ToCString(ctx, v) : 0;
            if (label)
            {
                text = bStringHash(label);
                JS_FreeCString(ctx, label);
            }
            JS_FreeValue(ctx, v);
        }

        if (!text)
            return JS_ThrowTypeError(ctx, "speed.menu.add needs label or labelHash");

        return JS_NewInt32(ctx, Add(icon, text));
    }

    inline JSValue HashJs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.menu.hash(key)");
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;

        JSValue out = JS_NewInt64(ctx, (int64_t)bStringHash(s));
        JS_FreeCString(ctx, s);
        return out;
    }

    inline void Register(JSContext* ctx, JSValue speed)
    {
        JSValue menu = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, menu, "add",
                          JS_NewCFunction(ctx, AddJs, "add", 1));
        JS_SetPropertyStr(ctx, menu, "hash",
                          JS_NewCFunction(ctx, HashJs, "hash", 1));
        JS_SetPropertyStr(ctx, speed, "menu", menu);
    }
}
