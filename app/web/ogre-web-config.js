// Where this copy of the page loads its Emscripten build from.
//
// Two layouts are supported and neither patches this page:
//
//   * the repository layout, where `debug/server.py` serves the repo root and
//     this file lives next to the page at `app/web/`, so the build is at
//     `../../build-wasm/`;
//   * the static layout `make web-dist` writes, where the page and the build
//     sit in one directory and the generated copy of this file says `"./"`.
//
// `index.html` loads this before its own inline script. Setting
// `window.OGRE_WASM_DIR` by hand (or from an enclosing page) still wins.
window.OGRE_WASM_DIR = window.OGRE_WASM_DIR || "../../build-wasm/";
