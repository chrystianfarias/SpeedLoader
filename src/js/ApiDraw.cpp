// speed.draw - sprites dentro da cena 3D do jogo.
//
// Diferente da interface: o Chromium compoe por cima de tudo, plano e sem
// profundidade. Uma chama no escape precisa ficar onde o carro esta, encolher
// com a distancia e sumir atras da lataria quando a camera gira — entao e
// desenhada como geometria, no EndScene, com a camera que o jogo ja tem
// montada.
#include "Api.h"

#include "ui/WorldDraw.h"

namespace
{
    double Num(JSContext* ctx, JSValueConst obj, const char* chave, double padrao)
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, chave);
        double d = padrao;
        if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        return d;
    }

    // speed.draw.spark(x, y, z, { size, life, color })
    JSValue Spark(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 3)
            return JS_ThrowTypeError(ctx, "speed.draw.spark(x, y, z, opcoes?)");

        double x = 0, y = 0, z = 0;
        if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
            JS_ToFloat64(ctx, &z, argv[2]))
            return JS_EXCEPTION;

        double tamanho = 0.25, vida = 0.25;
        uint32_t cor = 0xFF8020, corFim = 0;
        bool presa = false;
        double vx = 0, vy = 0, vz = 0;
        bool temCorFim = false;

        if (argc >= 4 && JS_IsObject(argv[3]))
        {
            tamanho = Num(ctx, argv[3], "size", tamanho);
            vida = Num(ctx, argv[3], "life", vida);

            JSValue at = JS_GetPropertyStr(ctx, argv[3], "attached");
            presa = JS_ToBool(ctx, at) != 0;
            JS_FreeValue(ctx, at);

            JSValue vel = JS_GetPropertyStr(ctx, argv[3], "vel");
            if (JS_IsObject(vel))
                for (int i = 0; i < 3; i++)
                {
                    JSValue e = JS_GetPropertyUint32(ctx, vel, i);
                    double d = 0;
                    JS_ToFloat64(ctx, &d, e);
                    JS_FreeValue(ctx, e);
                    (i == 0 ? vx : i == 1 ? vy : vz) = d;
                }
            JS_FreeValue(ctx, vel);

            JSValue cf = JS_GetPropertyStr(ctx, argv[3], "color2");
            if (!JS_IsUndefined(cf))
            {
                int32_t n = 0;
                JS_ToInt32(ctx, &n, cf);
                corFim = (uint32_t)n;
                temCorFim = true;
            }
            JS_FreeValue(ctx, cf);

            JSValue c = JS_GetPropertyStr(ctx, argv[3], "color");
            if (!JS_IsUndefined(c))
            {
                int32_t v = 0;
                JS_ToInt32(ctx, &v, c);
                cor = (uint32_t)v;
            }
            JS_FreeValue(ctx, c);
        }

        WorldDraw::Spawn((float)x, (float)y, (float)z, (float)tamanho,
                         (float)vida, cor, presa, (float)vx, (float)vy, (float)vz,
                         temCorFim ? corFim : cor);
        return JS_UNDEFINED;
    }

    // speed.draw.camera({ view, fov, near, far, depth })
    //
    // O jogo nao entrega as matrizes ao pipeline fixo (desenha por shader),
    // entao o mod diz onde esta a de vista e com que lente desenhar. Fica no
    // mod, e nao fixo aqui, porque o endereco e conhecimento que o proprio mod
    // descobriu e pode ajustar sem recompilar o loader.
    JSValue Camera(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1 || !JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx, "speed.draw.camera({ view, fov })");

        double vista = Num(ctx, argv[0], "view", 0);
        double fov   = Num(ctx, argv[0], "fov", 60);
        double perto = Num(ctx, argv[0], "near", 0.1);
        double longe = Num(ctx, argv[0], "far", 3000);

        JSValue d = JS_GetPropertyStr(ctx, argv[0], "depth");
        bool profundidade = JS_ToBool(ctx, d) != 0;
        JS_FreeValue(ctx, d);

        JSValue y = JS_GetPropertyStr(ctx, argv[0], "flipY");
        bool espelhaY = JS_ToBool(ctx, y) != 0;
        JS_FreeValue(ctx, y);

        JSValue a = JS_GetPropertyStr(ctx, argv[0], "auto");
        bool automatico = JS_IsUndefined(a) ? true : (JS_ToBool(ctx, a) != 0);
        JS_FreeValue(ctx, a);

        WorldDraw::Camera((uintptr_t)vista, (float)fov, (float)perto,
                          (float)longe, profundidade, espelhaY, automatico);
        return JS_UNDEFINED;
    }

    // speed.draw.texture(endereco) -> { width, height, format } ou null
    JSValue Texture(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int64_t endereco = 0;
        if (argc >= 1 && JS_ToInt64(ctx, &endereco, argv[0])) return JS_EXCEPTION;

        unsigned largura = 0, altura = 0;
        int formato = 0;
        if (!WorldDraw::UsarTextura((uintptr_t)endereco, &largura, &altura, &formato))
            return JS_NULL;

        if (!endereco) return JS_UNDEFINED;

        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, (int)largura));
        JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, (int)altura));
        JS_SetPropertyStr(ctx, o, "format", JS_NewInt32(ctx, formato));
        return o;
    }

    // speed.draw.atlas(colunas, linhas)
    JSValue Atlas(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int32_t colunas = 1, linhas = 1;
        if (argc >= 1) JS_ToInt32(ctx, &colunas, argv[0]);
        if (argc >= 2) JS_ToInt32(ctx, &linhas, argv[1]);
        WorldDraw::Atlas(colunas, linhas);
        return JS_UNDEFINED;
    }

    // speed.draw.anchor(x, y, z, [9 floats]) — o referencial do carro
    JSValue Anchor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 4)
        {
            WorldDraw::Ancora(0, 0);
            return JS_UNDEFINED;
        }

        double x = 0, y = 0, z = 0;
        if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
            JS_ToFloat64(ctx, &z, argv[2]))
            return JS_EXCEPTION;

        float pos[3] = { (float)x, (float)y, (float)z };
        float rot[9];
        for (int i = 0; i < 9; i++)
        {
            JSValue v = JS_GetPropertyUint32(ctx, argv[3], i);
            double d = (i % 4 == 0) ? 1.0 : 0.0;
            JS_ToFloat64(ctx, &d, v);
            JS_FreeValue(ctx, v);
            rot[i] = (float)d;
        }

        WorldDraw::Ancora(pos, rot);
        return JS_UNDEFINED;
    }

    // speed.draw.textureFrom(endereco) — o campo que guarda o ponteiro
    JSValue TextureFrom(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int64_t endereco = 0;
        if (argc >= 1 && JS_ToInt64(ctx, &endereco, argv[0])) return JS_EXCEPTION;
        WorldDraw::FonteDeTextura((uintptr_t)endereco);
        return JS_UNDEFINED;
    }

    JSValue TextureOk(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return JS_NewBool(ctx, WorldDraw::TexturaValida()); }

    JSValue Clear(JSContext*, JSValueConst, int, JSValueConst*)
    { WorldDraw::Clear(); return JS_UNDEFINED; }

    JSValue Count(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return JS_NewInt32(ctx, WorldDraw::Count()); }
}

