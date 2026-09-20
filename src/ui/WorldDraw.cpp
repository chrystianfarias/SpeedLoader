#include "ui/WorldDraw.h"

#include "core/Hook.h"
#include "core/Log.h"

#include <string.h>
#include <windows.h>

#include <math.h>
#include <vector>

// Sem D3DX: o SDK dele foi descontinuado e nao existe nesta maquina. A conta
// aqui e pequena, e evitar a dependencia deixa o build simples.
namespace
{
    struct Vec3
    {
        float x, y, z;
        Vec3() : x(0), y(0), z(0) {}
        Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
        Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
        Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
        Vec3 operator*(float k) const { return Vec3(x * k, y * k, z * k); }
    };
}

namespace
{
    struct Particula
    {
        Vec3 pos;          // no mundo, ou no carro quando presa
        Vec3 vel;          // no mesmo referencial da posicao
        bool presa;
        float tamanho;
        float vida;       // remaining, in seconds
        float vidaTotal;
        DWORD cor;        // no nascimento
        DWORD corFim;     // na morte; a cor caminha de uma para a outra
    };

    // Interpola duas cores canal a canal. E o que permite a chama sair clara e
    // terminar azulada, como o estouro do proprio jogo — gas queimando quente
    // comeca branco e esfria para azul, e uma cor fixa nunca imita isso.
    DWORD Misturar(DWORD a, DWORD b, float k)
    {
        if (k < 0) k = 0;
        if (k > 1) k = 1;

        DWORD saida = 0;
        for (int i = 0; i < 3; i++)
        {
            int desloca = i * 8;
            float ca = (float)((a >> desloca) & 0xFF);
            float cb = (float)((b >> desloca) & 0xFF);
            saida |= ((DWORD)(ca + (cb - ca) * k) & 0xFF) << desloca;
        }
        return saida;
    }

    struct Vertice
    {
        float x, y, z;
        DWORD cor;
        float u, v;
    };
    const DWORD FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    std::vector<Particula> g_particulas;
    IDirect3DTexture9* g_textura = 0;

    // A textura do proprio jogo, quando o mod aponta uma. O borrao procedural
    // continua existindo como reserva: se o ponteiro nao valer mais (troca de
    // dispositivo, recurso descarregado), volta-se a ele em vez de sumir com a
    // chama.
    IDirect3DTexture9* g_texturaJogo = 0;

    // ONDE o ponteiro mora, nao o ponteiro.
    //
    // Guardar o ponteiro funciona ate sair do evento: o jogo descarrega os
    // recursos e recarrega depois, e o endereco antigo passa a apontar para
    // memoria liberada — dai a textura "bugada" na volta. Guardando o campo que
    // contem o ponteiro (info+0x18), basta rele-lo: quando o jogo troca a
    // textura, a nossa troca junto, e quando ele descarrega, a validacao falha
    // e cai-se no borrao proprio em vez de desenhar lixo.
    uintptr_t g_fonteTextura = 0;
    uintptr_t g_ultimoPonteiro = 0;

    // A textura do jogo e uma folha de sprites 2x2: quatro quadros de uma
    // animacao, nao uma imagem so. Desenhar ela inteira mostra os quatro de uma
    // vez; o certo e percorrer os quadros ao longo da vida da particula, que e
    // como uma baforada de fogo se comporta — nasce, cresce, se desfaz.
    int g_colunas = 1;
    int g_linhas = 1;

    // O referencial do carro, atualizado a cada quadro pelo mod.
    //
    // Uma particula solta no mundo fica para tras quando o carro anda — a 100
    // km/h, um quarto de segundo de vida vira sete metros de rastro. Chama de
    // escape nao se comporta assim: ela acompanha o carro. Entao a particula
    // presa guarda a posicao EM COORDENADAS DO CARRO e so vira mundo na hora de
    // desenhar, com o referencial daquele quadro.
    float g_ancoraPos[3] = { 0, 0, 0 };
    float g_ancoraRot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    bool  g_temAncora = false;
    bool g_avisouFalha = false;

    // A camera, dita de fora. Ver WorldDraw::Camera.
    uintptr_t g_enderecoVista = 0;
    float g_fovY  = 60.0f;
    float g_perto = 0.1f;
    float g_longe = 3000.0f;
    bool  g_profundidade = false;
    bool  g_espelhaY = false;
    bool  g_automatico = true;

