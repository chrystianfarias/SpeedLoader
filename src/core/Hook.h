#pragma once

#include <windows.h>
#include <stdint.h>

// Prologue detour: saves `len` original bytes into a trampoline and writes a
// JMP rel32 to the destination. `len` must land on an instruction boundary
// (check it in a disassembler; Pops' tools\prologue.py answers that).
struct Trampoline
{
    uint8_t code[32];

    void* Install(uintptr_t target, void* detour, size_t len)
    {
        DWORD old;
        VirtualProtect(code, sizeof(code), PAGE_EXECUTE_READWRITE, &old);
        memcpy(code, (void*)target, len);
        code[len] = 0xE9;
        *(int32_t*)(code + len + 1) =
            (int32_t)((target + len) - ((uintptr_t)code + len + 5));

        VirtualProtect((void*)target, len, PAGE_EXECUTE_READWRITE, &old);
        uint8_t* p = (uint8_t*)target;
        p[0] = 0xE9;
        *(int32_t*)(p + 1) = (int32_t)((uintptr_t)detour - (target + 5));
        for (size_t i = 5; i < len; i++) p[i] = 0x90;
        VirtualProtect((void*)target, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)target, len);

        return code;
    }
};

// Repoints an existing CALL rel32, returning the previous target.
inline uintptr_t RedirectCall(uintptr_t callSite, void* newTarget)
{
    DWORD old;
    VirtualProtect((void*)callSite, 5, PAGE_EXECUTE_READWRITE, &old);
    uintptr_t prev = callSite + 5 + *(int32_t*)(callSite + 1);
    *(int32_t*)(callSite + 1) = (int32_t)((uintptr_t)newTarget - (callSite + 5));
    VirtualProtect((void*)callSite, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)callSite, 5);
    return prev;
}

// How many bytes to copy into a trampoline: sums whole instructions until it
// covers `need` bytes (5, the size of a JMP rel32).
//
// This is not a disassembler: it covers the prologue vocabulary (push, mov,
// sub, lea, test, xor) and gives up on anything outside it. Giving up is the
// right answer - a relative jump copied to another address would point at the
// wrong place, and half an instruction becomes executable garbage.
inline size_t CopyLength(const uint8_t* code, size_t need)
{
    size_t total = 0;

    while (total < need)
    {
        const uint8_t* p = code + total;
        size_t len = 0;

        // Prefixes that show up in prologues (operand size, segment).
        while (*p == 0x66 || *p == 0x67 || *p == 0x64 || *p == 0x65 ||
               *p == 0xF2 || *p == 0xF3)
        {
            len++;
            p++;
        }

        uint8_t op = *p;
        bool hasModrm = false;
        size_t imm = 0;

        if (op >= 0x50 && op <= 0x5F)           len += 1;              // push/pop reg
        else if (op >= 0xB8 && op <= 0xBF)      len += 5;              // mov r32, imm32
        else if (op == 0x6A)                    len += 2;              // push imm8
        else if (op == 0x68)                    len += 5;              // push imm32
        else if (op == 0x90)                    len += 1;              // nop
        else if (op == 0xA1 || op == 0xA3)      len += 5;              // mov eax, [moffs]
        else if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B ||
                 op == 0x8D || op == 0x85 || op == 0x31 || op == 0x33 ||
                 op == 0x01 || op == 0x03 || op == 0x29 || op == 0x2B ||
                 op == 0x09 || op == 0x0B || op == 0x21 || op == 0x23 ||
                 op == 0x39 || op == 0x3B || op == 0xFF)
        { hasModrm = true; len += 1; }
        else if (op == 0x83 || op == 0x6B || op == 0xC6)
        { hasModrm = true; len += 1; imm = 1; }
        else if (op == 0x81 || op == 0x69 || op == 0xC7)
        { hasModrm = true; len += 1; imm = 4; }
        else
            return 0;   // unknown, or relative (E8/E9/EB/Jcc): do not copy

        if (hasModrm)
        {
            uint8_t modrm = p[1];   // comes right after the opcode
            uint8_t mod = modrm >> 6, rm = modrm & 7;
            len += 1;

            if (mod != 3 && rm == 4)            // SIB byte
            {
                uint8_t sib = p[2];
                len += 1;
                if (mod == 0 && (sib & 7) == 5) len += 4;
            }
            if (mod == 1)                       len += 1;
            else if (mod == 2)                  len += 4;
            else if (mod == 0 && rm == 5)       len += 4;   // [disp32]

            len += imm;
        }

        total += len;
    }

    return total;
}

// Swaps an entry in the executable's import table. This is how we catch
// Direct3DCreate9 before the game creates its device.
inline void* HookIat(const char* dllName, const char* funcName, void* newFunc)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;

    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
         imp->Name; imp++)
    {
        if (_stricmp((const char*)(base + imp->Name), dllName) != 0) continue;

        IMAGE_THUNK_DATA* orig = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* cur  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, cur++)
        {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* n =
                (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
            if (strcmp((const char*)n->Name, funcName) != 0) continue;

            DWORD old;
            VirtualProtect(&cur->u1.Function, sizeof(void*),
                           PAGE_READWRITE, &old);
            void* prev = (void*)cur->u1.Function;
            cur->u1.Function = (uintptr_t)newFunc;
            VirtualProtect(&cur->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return 0;
}

// Swaps a vtable slot (COM). Returns the previous pointer.
inline void* HookVTable(void* instance, int index, void* newFunc)
{
    void** vtbl = *(void***)instance;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old);
    void* prev = vtbl[index];
    vtbl[index] = newFunc;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return prev;
}
