# Browser deployment

The Emscripten build uses pthreads, so the page needs `SharedArrayBuffer`, and
browsers expose `SharedArrayBuffer` only on a **secure context** (HTTPS or
localhost) that is **cross-origin isolated**. The isolation comes from two
response headers:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without them the wasm module fails to start with `TypeError: Cannot create
SharedArrayBuffer`, and `app/web/index.html` prints the same thing in its own
words instead of loading the glue.

## The static package

```sh
make web-dist          # runs `make web` first
```

writes `dist-web/`, one directory a plain static host serves as-is:

```text
dist-web/index.html             the page (a copy of app/web/index.html)
dist-web/web.js                 input, audio, ROM picker, boot
dist-web/audio-worklet.js       the game's audio ring buffer
dist-web/ogre-web-config.js     `window.OGRE_WASM_DIR = "./"` (generated)
dist-web/ogrebattle64.js        Emscripten glue
dist-web/ogrebattle64.wasm      the game + N64ModernRuntime (~21 MB)
dist-web/coi-serviceworker.js   supplies COOP/COEP on hosts that cannot
dist-web/.nojekyll              stops GitHub Pages running the files through Jekyll
```

`dist-web/` is gitignored. The Makefile copies every `ogrebattle64.*` artifact
the build produced and fails if one is missing from the package, so an Emscripten
version that emits an extra file is caught rather than served as a 404. The last
recipe step is `chmod -R a+rX`, because the checked-in files carry the checkout's
own mode and a strict umask would leave the package mode 0600, which a static
host reading the files as another user cannot serve.

## Cross-origin isolation on a host that cannot set headers

GitHub Pages, S3, and `python3 -m http.server` cannot send response headers, so
the page cannot be isolated by the server. `app/web/coi-serviceworker.js` is a
service worker ([coi-serviceworker
v0.1.7](https://github.com/gzuidhof/coi-serviceworker), MIT, vendored verbatim
with a header comment) that adds `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy` to every response it serves, and the page reloads
itself once when the worker takes control. `index.html` loads it with a
`<script src>` tag in the head, because the worker registers
`document.currentScript.src`.

It is a no-op where the server already sends the headers: `debug/server.py` does,
so `window.crossOriginIsolated` is already true and the script returns without
registering anything. That keeps the development flow unchanged and means a
service worker is not installed for a local session.

A service worker needs a secure context, so this works on `https://` and on
`http://127.0.0.1` / `http://localhost`. It does not work from `file://` or from
a plain-HTTP LAN address.

## Local testing

Serve the package with any static server. A server that does **not** send
COOP/COEP is the interesting case, because that is what GitHub Pages is:

```sh
make web-dist
python3 -m http.server --bind 127.0.0.1 --directory dist-web 8940
# http://127.0.0.1:8940/
```

To check the headers path instead (no service worker involved):

```sh
python3 debug/server.py            # COOP/COEP + no-store, port 8931
# http://127.0.0.1:8931/app/web/index.html
```

Both configurations are measured by one probe:

```sh
node debug/probes/isolate.cjs --url http://127.0.0.1:8940/
```

It prints `isolated`, whether `SharedArrayBuffer` exists, whether a service
worker is controlling the page and whether the shell reached
`WASM runtime initialized`, and exits 2 when the page is not isolated.

## GitHub Pages

`.github/workflows/pages.yml` builds the package and deploys `dist-web/` with
`actions/upload-pages-artifact@v5` + `actions/deploy-pages@v5` on every push to
`main` and on `workflow_dispatch`. The upload sets `include-hidden-files: true`,
because the action excludes dotfiles by default and the package carries
`.nojekyll`; the action's own `tar --directory <path>` makes the artifact the
package's contents exactly. It needs the same private data bundle as the release
workflow and the compile check: the repository variable `OGRE_DATA_REPO` and the
secret `OGRE_DATA_TOKEN`. Without `OGRE_DATA_REPO` the build job is skipped
rather than failed.

One repository setting is required and the workflow cannot set it: **Settings →
Pages → Build and deployment → Source = GitHub Actions**. Until it is set, the
deploy job fails with `Get Pages site failed`. It is set on this repository
(`gh api repos/lfarroco/ogre-battle-64-recomp/pages` reports
`"build_type": "workflow"`, set on 2026-10-14), so the next push to `main` that
the build job accepts publishes the site.

The published URL is `https://lfarroco.github.io/ogre-battle-64-recomp/`.

nginx and other hosts that can set headers do not need the service worker:

```nginx
add_header Cross-Origin-Opener-Policy "same-origin" always;
add_header Cross-Origin-Embedder-Policy "require-corp" always;
```

## ROM handling

- The ROM is chosen through the browser File API and read locally.
- It is written into the wasm virtual filesystem at `/rom.z64`, and a validated
  copy plus the save data persist in IndexedDB (`/ogre`).
- Nothing is uploaded to any server, and no ROM data is in the repository or in
  `dist-web/`.
- The ROM must be a big-endian `.z64` dump; `recomp::select_rom` validates its
  hash against the supported revision and the page reports a mismatch.

## The page in the repository layout

Serving the repository root (`python3 debug/server.py`) is still supported and is
what the probes default to. The page then lives at `app/web/index.html` and loads
the build from `../../build-wasm/`; `app/web/ogre-web-config.js` holds that
default, and `make web-dist` replaces it with `"./"` in the package. No other
file differs between the two layouts.
