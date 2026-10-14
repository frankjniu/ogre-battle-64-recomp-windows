# Session 140 handoff — the browser build ships as a static package for GitHub Pages

## Goal

Continue the browser port with the intention of a static build servable from
GitHub Pages (developer, session start). The renderer work of sessions 138 and
139 had made the game run in the browser and had left three renderer gaps; none
of them blocked deployment. What blocked deployment was the delivery: the build
produced `build-wasm/` and the page expected to be served from the repository
root by `debug/server.py`, which sets `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy`, and a static host cannot set response headers.

## Result

1. **`app/web/coi-serviceworker.js` (new)** is coi-serviceworker v0.1.7 (MIT,
   Guido Zuidhof and contributors) copied verbatim under a header comment that
   records the version, the licence, the upstream URL and the reason the file is
   in the tree. It is a service worker: it adds `Cross-Origin-Opener-Policy:
   same-origin` and `Cross-Origin-Embedder-Policy` to every response it serves,
   and the page reloads itself once when the worker takes control. That is what
   makes a host without header support able to run the pthread build.
2. **`app/web/index.html`** loads it with a `<script src>` tag in the head,
   because the worker registers `document.currentScript.src`. The same block
   loads `ogre-web-config.js`. A new guard in the inline script states why
   nothing happens when `SharedArrayBuffer` is absent, instead of letting the
   glue abort with a bare exception.
3. **`app/web/ogre-web-config.js` (new)** holds `window.OGRE_WASM_DIR =
   "../../build-wasm/"`, the repository layout's path. `index.html` falls back to
   the same value. `make web-dist` generates a copy of that one file with `"./"`,
   so one page serves both layouts and the build patches no source file.
4. **`make web-dist` (Makefile, new target)** runs `make web` and writes
   `dist-web/`: `index.html`, `web.js`, `audio-worklet.js`,
   `coi-serviceworker.js`, `ogrebattle64.js`, `ogrebattle64.wasm`, a generated
   `ogre-web-config.js` and `.nojekyll`. It fails when a file it expects is
   missing, and when `build-wasm/` holds an `ogrebattle64.*` artifact the package
   does not carry.
5. **`.github/workflows/pages.yml` (new)** builds the package on a hosted runner
   and deploys `dist-web/` with `actions/upload-pages-artifact` +
   `actions/deploy-pages` on every push to `main` and on `workflow_dispatch`. It
   needs `vars.OGRE_DATA_REPO` + `secrets.OGRE_DATA_TOKEN` for the generated game
   code, the same as `.github/workflows/wasm.yml`, and the build job is skipped
   without them.
6. **`debug/probes/isolate.cjs` (new)** measures the deployment contract:
   `crossOriginIsolated`, whether `SharedArrayBuffer` exists, whether a service
   worker controls the page, and whether the shell reached `WASM runtime
   initialized`. Exit 0 means the page is isolated.
7. **The package has no ROM data.** The page reads the visitor's own dump
   through the File API, writes it to `/rom.z64`, and never uploads it.

## Evidence

### 1. The service worker is byte-identical to upstream

```sh
curl -sSfL -o /tmp/coi-upstream.js \
  https://raw.githubusercontent.com/gzuidhof/coi-serviceworker/master/coi-serviceworker.js
tail -n +20 app/web/coi-serviceworker.js > /tmp/coi-vendored-body.js
diff -u /tmp/coi-upstream.js /tmp/coi-vendored-body.js   # no output
```

The first 19 lines of the vendored file are the project's comment and the
upstream `/*! coi-serviceworker v0.1.7 ... licensed under MIT */` banner.

### 2. A plain static server isolates the page and runs the game

The server sends no COOP/COEP and serves `.wasm` as `application/wasm`:

```sh
make web-dist
python3 -m http.server --bind 127.0.0.1 --directory dist-web 8940 &
curl -sS -D - -o /dev/null http://127.0.0.1:8940/ogrebattle64.wasm
#   Content-type: application/wasm
#   (no Cross-Origin-Opener-Policy, no Cross-Origin-Embedder-Policy)

node debug/probes/isolate.cjs --url http://127.0.0.1:8940/ --secs 40
# url        http://127.0.0.1:8940/
# isolated   true (first seen at 811 ms)
# SharedArrayBuffer true
# secure context true
# service worker controlling
# wasm ready true
# ISOLATED (SharedArrayBuffer available)

node debug/probes/boot.cjs --url http://127.0.0.1:8940/ --attempts 2 --secs 60 \
  --out web-dist-boot
# attempt 1: sawRealDl=false rendered=true
# RENDERED {"w":640,"h":480,"total":307200,"nonBlack":24508,"colorful":10160,...}
```

The capture is `docs/proofs/web-dist-title.png`; it is the title screen, the same
picture as `debug/out/web-headers-boot-canvas.png` below.

### 3. A/B against the header-setting development server

Same wasm, repository layout, `python3 debug/server.py` (COOP/COEP, no service
worker):

```sh
node debug/probes/isolate.cjs --url http://127.0.0.1:8931/app/web/index.html
# isolated   true (first seen at 484 ms)
# service worker supported, not controlling      <- the script registered nothing

node debug/probes/boot.cjs --url http://127.0.0.1:8931/app/web/index.html \
  --attempts 2 --secs 60 --out web-headers-boot
# RENDERED {"w":640,"h":480,"total":307200,"nonBlack":24840,"colorful":15076,...}
```

