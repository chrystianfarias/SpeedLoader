// speed.store - what a mod keeps between sessions.
//
// One JSON file per mod, in "<game>\scripts\SpeedLoader\data\<modid>.json".
//
// The data folder sits OUTSIDE mods\ on purpose: installing a mod copies the
// mods folder over, and a save living in there would be wiped by an update of
// the very mod that wrote it. A fuel tank surviving a session but not an
// install would be worse than no persistence at all.
//
// Writes are immediate rather than batched at shutdown - a game being modded
// crashes, and a save that only lands on a clean exit is the one you do not
// have when it matters.
#include "Api.h"

#include "core/Config.h"
#include "game/Game.h"
#include "core/Log.h"

#include <direct.h>
#include <stdio.h>
#include <map>
#include <string>

namespace
{
    // The whole file, as a parsed object, one per mod.
    std::map<std::string, JSValue> g_data;

    std::string PathFor(const std::string& modId)
    {
        char dir[MAX_PATH];
        Config::Resolve("data", dir, sizeof(dir));
        _mkdir(dir);

        return std::string(dir) + "\\" + modId + ".json";
    }

    JSValue Load(JSContext* ctx, Js::Mod* mod)
    {
        auto it = g_data.find(mod->id);
        if (it != g_data.end()) return it->second;

        JSValue obj = JS_NewObject(ctx);

        FILE* f = fopen(PathFor(mod->id).c_str(), "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (size > 0 && size < 4 * 1024 * 1024)
            {
                std::string text((size_t)size, '\0');
                if (fread(&text[0], 1, (size_t)size, f) == (size_t)size)
                {
                    JSValue parsed = JS_ParseJSON(ctx, text.c_str(), text.size(),
                                                  "<store>");
                    if (JS_IsException(parsed))
                    {
                        // A corrupt file must not take the mod down with it:
                        // log it and start clean.
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        LogJs("store for %s is unreadable - starting empty",
                              mod->id.c_str());
                    }
                    else if (JS_IsObject(parsed))
                    {
                        JS_FreeValue(ctx, obj);
                        obj = parsed;
                        parsed = JS_UNDEFINED;
                    }
                    JS_FreeValue(ctx, parsed);
                }
            }
            fclose(f);
        }

        g_data[mod->id] = obj;
        return obj;
    }

    void Persist(JSContext* ctx, Js::Mod* mod, JSValue obj)
    {
        JSValue json = JS_JSONStringify(ctx, obj, JS_UNDEFINED, JS_NewInt32(ctx, 2));
        const char* text = JS_ToCString(ctx, json);

        if (text)
        {
            FILE* f = fopen(PathFor(mod->id).c_str(), "wb");
            if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
            else LogJs("store for %s could not be written", mod->id.c_str());
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, json);
    }