    // A matriz que o JOGO usa, pescada das constantes do vertex shader.
    //
    // Reconstruir a lente na mao nao fecha: nenhum campo de visao acertava,
    // sinal de que a projecao do jogo nao e uma perspectiva simetrica comum
    // (jogo de corrida costuma deslocar o centro para levantar o horizonte).
    // Adivinhar isso e impossivel — mas nao e preciso: desenhando por shader, o
    // jogo sobe a matriz combinada de vista+projecao como constantes, e ela
    // pode ser lida do dispositivo e reaproveitada inteira. Sem campo de visao,
    // sem espelho, sem plano de corte: a mesma conta que desenha o carro
    // desenha a chama.
    // Ler as constantes no EndScene nao acha nada, e o log provou: 64 lidas, 0
    // candidatas. Faz sentido — quem escreve por ultimo no quadro e o HUD 2D,
    // que sobrescreve a matriz da cena antes de chegarmos la. Entao nao se le
    // no fim: escuta-se o envio, interceptando SetVertexShaderConstantF, e
    // guarda-se a matriz no instante em que o jogo a usa para desenhar o mundo.
    float g_matrizJogo[16];
    bool  g_achou = false;
    int   g_registro = -1;
    bool  g_transposta = false;
    bool  g_escutando = false;

    typedef HRESULT (STDMETHODCALLTYPE* tSetVSConst)(IDirect3DDevice9*, UINT,
                                                     const float*, UINT);
    tSetVSConst g_origSetVSConst = 0;

    // As matrizes que o jogo entrega ao pipeline fixo.
    //
    // A premissa de que o NFSU2 desenhava tudo por vertex shader estava errada,
    // e o log provou de dois jeitos: nenhuma constante de shader e enviada, e
    // as matrizes lidas no EndScene voltam como identidade. Elas voltam assim
    // porque o HUD 2D as zera no fim do quadro — ler ali e tarde. Entao se
    // escuta o envio, e se guarda a projecao de PERSPECTIVA (a do mundo, que
    // tem o canto _44 zerado; a do HUD e ortografica e tem 1 ali) junto da
    // vista que estava valendo com ela.
    typedef HRESULT (STDMETHODCALLTYPE* tSetTransform)(IDirect3DDevice9*,
                                                       D3DTRANSFORMSTATETYPE,
                                                       const D3DMATRIX*);
    tSetTransform g_origSetTransform = 0;

    D3DMATRIX g_vistaJogo;
    D3DMATRIX g_projJogo;
    bool g_temVistaJogo = false;
    bool g_temProjJogo = false;

    float g_referencia[3];
    bool  g_temReferencia = false;
    float g_melhorErro = 1e9f;

    // Telemetria da escuta. Sem ela nao da para distinguir "o jogo nao envia
    // matriz nenhuma" de "envia, mas nenhuma passou no teste" — e o log ficou
    // mudo justamente porque as reprovadas por forma degenerada devolvem sempre
    // o mesmo valor e nunca batiam o recorde anterior.
    int g_envios = 0;
    int g_enviosGrandes = 0;
    int g_regMin = 9999, g_regMax = -1, g_maiorQuantos = 0;

    bool LerMatriz(uintptr_t endereco, D3DMATRIX* saida)
    {
        if (!endereco || IsBadReadPtr((const void*)endereco, sizeof(D3DMATRIX)))
            return false;
        memcpy(saida, (const void*)endereco, sizeof(D3DMATRIX));

        // Uma matriz de vista tem a ultima coluna (0,0,0,1) e a parte de
        // rotacao ortonormal. Conferir a primeira linha ja basta para nao
        // entregar lixo ao pipeline caso o endereco mude entre sessoes: uma
        // matriz errada nao da erro nenhum, so some com o desenho.
        float n = saida->_11 * saida->_11 + saida->_12 * saida->_12 +
                  saida->_13 * saida->_13;
        return n > 0.9f && n < 1.1f;
    }

    // Multiplica um ponto (x,y,z,1) pela matriz, na convencao escolhida.
    void Projetar(const float* m, bool transposta, const float* p, float* saida)
    {
        for (int i = 0; i < 4; i++)
        {
            const float* col = transposta ? &m[i * 4] : 0;
            saida[i] = transposta
                ? (col[0] * p[0] + col[1] * p[1] + col[2] * p[2] + col[3])
                : (m[0 * 4 + i] * p[0] + m[1 * 4 + i] * p[1] +
                   m[2 * 4 + i] * p[2] + m[3 * 4 + i]);
        }
    }

    // Inverte a matriz de vista. Barato porque ela nao e uma matriz qualquer:
    // a parte de rotacao e ortonormal, e para essas a inversa e a transposta.
    void InverterVista(const D3DMATRIX& v, float* saida)
    {
        const float* m = &v._11;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                saida[i * 4 + j] = m[j * 4 + i];

        for (int j = 0; j < 3; j++)
            saida[12 + j] = -(m[12] * saida[j] + m[13] * saida[4 + j] +
                              m[14] * saida[8 + j]);

        saida[3] = saida[7] = saida[11] = 0.0f;
        saida[15] = 1.0f;
    }