`24840 / 15076` is the session-139 baseline for this probe, unchanged. The
`dist-web` run's `24508 / 10160` is a different frame of the same title fade; both
canvas captures are the same screen, and the service-worker path changes nothing
about the wasm or the page behaviour. `service worker supported, not controlling`
is the check that the development flow installs no worker.

### 4. A GitHub-Pages-like subpath works

A project site is served from `/<repo>/`, so the page and the worker both live one
directory down. The package was served that way with the same header-less server:

```sh
mkdir -p /tmp/pages-sim/ogre-battle-64-recomp
cp -R dist-web/. /tmp/pages-sim/ogre-battle-64-recomp/
python3 -m http.server --bind 127.0.0.1 --directory /tmp/pages-sim 8941 &
node debug/probes/isolate.cjs --url http://127.0.0.1:8941/ogre-battle-64-recomp/
# isolated true (first seen at 650 ms), service worker controlling, wasm ready true
```

### 5. gzip-encoded responses survive the worker

A service worker that copies response headers onto a new `Response` can leave a
stale `Content-Encoding` on a body the browser has already decompressed. GitHub
Pages compresses text, so this was measured with a throwaway server
(`/tmp/gzip-server.py`, not committed) that gzip-encodes `.html`, `.js` and
`.wasm`:

```sh
curl -sS -D - -o /dev/null http://127.0.0.1:8942/ogrebattle64.wasm
#   Content-Type: application/wasm
#   Content-Encoding: gzip
#   Content-Length: 2981147
node debug/probes/isolate.cjs --url http://127.0.0.1:8942/
# isolated true (first seen at 627 ms), service worker controlling, wasm ready true
```

`wasm ready true` means the 21 MB module was fetched, decompressed and
instantiated through the worker.

## What the package is

```text
dist-web/index.html             a copy of app/web/index.html, unmodified
dist-web/web.js                 input, audio, ROM picker, boot
dist-web/audio-worklet.js       the game's audio ring buffer
dist-web/coi-serviceworker.js   COOP/COEP for hosts that cannot send them
dist-web/ogre-web-config.js     window.OGRE_WASM_DIR = "./"
dist-web/ogrebattle64.js        Emscripten glue
dist-web/ogrebattle64.wasm      game + N64ModernRuntime, 21039450 bytes
dist-web/.nojekyll              for a branch-based Pages deploy
```

`make web-dist` measured `dist-web (20M)`. `dist-web/` is gitignored.

## Files changed

| file | change |
|---|---|
| `app/web/coi-serviceworker.js` | new; coi-serviceworker v0.1.7, MIT, vendored with a header comment |
| `app/web/ogre-web-config.js` | new; the repository layout's `OGRE_WASM_DIR` |
| `app/web/index.html` | loads the two new scripts; the `SharedArrayBuffer` guard |
| `Makefile` | `web-dist` target: copies the package, fails on a missing artifact, and normalises the file modes with `chmod -R a+rX` |
| `.gitignore` | `dist-web/` |
| `.github/workflows/pages.yml` | new; `make web-dist` then `actions/upload-pages-artifact@v5` (`include-hidden-files: true`) + `actions/deploy-pages@v5` |
| `debug/probes/isolate.cjs` | new probe |
| `debug/package.json` | `npm run isolate` |
| `docs/WEB-PORT-DEPLOYMENT.md` | rewritten for the package, the worker, and Pages |
| `docs/guides/app-build.md`, `docs/guides/web-probes.md` | `make web-dist`; the plain-server route and the new probe |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status, decision, log, index |
| `docs/proofs/web-dist-title.png` | the `dist-web` title capture |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session).

## Verification run

- `make web-dist` — the Emscripten build was already current; the target
  reconfigured, built, and wrote and checked `dist-web/`.
- `debug/probes/isolate.cjs` — four URLs: the plain server, the plain server
  behind gzip, the Pages-like subpath, and `debug/server.py`.
- `debug/probes/boot.cjs` — the plain server and `debug/server.py`, two
  attempts each, 60 s.
- No native build was run. No renderer source changed, so the session-139
  proofs still hold.

## Probes

No probe was left in the tree. One throwaway server (`/tmp/gzip-server.py`) and
one throwaway directory (`/tmp/pages-sim/`) were used for the checks in §4 and
§5; neither is in the repository. `debug/probes/isolate.cjs` is a diagnostic
that stays.

## Open

1. **The Pages workflow has not run.** GitHub Pages was enabled for the
   repository during this session with the Actions source
   (`gh api repos/lfarroco/ogre-battle-64-recomp/pages` reports
   `"build_type": "workflow"`, `html_url
   https://lfarroco.github.io/ogre-battle-64-recomp/`), and the repository
   already carries `vars.OGRE_DATA_REPO` (`lfarroco/ogre-recomp-private`) and
   `secrets.OGRE_DATA_TOKEN`, so the first push to `main` is what triggers the
   build. Nothing else is required for the deploy half. The build half is the
   same code path as `.github/workflows/wasm.yml`, which has also never had an
   observed run.
2. **The first load fetches the whole 21 MB wasm before the visitor picks a
   ROM.** `index.html` loads the glue at page load, which is what every local
   probe assumes. Loading it when the ROM picker is used would cut a visit that
   never starts the game to a few tens of KB.
3. **Session 139's renderer items are unchanged**: the render target's height and
   the canvas size do not follow the VI mode, framebuffer-as-texture is missing,
   and `K4`/`K5` return 0.
4. **The browser build still assigns gamepads to N64 controllers 2-4** (`PLAN.md`
   open work 13). A player on the deployed page has no pad in slot 0.
