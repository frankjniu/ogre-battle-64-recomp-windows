# Handoff — 2026-10-12, session 135: the Windows release build's only compile error

**Goal.** The Release workflow's Windows job fails to compile. Find the error and
fix it.

**Result.** `app/src/renderer.cpp` created the trace dump directories with POSIX
`mkdir(path, 0777)` at four sites. The Windows job builds with clang-cl against
the MSVC and UCRT headers, which declare no two-argument `mkdir` (`<sys/stat.h>`
declares nothing for MSVC; `<direct.h>` declares the one-argument `_mkdir`), so
all four are errors there. The four calls are now
`std::filesystem::create_directories` through a local `ensure_dir` helper. That is
the call the rest of the app already uses (`launcher.cpp`, `crash_log.cpp`,
`hd_backgrounds.cpp`, `save_import.cpp`), and it is the same on every platform.

## 1. The failure

Run `38188914911` ("Release", `main`, `dba6233`, 2026-10-12T01:57Z, `gh run view
38188914911`). Job "Build Windows (x86-64)" failed in "Build and package";
macOS, Linux and Plan passed, and the release attach step was skipped.

The whole job has four compiler errors and they are all this call:

```
app/src/renderer.cpp(586,11): error: no member named 'mkdir' in the global namespace
  586 |         ::mkdir(dump_dir.c_str(), 0777);
app/src/renderer.cpp(587,11): error: no member named 'mkdir' in the global namespace
  587 |         ::mkdir(dir.c_str(), 0777);
app/src/renderer.cpp(1587,17): error: use of undeclared identifier 'mkdir'
 1587 |                 mkdir(dir.c_str(), 0777);
app/src/renderer.cpp(1588,17): error: use of undeclared identifier 'mkdir'
 1588 |                 mkdir((dir + "/tex").c_str(), 0777);
```

The two spellings of the diagnostic are the two scopes: `::mkdir` is not a member
of the global namespace, and an unqualified `mkdir` is not declared at all. Sites
586/587 are `TexTrace::dump` (`OGRE_TEX_TRACE_DUMP`), sites 1587/1588 are the
trace's dump-directory lambda. Both were added by `b941040` ("fixing: dark
sprites", session 130), which is also the commit that introduced the trace knobs.

`gh run view 38188914911 --log-failed` lists a build line for each of the app's 21
translation units and only `renderer.cpp` produced an error, so this call is the
only Windows compile error in the app. The `getenv`/`fopen` deprecation notes in the same log are
warnings and do not fail the build.

## 2. The change

`app/src/renderer.cpp`:

- `<sys/stat.h>` removed (it was included only for `mkdir`) and `<filesystem>`
  added.
- A helper beside the other small file-scope utilities:

  ```cpp
  void ensure_dir(const std::string& path) {
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
  }
  ```

  The `error_code` overload does not throw. `create_directories` also makes the
  parents and treats an existing directory as success, which is what the dump
  path needs on every draw; `mkdir` returned `EEXIST` there and the return value
  was never read.
- The four calls become `ensure_dir(dump_dir)`, `ensure_dir(dir)`,
  `ensure_dir(dir)` and `ensure_dir(dir + "/tex")`.

## 3. Verification

- `make -C build-app src/renderer.o` compiles the changed file on this machine
  (Apple clang, C++20). The only diagnostic is the pre-existing
  `-Wxor-used-as-pow` at line 1045 (`p[2 ^ 3]`), which is unrelated.
- `grep -rn "mkdir" app/src` returns nothing, so no two-argument call remains in
  the app. `app/web/web.js`'s `Module.FS.mkdir("/ogre")` is the Emscripten
  filesystem API and is not this call.
- `git diff patches/rt64-ob64.patch` against `git -C tools/RT64 diff` differs by
  one line, the plume submodule's `-dirty` marker, so the RT64 change this tree
  builds with is the patch the hosted runner applies. The Windows failure is not
  a patch-drift failure.
- **Not run here: the Windows job.** The hosted job's "Unpack the generated game
  code" step fails when `ogre-data.txt`'s `public commit` differs from
  `GITHUB_SHA`, so a Windows run needs the private data bundle regenerated for
  the new commit (`make release`, which needs the ROM). The compile-level check
  above and the four error lines are the evidence this session has.

## 4. What to do to ship it

`make release VERSION=…` regenerates the bundle, commits it to
`lfarroco/ogre-recomp-private`, and dispatches the workflow; `platforms=windows`
is the ~8-minute iteration path. A plain `workflow_dispatch` on a branch that the
bundle does not record stops before compiling.

## Files changed

- `app/src/renderer.cpp`
- `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, this file.

No probe was used, so nothing was reverted.
