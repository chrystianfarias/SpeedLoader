// speed.engine - writing back into the engine, from a mod.
//
// The split is deliberate. A mod decides the policy in JavaScript (which key,
// which RPM, how much hysteresis) and the native side does the writing, because
// these writes only work at one point inside the frame: the input fills the
// throttle, the physics consumes it, and only then do mods run. A value written
// from a "frame" callback is always a frame late.
//
// So the JS side never writes the engine itself. It configures a limiter and
// says whether the button is held; the rest happens where it has to.
#include "Api.h"

#include "game/EngineControl.h"
#include "game/Vehicle.h"

namespace
{
    bool GetBool(JSContext* ctx, JSValueConst obj, const char* key, bool fallback)
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        bool out = JS_IsUndefined(v) ? fallback : JS_ToBool(ctx, v) == 1;
        JS_FreeValue(ctx, v);
        return out;
    }

    float GetFloat(JSContext* ctx, JSValueConst obj, const char* key, float fallback)
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        double d = fallback;
        if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        return (float)d;
    }

    // speed.engine.limiter({ enabled, rpm, hysteresis, antiLag, holdOnShift })
    JSValue Limiter(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc >= 1 && JS_IsObject(argv[0]))
        {
            EngineControl::g_enabled =
                GetBool(ctx, argv[0], "enabled", EngineControl::g_enabled);
            EngineControl::g_limitRpm =
                GetFloat(ctx, argv[0], "rpm", EngineControl::g_limitRpm);
            EngineControl::g_hysteresis =
                GetFloat(ctx, argv[0], "hysteresis", EngineControl::g_hysteresis);
            EngineControl::g_antiLag =
                GetBool(ctx, argv[0], "antiLag", EngineControl::g_antiLag);
            EngineControl::g_holdOnShift =
                GetBool(ctx, argv[0], "holdOnShift", EngineControl::g_holdOnShift);
        }

        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "enabled",     JS_NewBool(ctx, EngineControl::g_enabled));
        JS_SetPropertyStr(ctx, out, "rpm",         JS_NewFloat64(ctx, EngineControl::g_limitRpm));
        JS_SetPropertyStr(ctx, out, "hysteresis",  JS_NewFloat64(ctx, EngineControl::g_hysteresis));
        JS_SetPropertyStr(ctx, out, "antiLag",     JS_NewBool(ctx, EngineControl::g_antiLag));
        JS_SetPropertyStr(ctx, out, "holdOnShift", JS_NewBool(ctx, EngineControl::g_holdOnShift));
        return out;
    }

    // speed.engine.hold(true) while the button is down.
    JSValue Hold(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc >= 1) EngineControl::g_engaged = JS_ToBool(ctx, argv[0]) == 1;
        return JS_NewBool(ctx, EngineControl::g_engaged);
    }

    // The gear change does not go through the game's cut timer, so the mod has
    // to say when one happened for anti-lag to carry the boost across it.
    JSValue Shift(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int32_t ms = 500;
        if (argc >= 1) JS_ToInt32(ctx, &ms, argv[0]);
        EngineControl::g_shiftUntil = GetTickCount() + (DWORD)ms;
        return JS_UNDEFINED;
    }

    // What the panel needs: is it cutting, how many times, and the engine's own
    // RPM (from physics, in real rpm - not the filtered copy the dial uses).
    JSValue State(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "cutting", JS_NewBool(ctx, EngineControl::g_cutting));
        JS_SetPropertyStr(ctx, out, "cuts",    JS_NewInt32(ctx, EngineControl::g_cutCount));

        float rpm = EngineControl::Rpm();
        JS_SetPropertyStr(ctx, out, "rpm",
                          rpm < 0.0f ? JS_NULL : JS_NewFloat64(ctx, rpm));

        char* engine = (char*)Vehicle::Engine();
        JS_SetPropertyStr(ctx, out, "throttle",
                          engine ? JS_NewFloat64(ctx, *(float*)(engine + ENGINE_THROTTLE))
                                 : JS_NULL);
        JS_SetPropertyStr(ctx, out, "redline",
                          engine ? JS_NewFloat64(ctx, *(float*)(engine + ENGINE_REDLINE))
                                 : JS_NULL);
        return out;
    }
}

namespace Js
{
    void RegisterEngine(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue engine = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, engine, "limiter", JS_NewCFunction(ctx, Limiter, "limiter", 1));
        JS_SetPropertyStr(ctx, engine, "hold",    JS_NewCFunction(ctx, Hold, "hold", 1));
        JS_SetPropertyStr(ctx, engine, "shift",   JS_NewCFunction(ctx, Shift, "shift", 1));
        JS_SetPropertyStr(ctx, engine, "state",   JS_NewCFunction(ctx, State, "state", 0));

        JS_SetPropertyStr(ctx, speed, "engine", engine);
    }
}
