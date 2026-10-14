#!/usr/bin/env node
'use strict';

// Capture a canvas series around a log marker.
//
// The other probes sample at a wall-clock cadence, which misses a short scene
// (the njpeg background pass is about a second long). This one waits for a
// regex to appear in `window.ogreLog` and then captures `--shots` frames
// `--gap` ms apart, so the frames bracket the moment the marker fired.
//
// The marker is usually one of the renderer's own diagnostics, and the scene is
// usually reached with the scripted-pad knobs in the URL. Both are passed on the
// command line:
//
//   node debug/probes/atmark.cjs --wait 'GFX-S2D' --shots 8 --gap 150 \
//     --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_TAP_MS=1500&OGRE_TAP_BUTTON=start,a,start,a'
//
// `--wait` is a regular expression matched against each log line. `--before <n>`
// additionally keeps the last n log lines before the marker fires, so a crash
// before the marker still leaves a trace.
//
// Usage:
//   node debug/probes/atmark.cjs [--wait RE] [--shots 6] [--gap 150]
//                               [--timeout 180] [--out atmark] [--rom PATH] [--url URL]
//
// Exit codes: 0 captured, 2 the marker never appeared, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'atmark');
const waitRe = opts.wait ? new RegExp(opts.wait) : /GFX-S2D/;
const shots = h.num(opts.shots, 6);
const gap = h.num(opts.gap, 150);
const timeoutSecs = h.num(opts.timeout, 180);

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);
  const { page, errors } = await h.openPage(browser);

  try {
    await h.gotoPage(page, cfg);
    await h.loadRom(page, cfg);

    // Poll the ring buffer for the marker. The log is a JS array, so this is a
    // page-side regex and costs one round trip per poll.
    const t0 = Date.now();
    let fired = false;
    while ((Date.now() - t0) / 1000 < timeoutSecs) {
      fired = await page.evaluate(
        (re) => window.ogreLog.find(new RegExp(re)).length > 0,
        waitRe.source
      ).catch(() => false);
      if (fired) {
        break;
      }
      await page.waitForTimeout(100);
    }

    const lines = [];
    const pixels = [];
    if (fired) {
      for (let i = 0; i < shots; i++) {
        const canvas = await h.screenshotCanvas(page);
        const px = canvas ? await h.analyzePng(decoder, canvas, { inset: 1 }).catch(() => null) : null;
        const name = `${prefix}-${String(i).padStart(2, '0')}.png`;
        if (canvas) {
          fs.writeFileSync(name, canvas);
        }
        pixels.push({ shot: i, file: name, px });
        lines.push(`shot ${i}: ${px ? JSON.stringify(px) : 'n/a'}`);
        await page.waitForTimeout(gap);
      }
    }

    const log = await page.evaluate(() => window.ogreLog.text()).catch(() => '');
    h.writeOut(prefix, 'live.txt', lines.join('\n') + '\n');
    h.writeOut(prefix, 'log.txt', log);
    h.writeErrors(prefix, errors);
    h.writeJson(prefix, 'result.json', { marker: waitRe.source, fired, pixels });

    console.log(fired ? `MARKER ${waitRe.source} fired` : `marker ${waitRe.source} never fired`);
    for (const p of pixels) {
      console.log(`  shot ${p.shot}: ${p.px ? JSON.stringify(p.px) : 'n/a'}`);
    }
    return fired ? 0 : 2;
  } finally {
    await page.close().catch(() => {});
    await browser.close().catch(() => {});
  }
})()
  .then((code) => process.exit(code))
  .catch((err) => {
    console.error(err && err.stack ? err.stack : String(err));
    process.exit(1);
  });