    void Multiplicar(const float* a, const float* b, float* saida)
    {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                float soma = 0;
                for (int k = 0; k < 4; k++) soma += a[i * 4 + k] * b[k * 4 + j];
                saida[i * 4 + j] = soma;
            }
    }

    // Quanto uma matriz se afasta da forma de uma projecao.
    //
    // Este e o teste que substituiu o anterior. O de antes exigia que a chama
    // caisse dentro da tela, e reprovava qualquer matriz que o jogo enviasse ja
    // com a posicao da peca embutida — que e o caso normal. Este nao depende de
    // ponto nenhum: sabendo a matriz de VISTA (achada na memoria e conferida
    // pelo /altura), divide-se uma pela outra e ve-se o que sobra. Se o que
    // sobra for uma projecao de verdade, ela tem forma inconfundivel: quase
    // tudo zero fora da diagonal, o 1 que faz a divisao por w, e o canto
    // zerado. Inclusive se a lente for descentrada, que e justamente o que
    // nenhum campo de visao conseguia imitar.
    float ErroDeProjecao(const float* p)
    {
        const int zeros[] = { 1, 2, 3, 4, 6, 7, 12, 13, 15 };
        float erro = 0;
        for (int i = 0; i < 9; i++) erro += fabsf(p[zeros[i]]);
        erro += fabsf(fabsf(p[11]) - 1.0f);

        // E precisa ser uma lente plausivel, nao uma matriz degenerada que
        // passa no teste por ser toda zero.
        if (fabsf(p[0]) < 0.2f || fabsf(p[0]) > 10.0f) return 1e9f;
        if (fabsf(p[5]) < 0.2f || fabsf(p[5]) > 10.0f) return 1e9f;
        if (fabsf(p[14]) < 1e-4f) return 1e9f;
        return erro;
    }

    bool DentroDaTela(const float* c)
    {
        if (!(c[3] > 0.05f) || !(c[3] < 1e6f)) return false;
        float x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
        return x > -1.3f && x < 1.3f && y > -1.3f && y < 1.3f &&
               z > 0.0f && z < 1.0f;
    }

    // Cada envio de 4 ou mais vetores e uma matriz candidata. O teste e o
    // mesmo: com a matriz certa, a chama cai dentro da tela. Uma matriz de
    // objeto (que ja carrega a posicao da peca) reprova, porque o nosso ponto
    // de referencia esta em coordenadas do mundo.
    HRESULT STDMETHODCALLTYPE SetVSConstHook(IDirect3DDevice9* device, UINT reg,
                                             const float* dados, UINT quantos)
    {
        HRESULT hr = g_origSetVSConst(device, reg, dados, quantos);

        g_envios++;
        if (quantos >= 4)
        {
            g_enviosGrandes++;
            if ((int)reg < g_regMin) g_regMin = (int)reg;
            if ((int)reg > g_regMax) g_regMax = (int)reg;
            if ((int)quantos > g_maiorQuantos) g_maiorQuantos = (int)quantos;
        }

        if (g_envios == 3000 && !g_achou)
            LogGfx("worlddraw: %d envios ao shader, %d com 4+ vetores, "
                   "registros c%d..c%d, maior envio %d vetores, melhor erro %.4f",
                   g_envios, g_enviosGrandes, g_regMin, g_regMax, g_maiorQuantos,
                   g_melhorErro >= 1e8f ? -1.0f : g_melhorErro);

        if (!dados || quantos < 4) return hr;

        D3DMATRIX vista;
        if (!LerMatriz(g_enderecoVista, &vista)) return hr;

        float inversa[16];
        InverterVista(vista, inversa);

        for (UINT i = 0; i + 4 <= quantos; i++)
        {
            for (int t = 0; t < 2; t++)
            {
                float m[16];
                if (t)
                    for (int l = 0; l < 4; l++)
                        for (int col = 0; col < 4; col++)
                            m[l * 4 + col] = dados[i * 4 + col * 4 + l];
                else
                    memcpy(m, &dados[i * 4], sizeof(m));

                float projecao[16];
                Multiplicar(inversa, m, projecao);

                // O limiar foi 0.02 e recusava a matriz certa por um fio: o
                // log registrou c0 com erro 0.0200, teimosamente estavel. A
                // sobra nao e ruido — vem de a matriz de vista lida da memoria
                // ser de um instante levemente diferente da que o jogo usou,
                // que e justamente o atraso que estamos tentando eliminar.
                // Exigir forma perfeita usando uma referencia imperfeita e
                // contraditorio; 0.06 aceita a matriz e continua longe das
                // reprovadas, que erram por unidades inteiras.
                float erro = ErroDeProjecao(projecao);
                if (erro > 0.06f)
                {
                    if (erro < g_melhorErro && erro < 1e8f)
                    {
                        g_melhorErro = erro;
                        LogGfx("worlddraw: perto em c%d%s, erro %.4f "
                               "[%.3f %.3f | %.3f %.3f | %.3f %.3f]",
                               (int)(reg + i), t ? " transposta" : "", erro,
                               projecao[0], projecao[5], projecao[8], projecao[9],
                               projecao[10], projecao[14]);
                    }
                    continue;
                }

                memcpy(g_matrizJogo, m, sizeof(m));
                g_transposta = false;      // ja desfeita acima

                if (!g_achou)
                {
                    g_achou = true;
                    g_registro = (int)(reg + i);
                    float fovY = 2.0f * atanf(1.0f / projecao[5]) * 57.2957795f;
                    LogGfx("worlddraw: matriz do jogo em c%d%s — fovY %.1f, "
                           "centro (%.3f, %.3f), erro %.4f",
                           g_registro, t ? " transposta" : "", fovY,
                           projecao[8], projecao[9], erro);
                }
                return hr;
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetTransformHook(IDirect3DDevice9* device,
                                               D3DTRANSFORMSTATETYPE estado,
                                               const D3DMATRIX* m)
    {
        // Os primeiros envios, crus: e o que diz de uma vez por todas o que o
        // jogo usa e o que ele nao usa.
        static int vistos = 0;
        if (m && vistos < 12)
        {
            vistos++;
            LogGfx("worlddraw: SetTransform estado %d [%.3f %.3f | %.3f %.3f | "
                   "%.3f %.3f | %.1f %.1f %.1f %.3f]",
                   (int)estado, m->_11, m->_22, m->_31, m->_32, m->_33, m->_34,
                   m->_41, m->_42, m->_43, m->_44);
        }

        if (m)
        {
            if (estado == D3DTS_VIEW)
            {
                g_vistaJogo = *m;
                g_temVistaJogo = true;
            }
            else if (estado == D3DTS_PROJECTION && m->_44 == 0.0f && m->_34 != 0.0f)
            {
                if (!g_temProjJogo)
                {
                    float fovY = m->_22 != 0 ? 2.0f * atanf(1.0f / m->_22) * 57.2957795f : 0;
                    LogGfx("worlddraw: projecao do jogo — fovY %.1f, aspecto %.2f, "
                           "centro (%.3f, %.3f), z %.4f/%.2f",
                           fovY, m->_22 / (m->_11 ? m->_11 : 1),
                           m->_31, m->_32, m->_33, m->_43);
                }
                g_projJogo = *m;
                g_temProjJogo = true;
            }
        }
        return g_origSetTransform(device, estado, m);
    }

    // O grampo tem de ser no CODIGO, dentro da d3d9.dll, e nao na vtable.
    //
    // A vtable do dispositivo e disputada: outro ASI (WidescreenFix ou
    // HDReflections) a restaura alguns segundos depois de nos — o D3D9Hook ja
    // documenta isso para o EndScene. Um grampo de vtable aqui morre antes de o
    // jogo chegar na pista, e o log fica mudo: nao por o jogo deixar de enviar
    // as matrizes, mas por nao haver mais grampo quando elas passam. O corpo da
    // funcao ninguem restaura.
    bool DetourNoCodigo(void* alvo, void* gancho, Trampoline* tramp,
                        void** original, const char* nome)
    {
        unsigned char* p = (unsigned char*)alvo;
        if (!alvo || IsBadReadPtr(p, 16)) return false;

        size_t len = CopyLength(p, 5);
        if (len == 0 || len > 16)
        {
            LogGfx("worlddraw: %s em 0x%p tem prologo que nao sei copiar"
                   " (%02X %02X %02X %02X %02X)",
                   nome, alvo, p[0], p[1], p[2], p[3], p[4]);
            return false;
        }

        *original = tramp->Install((uintptr_t)alvo, gancho, len);
        return *original != 0;
    }

    void Escutar(IDirect3DDevice9* device)
    {
        if (g_escutando) return;
        g_escutando = true;

        void** vt = *(void***)device;
        const int VT_SET_VS_CONST  = 94;  // IDirect3DDevice9::SetVertexShaderConstantF
        const int VT_SET_TRANSFORM = 44;  // IDirect3DDevice9::SetTransform

        static Trampoline tramp1, tramp2;
        bool a = DetourNoCodigo(vt[VT_SET_TRANSFORM], (void*)SetTransformHook,
                                &tramp1, (void**)&g_origSetTransform,
                                "SetTransform");
        bool b = DetourNoCodigo(vt[VT_SET_VS_CONST], (void*)SetVSConstHook,
                                &tramp2, (void**)&g_origSetVSConst,
                                "SetVertexShaderConstantF");

        LogGfx("worlddraw: escutando no codigo — SetTransform %s, constantes %s",
               a ? "ok" : "falhou", b ? "ok" : "falhou");
    }


    // A soft radial blob, built here rather than shipped as a file: it is a few
    // lines of code, it always matches the blend mode, and it saves the mod
    // from carrying an asset just to draw a dot of light.
    IDirect3DTexture9* CriarTextura(IDirect3DDevice9* device)
    {
        const UINT N = 64;
        IDirect3DTexture9* tex = 0;
        if (FAILED(device->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &tex, 0)))
            return 0;

        IDirect3DTexture9* sistema = 0;
        if (FAILED(device->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_SYSTEMMEM, &sistema, 0)))
        { tex->Release(); return 0; }

        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sistema->LockRect(0, &lr, 0, 0)))
        {
            for (UINT y = 0; y < N; y++)
            {
                DWORD* linha = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (UINT x = 0; x < N; x++)
                {
                    float dx = (x + 0.5f) / N * 2.0f - 1.0f;
                    float dy = (y + 0.5f) / N * 2.0f - 1.0f;
                    float d = sqrtf(dx * dx + dy * dy);

                    // Bright core, soft edge: squaring the falloff keeps the
                    // middle hot instead of washing the whole quad out.
                    float a = d >= 1.0f ? 0.0f : (1.0f - d);
                    a = a * a;

                    BYTE v = (BYTE)(a * 255.0f);
                    linha[x] = (v << 24) | 0x00FFFFFF;
                }
            }
            sistema->UnlockRect(0);
            device->UpdateTexture(sistema, tex);
        }

        sistema->Release();
        return tex;
    }
}

