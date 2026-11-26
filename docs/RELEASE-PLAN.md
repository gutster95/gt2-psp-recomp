# Release plan (decided 2026-11-21)

Goal: a public release that contains **no game code and no game data**, and that a normal user can run on Windows,
macOS and Linux without WSL or Docker. The user's own discs go in, a ready `PSP/GAME/GT2PSP` folder comes out.

Decision: **option A** - the game code is recompiled and compiled on the user's machine; we ship a PSP toolchain
for Windows ourselves (pspdev has no Windows package). Rejected: shipping the PRX files (they are a translation of
the original program code - takedown risk), PRX encrypted with a disc-derived key (still distributes the code),
WSL/Docker (too much for normal users).

## What the release contains

* `convert.bat` (Windows), `convert.command` (macOS), `convert.sh` (Linux) - start the converter, open the browser.
* `converter/` - local web app (Python standard library only, `converter/server.py` + `converter/web/index.html`).
* `src/gt2psp/` (recompiler, pack tool), `runtime/` (our PSP runtime and launcher), `data/` (hot function list,
  memory profiles - our own measurements), `third_party/me-custom-core` (MIT), `scripts/` (build).
* Toolchain: downloaded on first run, pinned and SHA-256 checked
  * Linux / macOS: official pspdev release v20261101 (`pspdev-ubuntu-latest-x86_64`, `-ubuntu-24.04-arm-arm64`,
    `-macos-latest-arm64`, `-macos-15-intel-x86_64`).
  * Windows: **our own build of pspdev for Windows** (see step 2), attached to our GitHub release.
* Not contained: anything from the discs, generated code, PRX/EBOOT of the game, XMB artwork from Images/ (Sony
  artwork; the release launcher gets a neutral icon).

## Steps

1. **Done 2026-11-21.** Python build driver instead of bash/cmake/make (`scripts/build_psp.py`); its Simulation PRX
   and launcher are byte-identical to the CMake build (same objects, same link order - object names matter: the
   linker script places `*generated/*.c.obj`). Was: Windows has none of those. The driver
   calls psp-gcc / psp-ld / psp-fixup-imports / psp-prxgen / mksfoex / pack-pbp directly, with the flags the CMake
   files use today (runtime: `-O2 -G0 -ffunction-sections -fno-strict-aliasing`, generated: `GEN_EXTRA`, the linker
   script from `scripts/make_linkfile.py`, the libraries of runtime/CMakeLists.txt). Parallel compile with a process
   pool (the generated code is ~90 files). Also builds the ME library and the launcher. Check: the PRX it produces
   for the Simulation disc behaves like the CMake one (same tests on the PSP; the binary differs only in link order).
2. **Built 2026-11-21** (`scripts/wintc/build.sh`, needs the LTO plugin DLL - see DEVLOG). **pspdev for Windows**: cross-build the pspdev toolchain (binutils, gcc, newlib, pspsdk, psp-pacman not needed)
   with mingw-w64 on Linux (GitHub Actions or the dev WSL distro), using pspdev's own build scripts with
   `--host=x86_64-w64-mingw32`. Only what the build needs: psp-gcc, psp-as, psp-ld, psp-ar, psp-fixup-imports,
   psp-prxgen, psp-build-exports, mksfoex, pack-pbp, the PSPSDK libs/headers. Zip it (~150 MB), attach to our release,
   publish its SHA-256 in `converter/toolchains.json`. GPL: link the exact sources/scripts in the release notes.
   Check: compile the runtime on Windows with it and compare with the Linux build.
3. **Mostly done 2026-11-21** (`converter/toolchain.py`, `converter/toolchains.json`; the Windows url is filled in
   once the zip is attached to a release). **Converter backend**: `toolchain.ensure()` (download, verify, unpack into `tools/pspdev-<platform>/`) and `build_psp.py`. Disc checks
   (SHA-1 of the .bin, see `KNOWN`), extract, recompile both discs, packs, output folder - already written.
   Add: free disk space check (~3 GB while converting), cancel button, "convert again" cleanup.
4. **Web UI** (`converter/web/index.html`): disc slots, verification, toolchain status, progress, log, result -
   first version done 2026-11-21 against the API of server.py.
5. **Launcher icon**: neutral, own design (no GT artwork) - `scripts/make_eboot_images.py` with a generated icon.
6. **Tests per platform**: Windows 11 (no WSL), macOS (Apple Silicon), Ubuntu 24.04 - fresh user account, only
   Python 3 installed; full conversion of both discs; result runs on the PSP-3000 (attract race, GT mode with
   save, Arcade race, movie). macOS: Gatekeeper quarantine on the downloaded toolchain (`xattr -rd`).
7. **Drafts done 2026-11-26** (README.md, docs/release/reddit-post.md). **Release texts**: README for the GitHub release and the Reddit post (drafts: `docs/release/`), screenshots
   from our own captures. Repository public only after a last check that no game-derived file is tracked
   (`git ls-files` against generated/, extracted*/, build/, *.PAK, *.PRX, Images/).

## Open questions

* **Resolved 2026-11-26** (own Linux build, `TC_HOST=linux`, release toolchain-linux-v1; glibc >= 2.38, older
  systems fall back to the official tarball with an install hint). Was: Linux: the official pspdev tarball needs some shared libraries at run time (libmpc, libmpfr, libgmp, libisl, zstd -
  see scripts/wsl-setup.sh). Desktop distros usually have them; otherwise the converter must say which package to
  install - or we build the Linux toolchain statically like the Windows one.

* Python on Windows: require python.org Python (the .bat explains it), or ship the embeddable Python (~10 MB, PSF
  licence allows it). Shipping it is friendlier.
* **Measured 2026-11-26:** PGO vs plain -O2 on the PSP-3000, attract race frames 300-2999: 26.9 vs 27.9 ms average,
  but 454 vs 380 frames over 33.4 ms - no clear gain, the release stays plain -O2. Was: PGO: the release build is plain -O2 (the profile data contains absolute paths). Measure the difference on the PSP;
  if it matters, rebuild the profile with `-fprofile-prefix-map` so it is portable.
* Simulation output stays bit-identical to the tested build? The release recompiles with the same tool versions, so
  yes - keep a hash list of the generated files from our build to compare against a user's run (debugging aid).
