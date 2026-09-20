/// <reference path="../../sdk/speedloader.d.ts" />

const RATE_MS = 1000 / 30;

let lastSent = 0;
let racing = false;

speed.on('frame', () => {
  const now = speed.now();
  if (now - lastSent < RATE_MS) return;
  lastSent = now;

  const t = speed.game.telemetry();

  if (!t) {
    if (racing) {
      racing = false;
      speed.ui.send('idle');
    }
    return;
  }

  racing = true;
  speed.ui.send('rpm', { rpm: t.rpm, gear: t.gear, redline: t.redline });
});

speed.on('ui:ready', () => { lastSent = 0; });
