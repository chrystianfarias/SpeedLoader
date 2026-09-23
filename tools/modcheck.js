// Confere um main.js de mod sem abrir o jogo.
//
//   node tools/modcheck.js mods/spawn-car/main.js
//
// Faz duas coisas, nessa ordem:
//
//   1. le os nomes: toda funcao que o arquivo CHAMA existe nele?
//   2. executa: carrega o mod com um `speed` de mentira e dispara cada evento
//      registrado, procurando erros de referencia.
//
// A primeira e a que importa, e existe por um motivo concreto: `node --check`
// valida sintaxe e acha tudo em ordem num arquivo de onde uma funcao foi
// apagada por engano; o jogo so reclama na hora em que alguem clica no botao
// que passa por ela. A segunda cobre menos do que parece — um stub so alcanca
// os caminhos que consegue, e o que vem depois de uma chamada nativa
// bem-sucedida nao roda aqui.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const alvo = process.argv[2];
if (!alvo) {
  console.error('uso: node tools/modcheck.js <main.js>');
  process.exit(2);
}

const fonte = fs.readFileSync(alvo, 'utf8');
let falhas = 0;

// ---------------------------------------------------------------- os nomes

// Comentarios e literais viram espaco: "function" dentro de uma string nao
// declara nada, e um parentese dentro de um comentario nao chama nada.
const codigo = fonte
  .replace(/\/\/[^\n]*/g, ' ')
  .replace(/\/\*[\s\S]*?\*\//g, ' ')
  .replace(/'(?:[^'\\]|\\.)*'/g, "''")
  .replace(/"(?:[^"\\]|\\.)*"/g, '""')
  .replace(/`(?:[^`\\]|\\.)*`/g, '``');

const declarados = new Set();
for (const m of codigo.matchAll(/function\s+([A-Za-z_$][\w$]*)/g)) declarados.add(m[1]);
for (const m of codigo.matchAll(/(?:const|let|var)\s+([A-Za-z_$][\w$]*)/g)) declarados.add(m[1]);
for (const m of codigo.matchAll(/([A-Za-z_$][\w$]*)\s*=>/g)) declarados.add(m[1]);

// O que vem de outro arquivo do mod (`import { abastecer } from "./fuel.js"`)
// existe tanto quanto o que foi declarado aqui.
for (const m of codigo.matchAll(/import\s*{([^}]*)}/g)) {
  for (const nome of m[1].split(',')) {
    const limpo = nome.trim().split(/\s+as\s+/).pop().trim();
    if (limpo) declarados.add(limpo);
  }
}
for (const m of codigo.matchAll(/import\s+([A-Za-z_$][\w$]*)\s+from/g)) declarados.add(m[1]);

const permitidos = new Set([
  'if', 'for', 'while', 'switch', 'catch', 'return', 'typeof', 'function',
  'do', 'else', 'new', 'delete', 'void', 'in', 'of', 'instanceof', 'await',
  'yield', 'throw', 'case',
  'speed', 'console', 'Math', 'Number', 'String', 'Boolean', 'Array', 'Object',
  'JSON', 'Set', 'Map', 'Date', 'RegExp', 'Error', 'Promise', 'parseInt',
  'parseFloat', 'isNaN', 'isFinite', 'setTimeout', 'setInterval',
  'clearTimeout', 'clearInterval', 'Symbol', 'BigInt', 'structuredClone',
  'encodeURIComponent', 'decodeURIComponent',
  'Uint8Array', 'Int8Array', 'Uint16Array', 'Int16Array', 'Uint32Array',
  'Int32Array', 'Float32Array', 'Float64Array', 'ArrayBuffer', 'DataView'
]);

for (const m of codigo.matchAll(/(^|[^.\w$])([A-Za-z_$][\w$]*)\s*\(/gm)) {
  const nome = m[2];
  if (permitidos.has(nome) || declarados.has(nome)) continue;
  console.error('FALTA: ' + nome + '() e chamada e nao existe neste arquivo');
  falhas++;
}

if (falhas) process.exit(1);

// ------------------------------------------------------------- a execucao

const eventos = {};
const store = {};
const chamadas = [];

// Enderecos viram numeros plausiveis e leituras devolvem inteiros pequenos:
// a ideia e passar pelas verificacoes de limite e percorrer o codigo, nao
// simular o jogo.
const mem = new Proxy({}, {
  get: (_, nome) => {
    const n = String(nome);
    if (n === 'readString') return () => 'FAKE';
    if (n === 'readBytes') return () => new ArrayBuffer(0x940);
    if (n === 'valid') return () => true;
    if (n === 'alloc') return () => 0x10000000;
    if (n.startsWith('write') || n === 'patch' || n === 'nop') return () => true;
    if (/^read[IU](8|16|32)$/.test(n)) return () => 1;
    if (/^readF/.test(n)) return () => 1.5;
    return () => 0x1000;
  }
});

const speed = {
  version: '0.0.0-fake',
  mod: { id: 'check', name: 'check', dir: '.' },
  mods: () => [],
  now: () => Date.now(),
  on: (ev, cb) => { (eventos[ev] = eventos[ev] || []).push(cb); },
  off: () => {},
  call: (addr) => { chamadas.push(addr); return 0x2000; },
  mem,
  menu: { add: () => 1, hash: () => 0x1234 },
  game: {
    state: () => 6, stateName: () => 'gameplay', isRacing: () => true,
    hasFocus: () => true, player: () => 0x1000, car: () => 0x1000,
    gearbox: () => 0x1000, physics: () => 0x1000, engine: () => 0x1000,
    window: () => 1, carId: () => 1, carModel: () => 'FAKE',
    telemetry: () => null, hash: () => 1, sendFrontendMessage: () => {},
    offset: {}, addr: {}
  },
  store: {
    get: (k, d) => (k in store ? store[k] : d),
    set: (k, v) => { store[k] = v; },
    all: () => store, clear: () => {},
    car: { get: (k, d) => d, set: () => true, id: () => 1 }
  },
  audio: {
    load: () => 1, play: () => true, stop: () => {}, stopAll: () => {},
    unload: () => true, volume: () => 1, ready: () => true
  },
  ui: {
    send: () => {}, show: () => {}, hide: () => {}, toggle: () => true,
    visible: () => true, capture: () => {}, capturing: () => false,
    reload: () => {}, devtools: () => {}, size: () => ({ width: 0, height: 0 })
  }
};

const ctx = vm.createContext({
  speed, console,
  setTimeout: () => 0, setInterval: () => 0,
  clearTimeout: () => {}, clearInterval: () => {}
});

try {
  vm.runInContext(fonte, ctx, { filename: path.resolve(alvo) });
} catch (e) {
  // Um mod que usa `import` e um modulo ES: fora do alcance deste harness, e
  // nao e falha dele.
  if (/import statement/.test(e.message)) {
    console.log('nomes ok; execucao pulada (modulo ES)');
    process.exit(0);
  }
  console.error('FALHOU ao carregar: ' + e.message);
  process.exit(1);
}

function disparar(rotulo, cb, arg) {
  try {
    cb(arg);
  } catch (e) {
    // Um ReferenceError e codigo faltando. O resto e o stub sendo stub.
    if (e instanceof ReferenceError) {
      console.error('FALHOU em ' + rotulo + ': ' + e.message);
      falhas++;
    }
  }
}

for (const [ev, cbs] of Object.entries(eventos)) {
  for (const cb of cbs) {
    disparar('"' + ev + '"', cb, { channel: ev, data: {}, indice: 0, valor: 0, key: 0x75 });
  }
}

// As teclas uma a uma: e por onde um mod costuma pendurar suas funcoes.
for (const cb of (eventos['keydown'] || [])) {
  for (let vk = 0x70; vk <= 0x7B; vk++) {
    disparar('tecla 0x' + vk.toString(16), cb, { key: vk });
  }
}

console.log(falhas
  ? falhas + ' falha(s)'
  : 'ok: ' + Object.keys(eventos).length + ' eventos, ' +
    chamadas.length + ' chamadas nativas simuladas');
process.exit(falhas ? 1 : 0);
