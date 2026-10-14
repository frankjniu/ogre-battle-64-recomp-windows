#!/usr/bin/env node
'use strict';

// Cross-origin-isolation check for the browser build.
//
// The Emscripten build uses pthreads, so the page needs `SharedArrayBuffer`,
// which exists only on a cross-origin-isolated page. Two configurations:
//
//   * a host that sends COOP/COEP itself (`python3 debug/server.py`): the page
//     is isolated on the first load;
//   * a static host that cannot (`make web-dist` on GitHub Pages, or any plain
//     file server): `app/web/coi-serviceworker.js` registers, reloads the page
//     once, and serves the headers itself.
//
// This probe measures which of those happened and fails when neither did.
//
// Usage:
//   make web-dist
//   python3 -m http.server -d dist-web 8940 &
//   node debug/probes/isolate.cjs --url http://127.0.0.1:8940/
//
//   python3 debug/server.py &
//   node debug/probes/isolate.cjs --url http://127.0.0.1:8931/app/web/index.html
//
// Exit codes: 0 isolated, 2 not isolated, 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const secs = h.num(opts.secs, 30);

// One snapshot of everything the answer depends on. `ready` is the shell's own
// line, so it also says whether the glue got far enough to fetch the wasm.
const SNAPSHOT = () => ({
  isolated: window.crossOriginIsolated === true,
  sab: typeof SharedArrayBuffer !== 'undefined',
  secure: window.isSecureContext === true,
  swSupported: 'serviceWorker' in navigator,
  controlled: !!(navigator.serviceWorker && navigator.serviceWorker.controller),
  ready: !!(window.ogreLog && window.ogreLog.contains('WASM runtime initialized')),
});

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);

  const started = Date.now();
  await page.goto(cfg.url);

  // The page reloads itself once when the service worker takes control, so a
  // snapshot can be taken mid-navigation. Retry instead of failing on that.
  let snap = null;
  let sawIsolated = null;
  const deadline = started + secs * 1000;
  while (Date.now() < deadline) {
    try {
      snap = await page.evaluate(SNAPSHOT);
    } catch (e) {
      snap = null;
    }
    if (snap && snap.isolated && !sawIsolated) {
      sawIsolated = { ...snap, ms: Date.now() - started };
    }
    if (sawIsolated && snap && snap.ready) {
      break;
    }
    if (!sawIsolated && snap && !snap.swSupported) {
      break; // nothing can change; report it
    }
    await h.sleep(500);
  }

  const last = snap || { isolated: false, sab: false, secure: false, swSupported: false, controlled: false, ready: false };
  console.log(`url        ${cfg.url}`);
  console.log(`isolated   ${last.isolated}${sawIsolated ? ` (first seen at ${sawIsolated.ms} ms)` : ''}`);
  console.log(`SharedArrayBuffer ${last.sab}`);
  console.log(`secure context ${last.secure}`);
  console.log(`service worker ${last.swSupported ? (last.controlled ? 'controlling' : 'supported, not controlling') : 'unsupported'}`);
  console.log(`wasm ready ${last.ready}`);
  if (errors.length) {
    console.log(`page errors ${errors.length}:`);
    for (const e of errors.slice(0, 10)) {
      console.log('  ' + e.split('\n')[0]);
    }
  }

  if (last.isolated && last.sab) {
    console.log('ISOLATED (SharedArrayBuffer available)');
  } else {
    console.log('NOT ISOLATED (the pthread build cannot start here)');
  }

  await browser.close().catch(() => {});
  process.exit(last.isolated && last.sab ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
