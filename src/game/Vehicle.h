#pragma once

#include <windows.h>
#include <stdint.h>

#include "core/Hook.h"
#include "core/Log.h"
#include "game/Game.h"

// Captures the vehicle PHYSICS object.
//
// The car mirror (player+0x04) gives speed, RPM and throttle, but stops at what
// the dial needs. Boost, wheels and the real engine live in physics, and
// physics is not hanging off any global: the way to get it is to wait for the
// game to call 0x5A5540, which is __thiscall and brings the object in ecx.
//
//   physics + 0x20 / + 0x48   engine object (two aliases for the same thing)
//   physics + 0x58            the car mirror
//   physics + 0x5C            timestamp of the game's own ignition cut
//   engine  + 0x14            engine speed, in RAD/S
//   engine  + 0x80            the input throttle, before the cut zeroes it
//   engine  + 0x9C            current boost pressure (negative under vacuum)
//   engine  + 0xA4            the instantaneous TARGET from the engine map -
//                             not the car's ceiling: it drops while the
//                             throttle is cut
//   engine  + 0xAC            vacuum floor
//
// One thing that is not obvious and costs a day to learn the hard way: the
// order inside a frame is
//
//     input writes engine+0x80  ->  physics consumes  ->  0x5A5540  ->  mods
//
// so anything written from a frame callback lands AFTER the physics already
// read it. Values that are inputs (throttle, the cut timestamp) have to be
// written from inside the pass; values that are state (RPM, boost) must not be
// written at all - pinning them does not cut power, it breaks the integrator
// that computes everything else from them, and the car accelerates without
// limit.
namespace Vehicle
{
    #define VEHICLE_UPDATE   0x5A5540
    #define PHYSICS_ENGINE   0x48
    // physics+0x58 points INTO the mirror, 0x40 past its start - not at the
    // mirror itself. Proof: the smoothed RPM is written at [physics+0x58]+0x400
    // and read from mirror+0x440, and a hardware breakpoint on mirror+0x470
    // fired on a write to [edx+0x430] with edx = [physics+0x58].
    //
    // Comparing the two directly never matches, so the capture below silently
    // never fired and boost was always null.
    #define PHYSICS_MIRROR   0x58
    #define MIRROR_SKEW      0x40

    // The ignition cut the game already has, and uses on every gear change.
    // physics+0x5C is a TIMESTAMP of when the cut began, not a countdown: the
    // check at 0x5A1629 cuts while it differs from zero and clears it once
    // 0.45s have passed. Stamp it with GAME_CLOCK * CUT_TIME_SCALE to cut;
    // write zero to end the cut early.
    #define PHYSICS_CUT_TIMER 0x5C
    #define CUT_ZERO          0x7D7D3C   // the 0.0 it is compared against
    #define CUT_DURATION      0x7A18C8   // 0.45 seconds
    #define CUT_TIME_SCALE    0x784268   // 0.00025
    #define GAME_CLOCK        0x86518C   // int counter; * scale = seconds

    // The engine. RPM here is in RAD/S, not rpm: divide by 2*pi/60.
    #define ENGINE_RPM       0x14
    #define ENGINE_IDLE      0x28   // 800 or 850, per car
    #define ENGINE_REDLINE   0x2C   // 7000 or 7500, per car
    #define ENGINE_MULT      0x78   // multiplier applied to the throttle
    #define ENGINE_THROTTLE  0x80   // the INPUT throttle (0 / 0.7 / 1.0)
    #define ENGINE_BOOST     0x9C
    #define ENGINE_BOOST_MAX 0xA4
    #define ENGINE_BOOST_MIN 0xAC

    // The turbo model: one function, one call site. Its integrator lives at
    // 0x5A1094 (pressure += increment), which is why writing the pressure from
    // outside only ever produces a sawtooth. To hold boost, wrap the call and
    // let the function run seeing a full throttle.
    #define TURBO_UPDATE     0x5A0F30
    #define TURBO_CALL_SITE  0x5AA7F0

    // Car effects (CARFX). The player's effects object hangs off the mirror.
    #define MIRROR_CARFX     0x554
    #define CARFX_NITRO           10
    #define CARFX_EXHAUST_SMOKE   11
    #define CARFX_EXHAUST_BLOWOFF 12
    #define CARFX_NOS_BLOWOFF     13

    // ---------------------------------------------------------- Geometria
    //
    // Tudo abaixo foi medido dentro do jogo, com um cubo desenhado na cena, e
    // custou caro o bastante para merecer ficar escrito.
    //
    // O MUNDO e Z PARA CIMA: um carro parado fica com x,y na casa das centenas
    // e z na casa das dezenas (a altura do terreno).
    //
    // A POSE do carro (physics+0x20) traz a posicao em +0x20 e a rotacao em
    // +0x30, tres linhas de quatro floats. As LINHAS sao os eixos do carro no
    // mundo, entao um ponto local vira mundo multiplicando pelas COLUNAS:
    //
    //     mundo.x = pos.x + lx*m[0] + ly*m[4] + lz*m[8]
    //
    // A transposta tambem preserva distancias e por isso NENHUMA medicao
    // numerica separa as duas; o que separa e desenhar as duas na tela — com a
    // errada o ponto fica espelhado e so parece certo de alguns angulos.
    //
    // A posicao da pose e a da FISICA (centro de massa), que NAO e a origem do
    // modelo — e os marcadores do modelo sao medidos a partir desta ultima. A
    // diferenca esta em pose+0x1F0, como (frente, lado, altura) nos eixos do
    // carro: da 0.300 num carro e 0.220 em outro, e o componente lateral e
    // sempre zero, porque o centro de massa cai no plano de simetria.
    #define POSE             0x20
    #define POSE_POSITION    0x20
    #define POSE_ROTATION    0x30
    #define POSE_MODEL_ORIGIN 0x1F0

