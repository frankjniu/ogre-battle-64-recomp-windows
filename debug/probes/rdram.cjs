#!/usr/bin/env node
'use strict';

// Guest-RDRAM dump from the browser build, so a bug in the wasm run is read
// offline with `tools/rdram.py` exactly like a native `OGRE_DUMP_RDRAM` image.
//
// The wasm build writes the dump into its own virtual filesystem, so this probe
// reads it back out through `Module.FS.readFile` and puts it next to the page
// log. The dump itself is taken by `app/src/ram_key.cpp`, driven per frame from
// `ogre_gfx_flush()` in `app/src/web_renderer.cpp`:
//
//   OGRE_RAM_KEY_AT_MS=<n>   take the dump once the run reaches n ms
//   OGRE_RAM_KEY_FULL=1      the whole 8 MiB image instead of a window
//   OGRE_RAM_KEY_FULL=0      a window at OGRE_RAM_KEY_ADDR (default 0x802696C8)
//   OGRE_RAM_KEY_OUT=<dir>   where in the virtual filesystem to write
//
// The image is written in the game's logical byte order, which is what
// `tools/rdram.py` reads (see docs/guides/app-build.md -> "Reading a dump").
//
// Usage:
//   node debug/probes/rdram.cjs [--secs 120] [--wait 'GFX-VI'] [--out name]
//                               [--query 'OGRE_RAM_KEY_AT_MS=30000&OGRE_RAM_KEY_FULL=1']
//
// Exit codes: 0 the dump was read out, 2 no dump appeared in time, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'rdram');
const secs = h.num(opts.secs, 120);
const wait = opts.wait ? new RegExp(String(opts.wait)) : null;

/** Read a file out of the wasm filesystem as a Buffer (base64, chunked). */
async function readFsFile(page, path) {
  const b64 = await page.evaluate((p) => {
    const data = Module.FS.readFile(p);   // Uint8Array
    let out = '';
    const chunk = 0x8000;
    for (let i = 0; i < data.length; i += chunk) {
      out += String.fromCharCode.apply(null, data.subarray(i, i + chunk));
    }
    return btoa(out);
  }, path);
  return Buffer.from(b64, 'base64');
}

function withQuery(url, extra) {
  if (!extra) return url;
  return url + (url.includes('?') ? '&' : '?') + extra;
}

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);
  const url = withQuery(cfg.url, opts.query || '');
  await h.gotoPage(page, { ...cfg, url });
  await h.loadRom(page, cfg);

  const t0 = Date.now();
  let dumpPath = null;
  let waited = false;
  const samples = [];

  while (Date.now() - t0 < secs * 1000) {
    const lines = await page
      .evaluate(() => (window.ogreLog ? window.ogreLog.find(/\[ramkey\].*wrote /) : []))
      .catch(() => []);
    if (lines.length > 0) {
      const m = /wrote (\S+) \(/.exec(lines[lines.length - 1]);
      if (m) dumpPath = m[1];
    }
    if (wait) {
      waited = await page
        .evaluate((src) => window.ogreLog.find(new RegExp(src)).length > 0, wait.source)
        .catch(() => false);
    }
    const state = await h.readState(page).catch(() => null);
    samples.push(`t=${((Date.now() - t0) / 1000).toFixed(1)} tasks=${state ? h.taskCount(state.gfx) : -1} dump=${dumpPath || '-'}`);
    if (dumpPath && (!wait || waited)) break;
    await h.sleep(1000);
  }

  const log = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, log + '\n');
  fs.writeFileSync(`${prefix}-samples.txt`, samples.join('\n') + '\n');
  h.writeErrors(prefix, errors);

  let wrote = false;
  if (dumpPath) {
    const bytes = await readFsFile(page, dumpPath).catch((e) => {
      console.error(`READ FAILED ${dumpPath}: ${e.message}`);
      return null;
    });
    if (bytes) {
      fs.writeFileSync(`${prefix}-rdram.bin`, bytes);
      wrote = bytes.length === 0x800000;
      console.log(`DUMP ${dumpPath} -> ${prefix}-rdram.bin (${bytes.length} bytes)`);
    }
  } else {
    console.log('NO DUMP');
  }
  h.writeJson(prefix, 'result.json', { dumpPath, bytes: wrote ? 0x800000 : 0, wrote });
  await browser.close().catch(() => {});
  process.exit(wrote ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