    JSValue Get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1) return JS_ThrowTypeError(ctx, "speed.store.get(key, fallback?)");

        const char* key = JS_ToCString(ctx, argv[0]);
        if (!key) return JS_EXCEPTION;

        JSValue obj = Load(ctx, mod);
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        JS_FreeCString(ctx, key);

        if (JS_IsUndefined(v) && argc >= 2)
        {
            JS_FreeValue(ctx, v);
            return JS_DupValue(ctx, argv[1]);
        }
        return v;
    }

    JSValue Set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 2) return JS_ThrowTypeError(ctx, "speed.store.set(key, value)");

        const char* key = JS_ToCString(ctx, argv[0]);
        if (!key) return JS_EXCEPTION;

        JSValue obj = Load(ctx, mod);
        JS_SetPropertyStr(ctx, obj, key, JS_DupValue(ctx, argv[1]));
        JS_FreeCString(ctx, key);

        Persist(ctx, mod, obj);
        return JS_UNDEFINED;
    }

    // ---------------------------------------------------- por carro
    //
    // "Guardar algo por carro" nao e necessidade de um mod so: mapa de injecao,
    // desgaste, preparacao — tudo isso pertence ao carro, nao ao perfil. Entao
    // o namespace fica no SDK, e nao copiado dentro de cada mod.
    //
    // A chave e o id da carreira (Game::CarId). Sem carro carregado, get
    // devolve o padrao e set nao grava: e melhor nao guardar do que guardar no
    // lugar errado e misturar dois carros.
    JSValue CarrosDe(JSContext* ctx, Js::Mod* mod)
    {
        JSValue raiz = Load(ctx, mod);
        JSValue carros = JS_GetPropertyStr(ctx, raiz, "carros");
        if (!JS_IsObject(carros))
        {
            JS_FreeValue(ctx, carros);
            carros = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, raiz, "carros", JS_DupValue(ctx, carros));
        }
        return carros;
    }

    JSValue CarGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1)
            return JS_ThrowTypeError(ctx, "speed.store.car.get(key, fallback?)");

        int id = Game::CarId();
        if (!id) return argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;

        const char* key = JS_ToCString(ctx, argv[0]);
        if (!key) return JS_EXCEPTION;

        char idStr[24];
        _snprintf(idStr, sizeof(idStr), "%d", id);

        JSValue carros = CarrosDe(ctx, mod);
        JSValue carro = JS_GetPropertyStr(ctx, carros, idStr);
        JSValue v = JS_IsObject(carro) ? JS_GetPropertyStr(ctx, carro, key)
                                       : JS_UNDEFINED;
        JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, carro);
        JS_FreeValue(ctx, carros);

        if (JS_IsUndefined(v) && argc >= 2)
        {
            JS_FreeValue(ctx, v);
            return JS_DupValue(ctx, argv[1]);
        }
        return v;
    }

    JSValue CarSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 2)
            return JS_ThrowTypeError(ctx, "speed.store.car.set(key, value)");

        int id = Game::CarId();
        if (!id) return JS_NewBool(ctx, false);

        const char* key = JS_ToCString(ctx, argv[0]);
        if (!key) return JS_EXCEPTION;

        char idStr[24];
        _snprintf(idStr, sizeof(idStr), "%d", id);

        JSValue carros = CarrosDe(ctx, mod);
        JSValue carro = JS_GetPropertyStr(ctx, carros, idStr);
        if (!JS_IsObject(carro))
        {
            JS_FreeValue(ctx, carro);
            carro = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, carros, idStr, JS_DupValue(ctx, carro));
        }

        JS_SetPropertyStr(ctx, carro, key, JS_DupValue(ctx, argv[1]));
        JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, carro);
        JS_FreeValue(ctx, carros);

        Persist(ctx, mod, Load(ctx, mod));
        return JS_NewBool(ctx, true);
    }

    JSValue CarId(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        int id = Game::CarId();
        return id ? JS_NewInt32(ctx, id) : JS_NULL;
    }

    JSValue All(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod) return JS_NewObject(ctx);
        return JS_DupValue(ctx, Load(ctx, mod));
    }

    JSValue Clear(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod) return JS_UNDEFINED;

        auto it = g_data.find(mod->id);
        if (it != g_data.end()) { JS_FreeValue(ctx, it->second); g_data.erase(it); }

        JSValue obj = JS_NewObject(ctx);
        g_data[mod->id] = obj;
        Persist(ctx, mod, obj);
        return JS_UNDEFINED;
    }
}

namespace Js
{
    void RegisterStore(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue store = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, store, "get",   JS_NewCFunction(ctx, Get, "get", 2));
        JS_SetPropertyStr(ctx, store, "set",   JS_NewCFunction(ctx, Set, "set", 2));
        JS_SetPropertyStr(ctx, store, "all",   JS_NewCFunction(ctx, All, "all", 0));
        JS_SetPropertyStr(ctx, store, "clear", JS_NewCFunction(ctx, Clear, "clear", 0));

        JSValue carro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, carro, "get", JS_NewCFunction(ctx, CarGet, "get", 2));
        JS_SetPropertyStr(ctx, carro, "set", JS_NewCFunction(ctx, CarSet, "set", 2));
        JS_SetPropertyStr(ctx, carro, "id",  JS_NewCFunction(ctx, CarId, "id", 0));
        JS_SetPropertyStr(ctx, store, "car", carro);

        JS_SetPropertyStr(ctx, speed, "store", store);
    }
}