namespace Js
{
    void RegisterDraw(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue draw = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, draw, "spark", JS_NewCFunction(ctx, Spark, "spark", 4));
        JS_SetPropertyStr(ctx, draw, "camera", JS_NewCFunction(ctx, Camera, "camera", 1));
        JS_SetPropertyStr(ctx, draw, "texture", JS_NewCFunction(ctx, Texture, "texture", 1));
        JS_SetPropertyStr(ctx, draw, "textureOk",
                          JS_NewCFunction(ctx, TextureOk, "textureOk", 0));
        JS_SetPropertyStr(ctx, draw, "textureFrom",
                          JS_NewCFunction(ctx, TextureFrom, "textureFrom", 1));
        JS_SetPropertyStr(ctx, draw, "anchor", JS_NewCFunction(ctx, Anchor, "anchor", 4));
        JS_SetPropertyStr(ctx, draw, "atlas", JS_NewCFunction(ctx, Atlas, "atlas", 2));
        JS_SetPropertyStr(ctx, draw, "clear", JS_NewCFunction(ctx, Clear, "clear", 0));
        JS_SetPropertyStr(ctx, draw, "count", JS_NewCFunction(ctx, Count, "count", 0));

        JS_SetPropertyStr(ctx, speed, "draw", draw);
    }
}