// Onde a d3d9.dll mora. Tudo o que se chama tem de estar la dentro.
static bool FaixaD3D9(uintptr_t* inicio, uintptr_t* fim)
{
    HMODULE m = GetModuleHandleA("d3d9.dll");
    if (!m) return false;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);

    *inicio = (uintptr_t)m;
    *fim = *inicio + nt->OptionalHeader.SizeOfImage;
    return true;
}

// Valida um ponteiro qualquer como textura, sem confiar em nada.
//
// O endereco vem de varredura de memoria, entao pode ser qualquer coisa — e
// aqui esta a licao que custou um travamento: NAO BASTA o __try. Ele protege
// leitura invalida, mas nao o caso pior, que e a vtable ser lixo LEGIVEL: o
// `call` pula para um endereco arbitrario e o processo executa o que nao devia,
// sem excecao nenhuma para capturar.
//
// Por isso a validacao e em duas etapas, e a primeira nao executa nada: a
// vtable de uma textura de verdade mora dentro da d3d9.dll, e o primeiro metodo
// dela tambem. Só depois disso e que se pergunta ao D3D o tipo e a descricao.
static bool ChecarTextura(void* p, unsigned* larg, unsigned* alt, int* fmt)
{
    uintptr_t ini = 0, fim = 0;
    if (!FaixaD3D9(&ini, &fim)) return false;

    if (IsBadReadPtr(p, sizeof(void*))) return false;
    uintptr_t vtable = *(uintptr_t*)p;
    if (vtable < ini || vtable >= fim) return false;
    if (IsBadReadPtr((void*)vtable, sizeof(void*) * 12)) return false;

    // Os metodos apontados pela vtable tambem tem de estar na dll: uma vtable
    // plausivel mas apontando para fora e exatamente o que derruba o jogo.
    for (int i = 0; i < 12; i++)
    {
        uintptr_t metodo = ((uintptr_t*)vtable)[i];
        if (metodo < ini || metodo >= fim) return false;
    }

    __try
    {
        IDirect3DTexture9* t = (IDirect3DTexture9*)p;
        if (t->GetType() != D3DRTYPE_TEXTURE) return false;

        D3DSURFACE_DESC d;
        if (FAILED(t->GetLevelDesc(0, &d))) return false;
        if (!d.Width || !d.Height || d.Width > 8192 || d.Height > 8192) return false;

        *larg = d.Width; *alt = d.Height; *fmt = (int)d.Format;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

namespace WorldDraw
{
    void Init() {}

    bool TexturaValida() { return g_texturaJogo != 0; }

    void FonteDeTextura(uintptr_t endereco)
    {
        g_fonteTextura = endereco;
        g_ultimoPonteiro = 0;
    }

    bool UsarTextura(uintptr_t endereco, unsigned* largura, unsigned* altura,
                     int* formato)
    {
        if (!endereco)
        {
            g_texturaJogo = 0;
            g_fonteTextura = 0;
            return true;
        }

        if (IsBadReadPtr((const void*)endereco, sizeof(void*))) return false;
        if (!ChecarTextura((void*)endereco, largura, altura, formato)) return false;

        g_texturaJogo = (IDirect3DTexture9*)endereco;
        LogGfx("worlddraw: usando a textura do jogo em 0x%p (%ux%u, formato %d)",
               (void*)endereco, *largura, *altura, *formato);
        return true;
    }

    void OnDeviceLost()
    {
        if (g_textura) { g_textura->Release(); g_textura = 0; }

        // A do jogo nao e nossa para liberar, mas tambem nao sobrevive ao
        // reset: solta-se a referencia e deixa-se a fonte reler na volta.
        g_texturaJogo = 0;
        g_ultimoPonteiro = 0;
        g_particulas.clear();
    }

    void OnDeviceReset() { /* the texture is rebuilt on the next frame */ }

    void Ancora(const float* posicao, const float* rotacao)
    {
        if (!posicao || !rotacao) { g_temAncora = false; return; }
        for (int i = 0; i < 3; i++) g_ancoraPos[i] = posicao[i];
        for (int i = 0; i < 9; i++) g_ancoraRot[i] = rotacao[i];
        g_temAncora = true;
    }

    void Atlas(int colunas, int linhas)
    {
        g_colunas = (colunas > 0 && colunas <= 16) ? colunas : 1;
        g_linhas  = (linhas  > 0 && linhas  <= 16) ? linhas  : 1;
    }

    void Camera(uintptr_t enderecoVista, float fovYGraus, float perto,
                float longe, bool profundidade, bool espelhaY, bool automatico)
    {
        g_espelhaY = espelhaY;
        // Desligar e religar o modo automatico refaz a busca do zero: e o que
        // torna /vista auto util como teste, e nao so como chave.
        if (automatico != g_automatico) { g_achou = false; g_registro = -1; }
        g_automatico = automatico;
        g_enderecoVista = enderecoVista;
        if (fovYGraus > 1.0f && fovYGraus < 179.0f) g_fovY = fovYGraus;
        if (perto > 0.0f) g_perto = perto;
        if (longe > perto) g_longe = longe;
        g_profundidade = profundidade;
    }

    void Spawn(float x, float y, float z, float tamanho, float vida, DWORD cor,
               bool presa, float vx, float vy, float vz, DWORD corFim)
    {
        // A cap, not a queue: a runaway mod spawning every frame should cost a
        // constant amount of work, not grow until the frame time collapses.
        if (g_particulas.size() >= 256) return;

        // Serve de ponto de referencia para identificar a matriz do jogo: uma
        // particula esta, por definicao, onde a chama deveria aparecer.
        if (!presa)
        {
            g_referencia[0] = x; g_referencia[1] = y; g_referencia[2] = z;
            g_temReferencia = true;
        }

        Particula p;
        p.presa = presa;
        p.pos = Vec3(x, y, z);
        p.vel = Vec3(vx, vy, vz);
        p.corFim = corFim;
        p.tamanho = tamanho;
        p.vida = p.vidaTotal = vida;
        p.cor = cor;
        g_particulas.push_back(p);
    }

    void Clear() { g_particulas.clear(); }
    int  Count() { return (int)g_particulas.size(); }

    // Reconfere a textura do jogo a cada quadro, relendo a fonte. Barata: uma
    // leitura, e a validacao completa so quando o ponteiro muda.
    void AtualizarTextura()
    {
        if (!g_fonteTextura) return;
        if (IsBadReadPtr((const void*)g_fonteTextura, sizeof(void*))) return;

        uintptr_t agora = *(uintptr_t*)g_fonteTextura;
        if (agora == g_ultimoPonteiro) return;
        g_ultimoPonteiro = agora;

        unsigned largura = 0, altura = 0;
        int formato = 0;
        if (agora && !IsBadReadPtr((const void*)agora, sizeof(void*)) &&
            ChecarTextura((void*)agora, &largura, &altura, &formato))
        {
            g_texturaJogo = (IDirect3DTexture9*)agora;
            LogGfx("worlddraw: textura do jogo recarregada (0x%p, %ux%u)",
                   (void*)agora, largura, altura);
        }
        else
        {
            // Some em vez de desenhar lixo: o borrao proprio assume.
            g_texturaJogo = 0;
            LogGfx("worlddraw: textura do jogo saiu de cena, usando o borrao");
        }
    }

    void Render(IDirect3DDevice9* device, float dt)
    {
        if (!device) return;
        AtualizarTextura();
        if (g_automatico) Escutar(device);
        if (g_particulas.empty()) return;

        if (!g_textura)
        {
            g_textura = CriarTextura(device);
            if (!g_textura)
            {
                if (!g_avisouFalha)
                {
                    g_avisouFalha = true;
                    LogGfx("worlddraw: could not create the particle texture");
                }
                return;
            }
        }

        // A camera. Perguntar ao dispositivo nao adianta: o NFSU2 desenha tudo
        // com vertex shader e nunca define as matrizes do pipeline fixo, entao
        // elas voltam como identidade — os quads iam parar fora da tela, sem
        // erro nenhum, que era exatamente o sintoma.
        //
        // A de VISTA vem da memoria do jogo (0x8734A0, achada pelo comando
        // /camera: e a unica cuja rotacao e ortonormal e que poe o carro alguns
        // metros a frente da camera). O layout e um D3DMATRIX padrao, entao ela
        // e copiada crua.
        //
        // A de PROJECAO nao existe em lugar nenhum: sendo shader, o jogo guarda
        // as constantes ja combinadas. Ela e montada aqui — e uma perspectiva
        // comum, e os unicos dados que faltam sao o campo de visao (ajustavel
        // pelo mod) e a proporcao da tela, que sai do proprio viewport.
        D3DMATRIX view;
        if (!LerMatriz(g_enderecoVista, &view))
            device->GetTransform(D3DTS_VIEW, &view);

        // Diagnostico unico: se o jogo desenha com vertex shader, ele nunca
        // define as matrizes do pipeline fixo e estas voltam como identidade —
        // caso em que os quads iriam para fora da tela e nada apareceria, sem
        // nenhum erro. E a primeira coisa a descartar.
        static bool avisou = false;
        if (!avisou)
        {
            avisou = true;
            D3DMATRIX proj;
            device->GetTransform(D3DTS_PROJECTION, &proj);
            LogGfx("worlddraw: view [%.2f %.2f %.2f | %.2f %.2f %.2f | %.1f %.1f %.1f]",
                   view._11, view._12, view._13,
                   view._21, view._22, view._23,
                   view._41, view._42, view._43);
            LogGfx("worlddraw: proj [%.2f %.2f %.2f %.2f]",
                   proj._11, proj._22, proj._33, proj._43);

            IDirect3DVertexShader9* vs = 0;
            device->GetVertexShader(&vs);
            LogGfx("worlddraw: vertex shader ativo = %s", vs ? "sim" : "nao");
            if (vs) vs->Release();
        }

        // Os eixos da camera saem das COLUNAS da matriz de vista, sem inverter
        // nada: a parte rotacional dela e ortonormal, e para uma matriz assim a
        // inversa e a transposta. Isso e o que permite dispensar o D3DX.
        Vec3 direita(view._11, view._21, view._31);
        Vec3 cima(view._12, view._22, view._32);

        std::vector<Vertice> vertices;
        vertices.reserve(g_particulas.size() * 6);

        for (size_t i = 0; i < g_particulas.size(); )
        {
            Particula& p = g_particulas[i];
            p.vida -= dt;
            if (p.vida <= 0.0f)
            {
                p = g_particulas.back();
                g_particulas.pop_back();
                continue;
            }

            // Anda. A velocidade esta no mesmo referencial da posicao, entao
            // numa particula presa ela e velocidade EM RELACAO AO CARRO — que
            // e o que faz o jato sair do cano em vez de sair do mundo.
            p.pos = p.pos + p.vel * dt;

            // A presa so vira mundo agora, com o referencial deste quadro —
            // e o que a mantem grudada no carro em vez de ficar para tras.
            Vec3 posicao = p.pos;
            if (p.presa && g_temAncora)
            {
                const float* m = g_ancoraRot;
                posicao = Vec3(
                    g_ancoraPos[0] + p.pos.x * m[0] + p.pos.y * m[3] + p.pos.z * m[6],
                    g_ancoraPos[1] + p.pos.x * m[1] + p.pos.y * m[4] + p.pos.z * m[7],
                    g_ancoraPos[2] + p.pos.x * m[2] + p.pos.y * m[5] + p.pos.z * m[8]);
            }

            float t = p.vida / p.vidaTotal;          // 1 -> 0 over its life

            // O quadro da animacao, na ordem da folha: esquerda para direita,
            // de cima para baixo. A sequencia toca uma vez ao longo da vida.
            float u0 = 0.0f, v0 = 0.0f, du = 1.0f, dv = 1.0f;
            const int quadros = g_colunas * g_linhas;
            if (quadros > 1)
            {
                int quadro = (int)((1.0f - t) * quadros);
                if (quadro < 0) quadro = 0;
                if (quadro >= quadros) quadro = quadros - 1;

                du = 1.0f / g_colunas;
                dv = 1.0f / g_linhas;
                u0 = (quadro % g_colunas) * du;
                v0 = (quadro / g_colunas) * dv;
            }
            BYTE alfa = (BYTE)(t * 255.0f);

            // Grows a little as it fades, like a puff of burning gas.
            float s = p.tamanho * (1.0f + (1.0f - t) * 0.8f);
            DWORD cor = (alfa << 24) |
                        (Misturar(p.cor, p.corFim, 1.0f - t) & 0x00FFFFFF);

            Vec3 r = direita * s;
            Vec3 u = cima * s;

            Vec3 a = posicao - r - u;
            Vec3 b = posicao + r - u;
            Vec3 c = posicao + r + u;
            Vec3 d = posicao - r + u;

            const float u1 = u0 + du, v1 = v0 + dv;
            Vertice quad[6] = {
                { a.x, a.y, a.z, cor, u0, v1 },
                { d.x, d.y, d.z, cor, u0, v0 },
                { b.x, b.y, b.z, cor, u1, v1 },
                { b.x, b.y, b.z, cor, u1, v1 },
                { d.x, d.y, d.z, cor, u0, v0 },
                { c.x, c.y, c.z, cor, u1, v0 },
            };
            for (int k = 0; k < 6; k++) vertices.push_back(quad[k]);
            i++;
        }

        if (vertices.empty()) return;

        IDirect3DStateBlock9* estado = 0;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &estado))) return;
        estado->Capture();

        D3DMATRIX identidade = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        };
        device->SetTransform(D3DTS_WORLD, &identidade);

        // Primeiro a tentativa boa: a matriz do proprio jogo. So procura uma
        // vez, e so quando ha particula para desenhar — o ponto de referencia e
        // a propria particula, que por definicao esta onde a chama deveria
        // aparecer.
        // A melhor fonte, quando existe: as matrizes que o jogo entregou ao
        // desenhar o mundo neste quadro. Nada de campo de visao montado nem
        // espelho — e a mesma conta que desenhou o carro.
        if (g_automatico && g_temProjJogo && g_temVistaJogo)
        {
            device->SetTransform(D3DTS_VIEW, &g_vistaJogo);
            device->SetTransform(D3DTS_PROJECTION, &g_projJogo);
        }
        else if (g_automatico && g_achou)
        {
            D3DMATRIX vp;
            float* d = &vp._11;

            // O pipeline fixo multiplica com o ponto a ESQUERDA (p * M), e o
            // shader recebe as constantes para multiplicar a direita. Quando a
            // convencao do jogo for essa, a matriz entra transposta.
            if (g_transposta)
                for (int l = 0; l < 4; l++)
                    for (int col = 0; col < 4; col++)
                        d[l * 4 + col] = g_matrizJogo[col * 4 + l];
            else
                memcpy(d, g_matrizJogo, sizeof(g_matrizJogo));

            device->SetTransform(D3DTS_VIEW, &identidade);
            device->SetTransform(D3DTS_PROJECTION, &vp);
        }
        else if (g_enderecoVista)
        {
            device->SetTransform(D3DTS_VIEW, &view);

            D3DVIEWPORT9 vp;
            float aspecto = 4.0f / 3.0f;
            if (SUCCEEDED(device->GetViewport(&vp)) && vp.Height)
                aspecto = (float)vp.Width / (float)vp.Height;

            float h = 1.0f / tanf(g_fovY * 3.14159265f / 360.0f);
            float w = h / aspecto;
            float q = g_longe / (g_longe - g_perto);

            // O sinal do eixo vertical: se a matriz do jogo tiver o "para cima"
            // invertido em relacao ao que o pipeline fixo espera, tudo o que
            // desenhamos aparece espelhado em torno do eixo da camera — certo
            // na horizontal e na distancia, errado so na altura. Que e
            // exatamente o sintoma.
            if (g_espelhaY) h = -h;

            D3DMATRIX proj = {
                w, 0, 0, 0,
                0, h, 0, 0,
                0, 0, q, 1,
                0, 0, -q * g_perto, 0,
            };
            device->SetTransform(D3DTS_PROJECTION, &proj);
        }

        device->SetFVF(FVF);
        device->SetTexture(0, g_texturaJogo ? g_texturaJogo : g_textura);
        device->SetPixelShader(0);
        device->SetVertexShader(0);

        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);    // additive
        device->SetRenderState(D3DRS_ZENABLE, g_profundidade ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);

        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(vertices.size() / 3),
                                vertices.data(), sizeof(Vertice));

        estado->Apply();
        estado->Release();
    }
}
