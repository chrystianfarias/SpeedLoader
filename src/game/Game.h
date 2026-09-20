// Addresses for SPEED2.EXE v1.2 NTSC (NFS Underground 2, 4,800,512 bytes),
// image base 0x400000. Mapped in the Pops project (next to this one) starting
// from NFSU2ExtraOptions (ExOpts Team) and our own disassembly.
#pragma once

#include <windows.h>

// Expected entry point, to check the executable version before touching it.
#define EXPECTED_ENTRY_POINT  0x75BCC7

// ---- globals ----
#define _TheGameFlowManager   0x8654A4   // 1 boot, 3 frontend, 4/5 loading, 6 gameplay
#define _hWnd                 0x870990   // window class: GameFrame
#define _IsLostFocus          0x8709E0
#define _WindowedMode         0x87098C
#define _SkipMovies           0x8650A8
#define _cFEng_pInstance      0x8384C4
#define _PlayersByNumber      0x8900AC   // the player object directly, not an array

// The ID of the career car in use. An id, not a slot index — it survives the
// garage being reordered, and it is valid in menus as well as in a race,
// because it is career state and not race state.
//
// The game looks cars up by it: in 29 places the shape is
//     mov eax, [0x863480]; push eax; mov ecx, 0x83AD90; call 0x503680
#define _CurrentCarId         0x863480

#define BOOT_STATE      1
#define FRONTEND_STATE  3
#define GAMEPLAY_STATE  6

// 0x581470 and 0x581475 are two consecutive calls to the same empty stub (a
// `ret` at 0x4022C0). NFSU2ExtraOptions takes the first and Pops the second, so
// the site lives in the ini: living with both is a matter of picking another.
#define MAINLOOP_HOOK_SITE    0x581475

// ---- game functions ----
typedef DWORD  (__cdecl* tBStringHash)(char const* s);
typedef int    (__cdecl* tFEHashUpper)(char const* s);
typedef DWORD* (__cdecl* tFEngFindPackage)(char const* pkgName);
typedef void   (__cdecl* tFEngSendMessageToPackage)(unsigned int msg, char const* pkg);
typedef int    (__thiscall* tPlayerAutoPilot)(DWORD* player);

static const tBStringHash              bStringHash              = (tBStringHash)0x43DB50;
static const tFEHashUpper              FEHashUpper              = (tFEHashUpper)0x505450;
static const tFEngFindPackage          FEngFindPackage          = (tFEngFindPackage)0x52CEF0;
static const tFEngSendMessageToPackage FEngSendMessageToPackage = (tFEngSendMessageToPackage)0x55DDA0;
static const tPlayerAutoPilot          Player_AutoPilotOn       = (tPlayerAutoPilot)0x5FAE20;
static const tPlayerAutoPilot          Player_AutoPilotOff      = (tPlayerAutoPilot)0x5FAF10;

namespace Game
{
    inline DWORD State()    { return *(DWORD*)_TheGameFlowManager; }
    inline bool  HasFocus() { return !*(bool*)_IsLostFocus; }
    inline HWND  Window()   { return *(HWND*)_hWnd; }

    inline DWORD* Player()
    {
        DWORD* p = *(DWORD**)_PlayersByNumber;
        if (!p || IsBadReadPtr(p, 4)) return 0;
        return p;
    }

    // The car "mirror": player+0x04 (seen in Player_AutoPilotOn).
    //
    // Mirror because it is the copy the game keeps for its dial and engine
    // sound: great to READ, useless to write - physics ignores whatever lands
    // here. The real authority is the physics object (hook at 0x5A5540).
    inline DWORD* Car()
    {
        DWORD* p = Player();
        if (!p) return 0;
        DWORD* car = (DWORD*)p[1];
        if (!car || IsBadReadPtr(car, 0x500)) return 0;
        return car;
    }

    // Mirror fields, all floats.
    #define CAR_SPEED     0x42C   // m/s
    #define CAR_THROTTLE  0x438   // 0 / 0.7 / 1.0 on keyboard
    #define CAR_RPM       0x440   // 800 at idle up to ~7000 (low-pass output)
    #define CAR_DISTANCE  0x4E8

    // Gearbox snapshot: car+0x34 -> +0x18. It is only written ON A SHIFT and
    // freezes in between, so it is good for the gear (+0x34) and for detecting
    // the moment of a shift - not for live RPM or throttle.
    inline DWORD* Gearbox()
    {
        DWORD* car = Car();
        if (!car) return 0;
        DWORD* a = (DWORD*)car[0x34 / 4];
        if (!a || IsBadReadPtr(a, 4)) return 0;
        DWORD* g = (DWORD*)a[0x18 / 4];
        if (!g || IsBadReadPtr(g, 0x70)) return 0;
        return g;
    }

    // The car model, by the game's own lookup (0x610130).
    //
    // The garage entry holds a TYPE HASH at +0x20; the model table is searched
    // by that hash, not indexed. Treating the number as an index is what made a
    // 350Z report itself as "MIATA" — two unrelated values.
    //
    //     model table   [0x8A1CCC], 46 entries of 0x890 bytes
    //     entry + 0x00  the name ("350Z", "RX8", "SKYLINE")
    //     entry + 0xD0  the type hash
    #define _GarageContainer  0x83AD90
    #define _GarageFindCar    0x503680
    #define _ModelTable       0x8A1CCC
    #define MODEL_ENTRY       0x890
    #define MODEL_COUNT       46
    #define MODEL_HASH        0xD0
    #define CAR_TYPE_HASH     0x20

    typedef void* (__thiscall* tGarageFindCar)(void* container, int carId);

    // The garage entry for the car in use, or null.
    inline void* CarEntry()
    {
        int id = *(int*)_CurrentCarId;
        if (!id || id == -1) return 0;

        tGarageFindCar find = (tGarageFindCar)_GarageFindCar;
        void* entry = find((void*)_GarageContainer, id);
        return (entry && !IsBadReadPtr(entry, CAR_TYPE_HASH + 4)) ? entry : 0;
    }

    // "350Z", "SKYLINE", ... or null outside a career car.
    inline const char* CarModel()
    {
        void* entry = CarEntry();
        if (!entry) return 0;

        DWORD hash = *(DWORD*)((char*)entry + CAR_TYPE_HASH);
        char* table = *(char**)_ModelTable;
        if (!hash || !table || IsBadReadPtr(table, MODEL_ENTRY)) return 0;

        for (int i = 0; i < MODEL_COUNT; i++)
        {
            char* base = table + i * MODEL_ENTRY;
            if (IsBadReadPtr(base, MODEL_HASH + 4)) break;
            if (*(DWORD*)(base + MODEL_HASH) == hash) return base;
        }
        return 0;
    }

    // 0 means no career car loaded (main menu before a save, say).
    inline int CarId()
    {
        int id = *(int*)_CurrentCarId;
        return (id == -1) ? 0 : id;
    }

    inline bool VersionMatches()
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        uintptr_t entry = base + nt->OptionalHeader.AddressOfEntryPoint;
        return (entry + (0x400000 - base)) == EXPECTED_ENTRY_POINT;
    }
}
