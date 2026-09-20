/// <reference path="../../sdk/speedloader.d.ts" />
//
// Shows an animated SpeedLoader card while the boot splash ("press a button")
// is on screen, and takes it away as soon as the game moves on.
//
// Knowing we are on the splash took three tries, and the log settled each one:
//
//   1. FEngFindPackage("Chyron.fng") — never resolved, even with the splash
//      sitting there for thirty seconds. The mechanism is fine (hide-hud uses
//      the same registry to switch HUD packages off); the name is what is
//      wrong.
//   2. The gameflow state — the log said `state=3 (frontend)` from the first
//      second, splash on screen. The splash lives inside the frontend state,
//      same as the menus, so the state cannot tell them apart.
//   3. What did move: the byte at 0x836495 went 0 -> 1 around 4.9s, which is
//      the splash becoming ready (Pops found it as the latch the Chyron screen
//      checks before accepting its advance event). It never goes back to 0,
//      so it marks "the splash has appeared", not "the splash is up".
//
// So: try a list of names for the splash package, and if none of them is the
// one, fall back on the shape of the boot — the splash is what stands between
// the latch flipping and the main menu coming alive.

const FEngFindPackage = 0x52CEF0;
const SPLASH_LATCH = 0x836495;

// Candidates for the splash package. The first one that ever resolves wins and
// the rest are dropped.
const SPLASH_NAMES = [
  'Chyron.fng', 'Chyron', 'Chyron_FE.fng',
  'MC_Bootup.fng', 'UG_LS_Splash.fng', 'loading_boot.fng'
];

// The menu that follows the splash. Whichever of these comes alive means the
// splash is over - and it doubles as a check that package lookups work at all.
const MENU_NAMES = ['UI_Main.fng', 'MC_Main.fng', 'ui_Main.fng'];

const POLL_MS = 120;
const DIAGNOSTIC_MS = 12000;

// The fallback is a guess about the shape of the boot, so it gets a leash: the
// card cannot outlive this, whatever the heuristic believes.
const FALLBACK_MAX_MS = 60000;

const startedAt = speed.now();
let nextPoll = 0;
let nextDiagnostic = 0;
let showing = false;
let done = false;
let splashName = null;   // the candidate that answered, once one does
let dismissAt = 0;       // set when a key press should end the splash

function alive(name) {
  return speed.call(FEngFindPackage, [name], { conv: 'cdecl', ret: 'int' }) || 0;
}

function anyAlive(names) {
  for (const name of names) {
    const pkg = alive(name);
    if (pkg) return name + '=0x' + pkg.toString(16);
  }
  return null;
}

speed.on('frame', () => {
  if (done) return;

  const now = speed.now();
  if (now < nextPoll) return;
  nextPoll = now + POLL_MS;

  const menu = anyAlive(MENU_NAMES);

  let onSplash;
  if (splashName) {
    // The package alone is not enough: Chyron_FE stays loaded into the menu
    // (the log shows it alive with UI_Main up). What ends the splash is the
    // menu taking over, so both conditions have to hold.
    onSplash = !!alive(splashName) && !menu;
  } else {
    const hit = anyAlive(SPLASH_NAMES);
    if (hit) {
      splashName = hit.split('=')[0];
      console.log('splash package is "' + splashName + '" — using it from now on');
      onSplash = true;
    } else {
      // Fallback: the splash is ready (latch) and the menu has not taken over.
      onSplash = speed.mem.readU8(SPLASH_LATCH) === 1 && !menu;

      // "Press a button" is literally how this screen ends, so a key press
      // means it is on its way out even if the menu package is not up yet.
      if (dismissAt && now >= dismissAt) onSplash = false;
      if (now - startedAt > FALLBACK_MAX_MS) onSplash = false;
    }
  }

  if (now - startedAt < DIAGNOSTIC_MS && now >= nextDiagnostic) {
    nextDiagnostic = now + 1000;
    console.log('boot: latch=' + speed.mem.readU8(SPLASH_LATCH) +
                ', splash=' + (anyAlive(SPLASH_NAMES) || 'none') +
                ', menu=' + (menu || 'none') +
                ' -> ' + (onSplash ? 'SHOW' : 'hide'));
  }

  if (onSplash === showing) return;
  showing = onSplash;

  if (showing) {
    sendShow();
  } else {
    speed.ui.send('hide', null);
    done = true;
    console.log('splash gone, card dismissed');
  }
});

function sendShow() {
  // The count is only honest once every mod has loaded, which is true by the
  // time a frame runs.
  const mods = speed.mods();
  speed.ui.send('show', {
    version: speed.version,
    mods: mods.length,
    names: mods.map((m) => m.name)
  });
  console.log('splash is up, card shown (' + mods.length + ' mods loaded)');
}

// The page mounts a moment after main.js starts running, and on the splash
// that moment matters: the first "show" went out at 1.92s while the page only
// mounted at 1.98s, so nobody was listening and the card never appeared. The
// page announces itself when it is ready, and we say it again.
speed.on('ui:ready', () => {
  if (showing) sendShow();
});

// Any key while the card is up, in fallback mode, is the button that dismisses
// the splash. Give the screen a moment to change before taking the card away.
speed.on('keydown', () => {
  if (!done && showing && !splashName && !dismissAt) dismissAt = speed.now() + 400;
});

// Once the game is loading or racing there is no splash to wait for.
speed.on('gamestate', (e) => {
  if (!done && e.state >= 4) {
    done = true;
    if (showing) {
      showing = false;
      speed.ui.send('hide', null);
    }
  }
});
