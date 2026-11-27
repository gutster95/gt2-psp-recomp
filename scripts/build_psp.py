"""Build the PSP binaries without bash/cmake/make (release plan step 1): the same compiler and linker calls as
runtime/CMakeLists.txt, runtime/launcher/CMakeLists.txt and third_party/me-custom-core (with pspdev.cmake), driven
from Python so the converter works the same on Windows, macOS and Linux.

    python scripts/build_psp.py --pspdev <toolchain dir> [--game sim|arc[=<generated dir>] ...] [--launcher]
                                [--title "Gran Turismo 2"] [--out build/release]

Outputs: <out>/GT2SIM.PRX, <out>/GT2ARC.PRX, <out>/EBOOT.PBP (launcher). Work files: <work> (default build/psp-work).
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PROJ = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""
RELEASE_GEN = "-O2 -DGT2_NO_TRACE -DGT2_GTE_OUTLINE"     # as scripts/dev/pgo.sh (release), without the profile
LIBS = ["-lpspsdk", "-lpspgu", "-lpsputility", "-lpspaudio", "-lpspdebug", "-lpspdisplay", "-lpspge", "-lpsprtc",
        "-lpspctrl", "-lpsppower"]


class Toolchain:
    def __init__(self, root: Path) -> None:
        self.root = root
        if not (root / "bin" / f"psp-gcc{EXE}").exists():
            raise SystemExit(f"no PSP toolchain in {root} (bin/psp-gcc{EXE} missing)")
        self.inc = [f"-I{root / 'psp' / 'include'}", f"-I{root / 'psp' / 'sdk' / 'include'}"]
        self.defs = ["-DPSP", "-D_PSP_FW_VERSION=600", "-D__PSP__"]
        self.libdirs = [f"-L{root / 'lib'}", f"-L{root / 'psp' / 'lib'}", f"-L{root / 'psp' / 'sdk' / 'lib'}"]
        self.sdklib = root / "psp" / "sdk" / "lib"

    def tool(self, name: str) -> str:
        return str(self.root / "bin" / f"{name}{EXE}")


def run(cmd: list[str], cwd: Path | None = None, out: Path | None = None) -> None:
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        sys.stderr.write(" ".join(shlex.quote(c) for c in cmd) + "\n" + r.stdout + r.stderr)
        raise SystemExit(f"failed: {Path(cmd[0]).name} (exit {r.returncode})")
    if out is not None:
        out.write_text(r.stdout, encoding="utf-8", newline="\n")


def compile_all(tc: Toolchain, jobs: list[tuple[Path, Path, list[str]]], threads: int, deps: float = 0.0) -> None:
    """jobs: (source, object, flags). Skips objects newer than their source and than deps (the newest header they may
    include; simple incremental build)."""
    todo = [(s, o, f) for s, o, f in jobs if not (o.exists() and o.stat().st_mtime > max(s.stat().st_mtime, deps)
                                                    and Path(str(o) + ".flags").exists()
                                                    and Path(str(o) + ".flags").read_text() == " ".join(f))]
    for _, o, _ in todo:
        o.parent.mkdir(parents=True, exist_ok=True)

    def one(job):
        s, o, f = job
        run([tc.tool("psp-gcc"), *f, "-o", str(o), "-c", str(s)])
        Path(str(o) + ".flags").write_text(" ".join(f))
        return s.name

    done = 0
    with ThreadPoolExecutor(max_workers=threads) as ex:
        for name in ex.map(one, todo):
            done += 1
            print(f"  [{done}/{len(todo)}] {name}", flush=True)


def me_library(tc: Toolchain, work: Path, threads: int) -> Path:
    """third_party/me-custom-core: libme-core.a + libme-core-lib.a (with the kcall kernel PRX embedded)."""
    src = PROJ / "third_party" / "me-custom-core"
    w = work / "me"
    w.mkdir(parents=True, exist_ok=True)
    if (w / "libme-core-lib.a").exists() and (w / "libme-core.a").exists():
        return w
    print("== Media Engine library")
    # kernel helper PRX (kernel/CMakeLists.txt)
    run([tc.tool("psp-build-exports"), "-b", str(src / "kernel" / "exports.exp")], out=w / "exports.c")
    kflags = [*tc.defs, *tc.inc, "-DPSP", "-Wno-narrowing", "-fsingle-precision-constant", "-Os", "-G0", "-Wall"]
    compile_all(tc, [(src / "kernel" / "main.c", w / "kcall_main.o", kflags),
                     (w / "exports.c", w / "kcall_exports.o", kflags)], threads)
    run([tc.tool("psp-gcc"), *tc.inc, "-DPSP", *tc.libdirs, "-Wl,-zmax-page-size=128", "-nostartfiles", "-Wl,-q",
         f"-Wl,-T{tc.sdklib / 'linkfile.prx'}", "-Wl,-zmax-page-size=128", "-Wl,--strip-all",
         str(w / "kcall_main.o"), str(w / "kcall_exports.o"), "-o", str(w / "kcall"), "-lpspkernel"])
    run([tc.tool("psp-prxgen"), str(w / "kcall"), str(w / "kcall.prx")])
    run([tc.tool("psp-fixup-imports"), str(w / "kcall.prx")])
    # import stubs and the embedded PRX (xxd -i + sed in the upstream CMakeLists)
    run([tc.tool("psp-build-exports"), "-s", str(src / "kernel" / "exports.exp")], cwd=w)
    data = (w / "kcall.prx").read_bytes()
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 12]) for i in range(0, len(data), 12)]
    (w / "embedded.c").write_text("unsigned char embedded_kcall[] = {\n  " + ",\n  ".join(rows) +
                                  f"\n}};\nunsigned int embedded_kcall_len = {len(data)};\n", newline="\n")
    flags = [*tc.defs, f"-I{src}", *tc.inc, "-DPSP", "-Os", "-G0", "-Wall", "-fno-pic"]
    core = [(src / "me-core-custom.c", w / "me-core-custom.o"), (src / "me-core-mapping.c", w / "me-core-mapping.o"),
            (src / "context.S", w / "context.o")]
    lib = [(src / "me-lib.c", w / "me-lib.o"), (w / "embedded.c", w / "embedded.o"), (w / "kcall.S", w / "kcall.o")]
    compile_all(tc, [(s, o, flags) for s, o in core + lib], threads)
    for name, objs in (("libme-core.a", core), ("libme-core-lib.a", lib)):
        (w / name).unlink(missing_ok=True)
        run([tc.tool("psp-ar"), "qc", str(w / name), *[str(o) for _, o in objs]])
        run([tc.tool("psp-ranlib"), str(w / name)])
    # the runtime includes <me-core-mapper/...>
    inc = w / "include" / "me-core-mapper"
    inc.mkdir(parents=True, exist_ok=True)
    for h in list(src.glob("*.h")) + list((src / "kernel").glob("*.h")):
        dst = inc / h.relative_to(src)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(h.read_bytes())
    return w


def runtime(tc: Toolchain, game: str, gen: Path, work: Path, me: Path, gen_flags: str, rt_flags: str,
            threads: int) -> Path:
    """runtime/CMakeLists.txt: runtime + generated code of one disc (gen: its recompiled code with game.h) -> PRX."""
    if not (gen / "tables.c").exists() or not (gen / "game.h").exists():
        raise SystemExit(f"no recompiled code in {gen} (run: python -m gt2psp recomp ...)")
    w = (work / f"{game}-{gen.name}").resolve()   # one object dir per disc: objects of another disc never mix in
    print(f"== runtime ({'Arcade' if game == 'arc' else 'Simulation'} Mode, {gen.name})")
    base = [*tc.defs, f"-I{PROJ / 'runtime' / 'include'}", f"-I{PROJ / 'runtime' / 'src'}", f"-I{gen}",
            f"-I{me / 'include'}", *tc.inc, "-DPSP", "-O2", "-G0", "-ffunction-sections", "-fno-strict-aliasing",
            "-Wall", "-Wno-unused-label", "-Wno-unused-variable"]
    rt_extra = shlex.split(rt_flags)
    jobs = []
    for s in sorted((PROJ / "runtime" / "src").glob("*.c")):
        extra = rt_extra if s.name in ("ge.c", "gpu.c", "io.c", "gte.c", "core.c") else []
        jobs.append((s, w / "src" / (s.name + ".obj"), base + extra))
    for s in sorted(gen.glob("*.c")):
        # the linker script places "*generated/*.c.obj" as a block (same object names as the CMake build)
        jobs.append((s, w / "generated" / (s.name + ".obj"), base + ["-w", *shlex.split(gen_flags)]))
    # the generated files are large: start them first so the pool stays busy
    jobs.sort(key=lambda j: -j[0].stat().st_size)
    headers = [*(PROJ / "runtime").glob("include/*.h"), *(PROJ / "runtime").glob("src/*.h"), *gen.glob("*.h")]
    compile_all(tc, jobs, threads, max(h.stat().st_mtime for h in headers))
    # linker script with the hot functions first (scripts/make_linkfile.py)
    run([sys.executable, str(PROJ / "scripts" / "make_linkfile.py")])
    # same input order as CMake (runtime sources, then generated), relative with "/" so the patterns match on Windows
    objs = [o.relative_to(w).as_posix() for _, o, _ in sorted(jobs, key=lambda j: (j[1].parent.name != "src", j[1].name))]
    elf = w / "gt2psp"
    print("  link")
    run([tc.tool("psp-gcc"), *tc.inc, "-DPSP", *tc.libdirs, "-Wl,-zmax-page-size=128",
         f"-specs={tc.sdklib / 'prxspecs'}", f"-Wl,-q,-T{PROJ / 'runtime' / 'ld' / 'linkfile.prx'}",
         str(tc.sdklib / "prxexports.o"), *objs, "-o", str(elf),
         str(me / "libme-core.a"), str(me / "libme-core-lib.a"), *LIBS], cwd=w)
    run([tc.tool("psp-fixup-imports"), str(elf)])
    run([tc.tool("psp-prxgen"), str(elf), str(w / "gt2psp.prx")])
    return w / "gt2psp.prx"


def launcher(tc: Toolchain, work: Path, out: Path, icon: Path | None, pic: Path | None, title: str,
             pal: bool = False) -> Path:
    """runtime/launcher/CMakeLists.txt -> EBOOT.PBP (start menu)."""
    print("== launcher")
    w = work / "launcher"
    w.mkdir(parents=True, exist_ok=True)
    flags = [*tc.defs, *tc.inc, "-DPSP", "-O2", "-Wall", f'-DGT2_LAUNCHER_TITLE="{title.upper()}"',
             *(["-DGT2_LAUNCHER_PAL"] if pal else [])]
    compile_all(tc, [(PROJ / "runtime" / "launcher" / "main.c", w / "main.o", flags)], 1)
    elf = w / "gt2launcher"
    run([tc.tool("psp-gcc"), *tc.inc, "-DPSP", *tc.libdirs, "-Wl,-zmax-page-size=128",
         f"-specs={tc.sdklib / 'prxspecs'}", f"-Wl,-q,-T{tc.sdklib / 'linkfile.prx'}", str(tc.sdklib / "prxexports.o"),
         str(w / "main.o"), "-o", str(elf), "-lpspdebug", "-lpspdisplay", "-lpspge", "-lpspctrl"])
    run([tc.tool("psp-fixup-imports"), str(elf)])
    run([tc.tool("psp-prxgen"), str(elf), str(w / "gt2launcher.prx")])
    run([tc.tool("mksfoex"), "-d", "MEMSIZE=2", "-s", "APP_VER=", title, str(w / "PARAM.SFO")])
    eboot = out / "EBOOT.PBP"
    run([tc.tool("pack-pbp"), str(eboot), str(w / "PARAM.SFO"), str(icon) if icon else "NULL", "NULL", "NULL",
         str(pic) if pic else "NULL", "NULL", str(w / "gt2launcher.prx"), "NULL"])
    return eboot


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pspdev", default=os.environ.get("PSPDEV", ""), help="toolchain root (contains bin/psp-gcc)")
    ap.add_argument("--game", action="append",
                    help="runtime(s) to build: sim or arc, optionally =<generated dir> (default: generated/, "
                         "generated/arcade/)")
    ap.add_argument("--title", default="Gran Turismo 2", help="XMB title of the launcher")
    ap.add_argument("--pal", action="store_true", help="PAL edition: the launcher shows 25 / 50 fps")
    ap.add_argument("--launcher", action="store_true", help="also build the launcher EBOOT.PBP")
    ap.add_argument("--out", default=str(PROJ / "build" / "release"))
    ap.add_argument("--work", default=str(PROJ / "build" / "psp-work"))
    ap.add_argument("--gen-flags", default=RELEASE_GEN, help="extra flags for the generated code")
    ap.add_argument("--rt-flags", default="", help="extra flags for ge.c gpu.c io.c gte.c core.c")
    ap.add_argument("--icon", help="ICON0.PNG for the launcher")
    ap.add_argument("--pic", help="PIC1.PNG for the launcher")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if not a.pspdev:
        raise SystemExit("--pspdev (or PSPDEV) is needed")
    tc = Toolchain(Path(a.pspdev).resolve())
    out, work = Path(a.out).resolve(), Path(a.work).resolve()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    if a.game:
        me = me_library(tc, work, a.jobs)
        for spec in a.game:
            g, _, gdir = spec.partition("=")
            if g not in ("sim", "arc"):
                raise SystemExit(f"--game {spec}: sim or arc")
            gen = Path(gdir).resolve() if gdir else PROJ / "generated" / ("arcade" if g == "arc" else "")
            prx = runtime(tc, g, gen, work, me, a.gen_flags, a.rt_flags, a.jobs)
            dst = out / ("GT2ARC.PRX" if g == "arc" else "GT2SIM.PRX")
            dst.write_bytes(prx.read_bytes())
            print(f"  -> {dst} ({dst.stat().st_size} bytes)")
    if a.launcher:
        e = launcher(tc, work, out, Path(a.icon) if a.icon else None, Path(a.pic) if a.pic else None, a.title,
                     a.pal)
        print(f"  -> {e} ({e.stat().st_size} bytes)")
    print(f"== done in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
