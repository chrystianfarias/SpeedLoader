#include "ui/DInputBlock.h"

#include "core/Log.h"
#include "ui/InputRouter.h"

#include <windows.h>

namespace
{
    // Only the two vtable slots that matter, by index - including dinput.h just
    // for two function pointers would drag COM headers into the build for no
    // gain.
    const int VT_CREATE_DEVICE   = 3;    // IDirectInput8::CreateDevice
    const int VT_GET_DEVICE_STATE = 9;   // IDirectInputDevice8::GetDeviceState
    const int VT_GET_DEVICE_DATA  = 10;  // IDirectInputDevice8::GetDeviceData

    typedef HRESULT (STDMETHODCALLTYPE* tGetDeviceState)(void*, DWORD, void*);
    typedef HRESULT (STDMETHODCALLTYPE* tGetDeviceData)(void*, DWORD, void*,
                                                        DWORD*, DWORD);
    typedef HRESULT (STDMETHODCALLTYPE* tCreateDevice)(void*, const GUID&, void**, void*);
    typedef HRESULT (WINAPI* tDirectInput8Create)(HINSTANCE, DWORD, const IID&,
                                                  void**, void*);

    tGetDeviceState     g_origGetState = 0;
    tGetDeviceData      g_origGetData  = 0;
    tCreateDevice       g_origCreate   = 0;
    tDirectInput8Create g_origCreateDI = 0;

    void* HookVTable(void* obj, int index, void* novo)
    {
        void** vt = *(void***)obj;
        DWORD old;
        VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old);
        void* anterior = vt[index];
        vt[index] = novo;
        VirtualProtect(&vt[index], sizeof(void*), old, &old);
        return anterior;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStateHook(void* self, DWORD tamanho, void* dados)
    {
        HRESULT hr = g_origGetState(self, tamanho, dados);

        // 256 bytes is the keyboard state; the mouse and pads have their own
        // sizes and are left alone, so the player can still look around with the
        // mouse while typing.
        if (SUCCEEDED(hr) && dados && tamanho == 256 && InputRouter::IsCapturing())
            memset(dados, 0, tamanho);

        return hr;
    }

    // O outro caminho, e o que o NFSU2 usa de fato: leitura em BUFFER, onde os
    // eventos de tecla vem numa fila em vez de um retrato do teclado. Zerar o
    // retrato nao adianta nada se o jogo nunca o pede.
    //
    // Enquanto o console esta aberto, a fila e drenada e reportada como vazia:
    // os eventos somem para o jogo, mas nao ficam acumulados para chegarem
    // todos juntos quando a barra fechar.
    HRESULT STDMETHODCALLTYPE GetDeviceDataHook(void* self, DWORD tamanhoItem,
                                                void* dados, DWORD* quantidade,
                                                DWORD flags)
    {
        HRESULT hr = g_origGetData(self, tamanhoItem, dados, quantidade, flags);

        if (SUCCEEDED(hr) && quantidade && InputRouter::IsCapturing())
            *quantidade = 0;

        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateDeviceHook(void* self, const GUID& guid,
                                               void** dispositivo, void* agg)
    {
        HRESULT hr = g_origCreate(self, guid, dispositivo, agg);
        if (SUCCEEDED(hr) && dispositivo && *dispositivo && !g_origGetState)
        {
            // Teclado e mouse compartilham a mesma vtable, entao um hook cobre
            // os dois; a filtragem e feita por tipo de leitura, nao por
            // dispositivo.
            g_origGetState = (tGetDeviceState)HookVTable(*dispositivo,
                                                         VT_GET_DEVICE_STATE,
                                                         (void*)GetDeviceStateHook);
            g_origGetData = (tGetDeviceData)HookVTable(*dispositivo,
                                                       VT_GET_DEVICE_DATA,
                                                       (void*)GetDeviceDataHook);
            LogTag("in  ", "keyboard reads wrapped (state + buffered) on 0x%p",
                   *dispositivo);
        }
        return hr;
    }

    HRESULT WINAPI DirectInput8CreateHook(HINSTANCE inst, DWORD versao,
                                          const IID& iid, void** saida, void* agg)
    {
        HRESULT hr = g_origCreateDI(inst, versao, iid, saida, agg);
        if (SUCCEEDED(hr) && saida && *saida && !g_origCreate)
        {
            g_origCreate = (tCreateDevice)HookVTable(*saida, VT_CREATE_DEVICE,
                                                     (void*)CreateDeviceHook);
            LogTag("in  ", "DirectInput wrapped (0x%p)", *saida);
        }
        return hr;
    }
}

namespace DInputBlock
{
    void Install()
    {
        // Through the import table, so this works whether or not the game has
        // created its devices yet - the game calls DirectInput8Create itself,
        // and that is where we get in.
        HMODULE base = GetModuleHandleA(0);
        if (!base) return;

        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);

        DWORD rva = nt->OptionalHeader
                      .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return;

        IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + rva);

        for (; imp->Name; imp++)
        {
            const char* dll = (const char*)((BYTE*)base + imp->Name);
            if (_stricmp(dll, "dinput8.dll") != 0) continue;

            IMAGE_THUNK_DATA* nomes = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA* enderecos = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->FirstThunk);

            for (; nomes->u1.AddressOfData; nomes++, enderecos++)
            {
                if (IMAGE_SNAP_BY_ORDINAL(nomes->u1.Ordinal)) continue;

                IMAGE_IMPORT_BY_NAME* nome =
                    (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + nomes->u1.AddressOfData);
                if (strcmp((const char*)nome->Name, "DirectInput8Create") != 0) continue;

                DWORD old;
                VirtualProtect(&enderecos->u1.Function, sizeof(void*),
                               PAGE_READWRITE, &old);
                g_origCreateDI = (tDirectInput8Create)enderecos->u1.Function;
                enderecos->u1.Function = (DWORD_PTR)DirectInput8CreateHook;
                VirtualProtect(&enderecos->u1.Function, sizeof(void*), old, &old);

                LogTag("in  ", "DirectInput8Create wrapped in the import table");
                return;
            }
        }

        LogTag("in  ", "DirectInput8Create not found in the imports");
    }
}