    // Os marcadores do modelo (o LEFT_EXHAUST e companhia) ficam numa lista em
    // carfx+0x598, montada pelo jogo — ela ja reflete o para-choque EQUIPADO,
    // que e o que dispensa mapear posicao de escape por kit.
    //
    // A lista e circular e cada no tem 20 bytes:
    //
    //     +0x00  proximo (volta a cabeca no ultimo)
    //     +0x04  x, y, z   em coordenadas do MODELO
    //     +0x10  ponteiro para a orientacao do marcador
    //
    // A orientacao e uma matriz 3x4 a partir de +0x10 do alvo: para o escape as
    // linhas sao (0,0,1), (0,1,0), (-1,0,0) — o eixo Z do marcador aponta para
    // a traseira, que e para onde o cano sopra.
    #define CARFX_MARKERS    0x598
    #define MARKER_NEXT      0x00
    #define MARKER_POSITION  0x04
    #define MARKER_ORIENT    0x10
    #define MARKER_STRIDE    0x14

    inline void*     g_physics = 0;
    inline uintptr_t g_continue = 0;   // trampoline, or the hook already there

    // 0x5A5540 runs for EVERY vehicle in the world - traffic and opponents
    // included. Storing ecx from any call would make the object flip between
    // cars several times per frame, and boost in the HUD turned into noise.
    //
    // The filter is the mirror: only the vehicle whose physics+0x58 matches the
    // player's mirror is ours.
    inline void __cdecl Capture(void* self)
    {
        if (!self || IsBadReadPtr(self, PHYSICS_MIRROR + 4)) return;

        void* mirror = *(void**)((char*)self + PHYSICS_MIRROR);
        void* ours = (char*)Game::Car() + MIRROR_SKEW;
        if (mirror && mirror == ours) g_physics = self;
    }

    // Naked because this detour stands in for the function's prologue: it must
    // hand back every register and flag exactly as it found them.
    __declspec(naked) inline void VehicleHook()
    {
        __asm {
            pushad
            pushfd
            push ecx            // this = the vehicle of this call
            call Capture
            add  esp, 4
            popfd
            popad
            jmp dword ptr [g_continue]
        }
    }

    inline bool Install()
    {
        uint8_t* p = (uint8_t*)VEHICLE_UPDATE;

        if (*p == 0xE9)
        {
            // Someone got here first (Pops hooks this too). Their JMP cannot be
            // copied into a trampoline - a copied rel32 points at the wrong
            // place - so we install in front and jump to them: both run, in
            // installation order.
            g_continue = (uintptr_t)p + 5 + *(int32_t*)(p + 1);
            Log("physics: chaining to the previous hook (0x%08X)", g_continue);

            DWORD old;
            VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
            *(int32_t*)(p + 1) =
                (int32_t)((uintptr_t)VehicleHook - ((uintptr_t)p + 5));
            VirtualProtect(p, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 5);
        }
        else
        {
            size_t len = CopyLength(p, 5);
            if (len == 0)
            {
                Log("physics: prologue at 0x%08X I cannot copy"
                    " (%02X %02X %02X %02X %02X) - boost unavailable",
                    VEHICLE_UPDATE, p[0], p[1], p[2], p[3], p[4]);
                return false;
            }

            // Install already writes the JMP and returns the prologue trampoline.
            static Trampoline tramp;
            g_continue = (uintptr_t)tramp.Install(VEHICLE_UPDATE,
                                                  (void*)VehicleHook, len);
        }

        Log("physics: hooked at 0x%08X", VEHICLE_UPDATE);
        return true;
    }

    inline void* Physics()
    {
        if (!g_physics || IsBadReadPtr(g_physics, PHYSICS_MIRROR + 4)) return 0;

        // Check again on read: when the player changes car (or leaves the race)
        // the stored pointer goes stale, and reading from a freed object is
        // worse than showing no boost at all.
        if (*(void**)((char*)g_physics + PHYSICS_MIRROR)
            != (void*)((char*)Game::Car() + MIRROR_SKEW))
            return 0;

        return g_physics;
    }

    inline void* Engine()
    {
        void* phys = Physics();
        if (!phys) return 0;
        void* engine = *(void**)((char*)phys + PHYSICS_ENGINE);
        if (!engine || IsBadReadPtr(engine, 0xB0)) return 0;
        return engine;
    }
}
