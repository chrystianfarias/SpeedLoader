#pragma once

#include <d3d9.h>
#include <stdint.h>

// Sprites drawn in the world, inside the game's own 3D scene.
//
// Not the UI layer: Chromium composites on top of everything, flat, with no
// depth and no perspective. A flame at the exhaust has to sit where the car is,
// shrink with distance and hide behind the bodywork when the camera swings
// around - so it is drawn as geometry, in EndScene, with the view and
// projection the game is already using.
//
// The particles are additive and depth-tested but not depth-written, which is
// how flames and sparks are normally drawn: they light up what is behind them
// without occluding each other.
namespace WorldDraw
{
    void Init();

    // Aponta uma textura do proprio jogo para as particulas (0 volta ao borrao
    // procedural). O endereco e validado pelo D3D antes de ser aceito, porque
    // ele vem de varredura de memoria e pode ser qualquer coisa.
    bool UsarTextura(uintptr_t address, unsigned* width, unsigned* height,
                     int* format);

    // A textura e uma folha de sprites: quantos quadros em cada direcao. A
    // sequencia toca uma vez ao longo da vida de cada particula. 1x1 desliga.
    void Atlas(int columns, int rows);

    // O endereco do CAMPO que guarda o ponteiro da textura (info+0x18), para
    // que ela seja relida a cada quadro. Sobrevive a descarga e recarga de
    // recursos ao sair e voltar de um evento.
    void FonteDeTextura(uintptr_t address);

    // A textura do jogo ainda vale? Falsa depois de o jogo descarregar o
    // recurso — o sinal para o mod procurar de novo pelo nome.
    bool TexturaValida();

    // Onde esta a matriz de vista do jogo, e com que lente desenhar.
    //
    // CAMINHO PRINCIPAL (automatico): a matriz vista+projecao e pescada das
    // constantes que o jogo envia ao vertex shader, e reconhecida pela FORMA —
    // dividindo-a pela matriz de vista conhecida, o que sobra tem de parecer
    // uma projecao. Assim nao ha campo de visao a adivinhar: e a mesma conta
    // que desenha o carro, inclusive a lente descentrada do jogo (fovY ~69,
    // aspecto 1.62, Y ja invertido), que nenhum fov montado imitava.
    //
    // Dois detalhes custaram horas e ficam registrados:
    //
    //   - o grampo TEM de ser no codigo da d3d9.dll, nao na vtable: outro ASI
    //     (WidescreenFix/HDReflections) restaura a vtable do dispositivo, e um
    //     grampo ali morre em silencio antes do jogo chegar na pista;
    //   - ler as matrizes no EndScene nunca funciona: o HUD 2D poe vista e
    //     projecao em identidade antes disso.
    //
    // O caminho manual abaixo (endereco da vista + fov + espelho) ficou como
    // reserva. Ele erra pouco e de um jeito enganoso: acerta o ponto em que a
    // camera pivota e erra tudo em volta, que parece problema de posicao.
    //
    // O NFSU2 desenha por vertex shader e nunca alimenta o pipeline fixo, entao
    // as matrizes precisam vir daqui. A de vista e lida da memoria do jogo a
    // cada quadro (endereco achado pelo comando /camera do mod); a de projecao
    // e montada a partir do campo de visao, ja que o jogo nao guarda uma.
    //
    // profundidade=false desenha por cima da cena: util enquanto o corte de
    // profundidade nao estiver calibrado com o do jogo.
    void Camera(uintptr_t viewMatrixAddress, float fovYDegrees,
                float nearPlane, float farPlane, bool depth, bool flipY,
                bool automatic);
    void OnDeviceLost();
    void OnDeviceReset();

    // Spawns one particle. Position is world space; size is in world units;
    // life in seconds. Colour is 0xRRGGBB and fades to nothing over the life.
    // `attached` interpreta a posicao em coordenadas DO CARRO: a particula
    // passa a acompanhar o veiculo em vez de ficar para tras no mundo.
    // `vx,vy,vz` e velocidade no mesmo referencial da posicao (m/s): numa
    // particula presa, velocidade relativa ao carro. `endColour` e a cor no
    // fim da vida; a cor caminha do nascimento ate ela.
    void Spawn(float x, float y, float z, float size, float life, DWORD colour,
               bool attached = false, float vx = 0, float vy = 0, float vz = 0,
               DWORD endColour = 0);

    // O referencial do carro deste quadro: posicao e as tres linhas da matriz
    // de rotacao (os eixos do carro no mundo), como estao na pose.
    void Ancora(const float* position, const float* rotation3x3);

    // Called once a frame from the render hook, before the UI is composited.
    void Render(IDirect3DDevice9* device, float dt);

    void Clear();
    int  Count();
}
