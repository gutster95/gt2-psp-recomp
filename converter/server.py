"""GT2 PSP converter: a small local web app (Python standard library only) that turns the user's own Gran Turismo 2
discs into a native PSP version and can install it straight onto the PSP's memory stick.

    python converter/server.py          (or the start scripts convert.sh / convert.command / convert.bat)

Steps: the user picks the disc images (.cue, .bin or .bin.ecm) with the system's file dialog -> identify them by their
game code (SHA-1 of EXE + GT2.OVL, profiles.identify) -> extract the file system -> recompile the game code
(python -m gt2psp recomp) -> convert the data (python -m gt2psp pack) -> compile for the PSP with the pspdev toolchain
(scripts/build_psp.py) -> output/PSP/GAME/<folder of the edition> -> optionally install onto the PSP (only files that
differ from the folder already there are copied: an update usually replaces just EBOOT.PBP and the PRX, not the
600 MB data pack). Each supported dump has a disc profile and belongs to an edition (src/gt2psp/profiles.py DISCS /
EDITIONS): GT2 (USA) -> GT2PSP, GT2 Plus (USA) -> GT2PLUS, each folder with its own launcher and save data. Discs of
two editions give two folders. Nothing of the game is contained in this project: every game-derived file is produced
here, on the user's machine, from the user's discs.
"""

from __future__ import annotations

import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402  (converter/toolchain.py)

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))
from gt2psp.disc import Disc  # noqa: E402
from gt2psp.profiles import EDITIONS, PROFILES, code_id, identify  # noqa: E402  (editions, disc profiles)

WEB = Path(__file__).resolve().parent / "web"
OUTPUT = ROOT / "output"
DISCS = ROOT / "build" / "discs"        # .cue sheets written for a bare .bin, images restored from .bin.ecm
SETTINGS = ROOT / "build" / "converter.json"   # remembered PSP path
PORT = int(os.environ.get("GT2PSP_PORT", "8642"))


def bin_of(cue: Path) -> Path | None:
    """The data track file named in a .cue sheet (relative to the sheet, or absolute)."""
    try:
        for line in cue.read_text(errors="replace").splitlines():
            line = line.strip()
            if line.upper().startswith("FILE"):
                name = line[4:].strip()
                if name.startswith('"'):
                    name = name[1:name.index('"', 1)]
                else:
                    name = name.split()[0]
                p = cue.parent / name
                return p if p.exists() else None
    except OSError:
        return None
    return None


def write_cue(bin_path: Path) -> Path:
    """A .cue for a bare .bin (single MODE2/2352 track) in build/discs - the user's folder is never written to."""
    DISCS.mkdir(parents=True, exist_ok=True)
    cue = DISCS / (bin_path.name.rsplit(".", 1)[0] + ".cue")
    cue.write_text(f'FILE "{bin_path.resolve()}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    return cue


class Job:
    """One conversion or installation at a time; the web page polls its state."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.running = False
        self.step = ""
        self.steps: list[dict] = []
        self.log: list[str] = []
        self.done = False
        self.error = ""
        self.progress = 0.0
        self.hashes: dict[str, dict] = {}   # picked file -> check progress / result
        self.folders: list[str] = []     # PSP/GAME folders written by the last conversion
        self.install: dict = {}          # state of the last installation onto the PSP

    def say(self, line: str) -> None:
        with self.lock:
            self.log.append(line.rstrip())
            del self.log[:-400]

    def state(self) -> dict:
        with self.lock:
            return {"running": self.running, "step": self.step, "steps": self.steps, "log": self.log[-120:],
                    "done": self.done, "error": self.error, "progress": self.progress, "hashes": self.hashes,
                    "output": ", ".join(str(OUTPUT / "PSP" / "GAME" / f) for f in self.folders),
                    "folders": self.folders, "install": self.install}


JOB = Job()


# ---- picking and checking discs --------------------------------------------------------------------------------

def prepare(src: Path, key: str) -> Path:
    """The .cue to work with for a picked file: a .cue as it is, a .bin with the .cue next to it (or one written to
    build/discs), a .bin.ecm restored to build/discs first (ECM is common for European dumps)."""
    name = src.name.lower()
    if name.endswith(".cue"):
        if bin_of(src):
            return src
        ecm = src.with_suffix(".bin.ecm")
        if not ecm.exists():
            raise ValueError("The .bin file named in this .cue was not found next to it.")
        src, name = ecm, ecm.name.lower()
    if name.endswith(".ecm"):
        from gt2psp.ecm import decode
        DISCS.mkdir(parents=True, exist_ok=True)
        dst = DISCS / src.name[:-4]
        if not dst.exists():
            def prog(f: float) -> None:
                with JOB.lock:
                    JOB.hashes[key] = {"progress": f * 0.9, "step": "Unpacking ECM"}
            tmp = dst.with_name(dst.name + ".part")
            try:
                decode(src, tmp, prog)
            except (ValueError, OSError):
                tmp.unlink(missing_ok=True)
                raise
            os.replace(tmp, dst)
        return write_cue(dst)
    if name.endswith(".bin") or name.endswith(".img"):
        for cue in src.parent.glob("*.cue"):
            b = bin_of(cue)
            if b and b.resolve() == src.resolve():
                return cue
        return write_cue(src)
    raise ValueError("Pick a .cue, .bin or .bin.ecm file.")


def verify(key: str) -> dict:
    """Check one picked file; the result (with the .cue to convert from) goes to JOB.hashes[key]."""
    with JOB.lock:
        JOB.hashes[key] = {"progress": 0.05}
    src = Path(key)
    try:
        if not src.exists():
            raise ValueError("File not found.")
        cue = prepare(src, key)
        b = bin_of(cue)
        with JOB.lock:
            JOB.hashes[key] = {"progress": 0.95}
        # by the game code (EXE + GT2.OVL), not the whole image: self-patched GT2 Plus discs and other dumps of a
        # supported version differ in data, padding or EDC/ECC but run the same code
        disc = Disc(b)
        try:
            d = identify(disc)
            code = d.code if d else code_id(disc)[0]
        finally:
            disc.close()
        if d:
            ed = EDITIONS[d.edition]
            r = {"ok": True, "disc": PROFILES[d.profile].kind, "profile": d.profile, "edition": d.edition,
                 "folder": ed.folder, "name": d.title, "code": code, "cue": str(cue), "file": src.name}
        else:
            r = {"ok": False, "code": code, "file": src.name,
                 "message": "Unknown game code. Supported: GT2 USA (Simulation Mode Rev 2, Arcade Mode), GT2 Europe "
                            "and Japan (both discs), GT2 Plus Beta v6.1 (USA; Europe on an original European disc's "
                            "code) - one .bin per disc."}
    except (ValueError, OSError) as e:
        r = {"ok": False, "file": src.name, "message": str(e)}
    with JOB.lock:
        JOB.hashes[key] = {"progress": 1.0, **r}
    return r


class Dialogs:
    """Native file dialogs (tkinter). They run on the main thread (macOS requires it); the HTTP server runs in a
    background thread and hands requests over. Without tkinter the page falls back to a path field."""

    def __init__(self) -> None:
        self.q: queue.Queue = queue.Queue()
        try:
            import tkinter  # noqa: F401
            self.available = True
        except ImportError:
            self.available = False

    def ask(self, kind: str, initial: str = "") -> list[str]:
        if not self.available:
            return []
        done: queue.Queue = queue.Queue()
        self.q.put((kind, initial, done))
        return done.get()

    def serve(self) -> None:
        """Main thread: show the dialogs that are asked for (Ctrl+C still works)."""
        while True:
            try:
                kind, initial, done = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                done.put(self._show(kind, initial))
            except Exception:          # a broken Tk install must not kill the converter
                done.put([])

    @staticmethod
    def _show(kind: str, initial: str) -> list[str]:
        import tkinter
        from tkinter import filedialog
        root = tkinter.Tk()
        root.withdraw()
        root.attributes("-topmost", True)    # in front of the browser
        root.update()
        try:
            if kind == "folder":
                p = filedialog.askdirectory(parent=root, title="Your PSP's memory stick (the drive with the PSP folder)",
                                            initialdir=initial or None, mustexist=True)
                return [p] if p else []
            mode = {"sim": "Simulation Mode", "arc": "Arcade Mode"}.get(kind.partition(":")[2], "")
            p = filedialog.askopenfilename(
                parent=root, title=f"Gran Turismo 2 {mode} disc image".replace("  ", " "), initialdir=initial or None,
                filetypes=[("Disc images", "*.cue *.bin *.ecm *.img"), ("All files", "*.*")])
            return [p] if p else []
        finally:
            root.destroy()


DIALOGS = Dialogs()


def load_settings() -> dict:
    try:
        return json.loads(SETTINGS.read_text())
    except (OSError, ValueError):
        return {}


def save_settings(**kw) -> None:
    s = load_settings()
    s.update(kw)
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(s, indent=1))


# ---- conversion ----------------------------------------------------------------------------------------------------

def run(cmd: list[str], env: dict | None = None, cwd: Path = ROOT) -> None:
    JOB.say("$ " + " ".join(cmd))
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors="replace")
    assert p.stdout
    for line in p.stdout:
        JOB.say(line)
    if p.wait() != 0:
        raise RuntimeError(f"step failed: {' '.join(cmd[:4])} ... (exit {p.returncode})")


def paths(r: dict) -> dict[str, Path]:
    """Work files of one disc (verify result: profile + edition - one profile can serve several editions, e.g. the
    EU Arcade disc and GT2 Plus Arcade (Europe), with their own game.h and data pack). The USA discs keep the
    developer layout (generated/, generated/arcade/, build/GT2.PAK, build/GT2A.PAK: scripts/build.sh and the PPSSPP
    scripts use it)."""
    legacy = {"sim": ("extracted", "generated", "GT2.PAK"), "arc": ("extracted-arcade", "generated/arcade", "GT2A.PAK")}
    key = r["profile"] if r["edition"] == "usa" else f"{r['edition']}-{r['profile']}"
    ext, gen, pak = legacy.get(key, (f"extracted-{key}", f"generated/{key}", f"{key}.PAK"))
    return {"ext": ROOT / ext, "gen": ROOT / gen, "pak": ROOT / "build" / pak}


def convert(sim: str | None, arc: str | None, psp: str | None = None) -> None:
    """sim / arc: picked files (keys of JOB.hashes). psp: memory stick to install onto afterwards (None: don't)."""
    py = [sys.executable, "-m", "gt2psp"]
    env = dict(os.environ, PYTHONPATH=str(ROOT / "src"))
    discs = [(d, k) for d, k in (("sim", sim), ("arc", arc)) if k]
    plan = [f"verify-{d}" for d, _ in discs]
    for d, _ in discs:
        plan += [f"extract-{d}", f"recomp-{d}", f"pack-{d}"]
    plan += ["toolchain", "compile", "folder"] + (["install"] if psp else [])
    with JOB.lock:
        JOB.steps = [{"id": s, "state": "wait"} for s in plan]
        JOB.folders = []

    def step(sid: str, i: int):
        with JOB.lock:
            JOB.step = sid
            JOB.progress = i / len(plan)
            for s in JOB.steps:
                if s["id"] == sid:
                    s["state"] = "run"

    def finished(sid: str):
        with JOB.lock:
            for s in JOB.steps:
                if s["id"] == sid:
                    s["state"] = "done"

    i = 0
    found: list[tuple[str, Path, dict]] = []     # (sim/arc, cue, verify result)
    for d, key in discs:
        step(f"verify-{d}", i)
        r = verify(key)
        if not r["ok"] or r["disc"] != d:
            raise RuntimeError(r.get("message") or f"{r.get('file', key)} is the other disc")
        JOB.say(f"{r['file']}: {r['name']} (code {r['code'][:12]}, profile {r['profile']})")
        found.append((d, Path(r["cue"]), r))
        finished(f"verify-{d}")
        i += 1
    for d, cue, r in found:
        prof, w = r["profile"], paths(r)
        step(f"extract-{d}", i); i += 1
        run(py + ["extract", str(cue), "--out", str(w["ext"])], env)
        finished(f"extract-{d}")
        step(f"recomp-{d}", i); i += 1
        cmd = py + ["recomp", "--disc", prof, "--edition", r["edition"], "--files", str(w["ext"] / "files"),
                    "--out", str(w["gen"])]
        run(cmd, env)
        finished(f"recomp-{d}")
        step(f"pack-{d}", i); i += 1
        run(py + ["pack", str(cue), "--out", str(w["pak"])], env)
        finished(f"pack-{d}")
    step("toolchain", i); i += 1
    def dl(f: float) -> None:
        with JOB.lock:
            JOB.progress = (i - 1 + f) / len(plan)
    tc = toolchain.ensure(JOB.say, dl)
    JOB.say(f"PSP toolchain: {tc}")
    finished("toolchain")
    editions: dict[str, list[tuple[str, dict]]] = {}     # discs of one edition share a PSP folder
    for d, _cue, r in found:
        editions.setdefault(r["edition"], []).append((d, r))
    step("compile", i); i += 1
    # XMB artwork: own pictures made by scripts/make_eboot_images.py from Images/ (local, never in git or the release)
    # win over the neutral release icon
    icon = ROOT / "build" / "ICON0.PNG"
    if not icon.exists():
        icon = ROOT / "converter" / "ICON0.PNG"
    pic = ROOT / "build" / "PIC1.PNG"
    for ed_name, members in editions.items():
        ed = EDITIONS[ed_name]
        JOB.say(f"== {ed.title} -> PSP/GAME/{ed.folder}")
        build = [sys.executable, str(ROOT / "scripts" / "build_psp.py"), "--pspdev", str(tc), "--launcher",
                 "--title", ed.title, "--out", str(ROOT / "build" / "release" / ed_name)]
        for d, r in members:
            build += ["--game", f"{d}={paths(r)['gen']}"]
        if icon.exists():
            build += ["--icon", str(icon)]
        if pic.exists():
            build += ["--pic", str(pic)]
        if any(PROFILES[r["profile"]].pal for _d, r in members):
            build.append("--pal")
        run(build, env)
    finished("compile")
    step("folder", i); i += 1
    for ed_name, members in editions.items():
        ed = EDITIONS[ed_name]
        dst = OUTPUT / "PSP" / "GAME" / ed.folder
        dst.mkdir(parents=True, exist_ok=True)
        rel = ROOT / "build" / "release" / ed_name
        files = [(rel / "EBOOT.PBP", "EBOOT.PBP")]
        for d, r in members:
            prx = "GT2SIM.PRX" if d == "sim" else "GT2ARC.PRX"
            files.append((rel / prx, prx))
            files.append((paths(r)["pak"], "GT2.PAK" if d == "sim" else "GT2A.PAK"))
        for f, name in files:
            JOB.say(f"copy {f.name} -> {ed.folder}/{name}")
            shutil.copyfile(f, dst / name)
        if ed_name == "jpn":
            d, r = members[0]
            kanji(paths(r)["ext"] / "files" / PROFILES[r["profile"]].exe, dst / "KANJI.BIN", env)
        with JOB.lock:
            JOB.folders.append(ed.folder)
    finished("folder")
    if psp:
        step("install", i); i += 1
        install(psp, list(JOB.folders))
        finished("install")
    with JOB.lock:
        JOB.progress = 1.0


# The Japanese GT2 draws texts with the PS1 BIOS ROM's kanji font, which the PSP does not have: KANJI.BIN stands in
# for it, built from the free X.Org JIS bitmap font (jis-misc jiskan16.bdf, "from JIS X 9051-1984, by permission to
# use"), downloaded once and checked against a fixed checksum. Without it the kanji stay blank (scripts/make_kanji.py).
KANJI_FONT = ("https://gitlab.freedesktop.org/xorg/font/jis-misc/-/raw/5b54b1798ba76092850fe947d4958409ca61e86d/"
              "jiskan16.bdf", "e9ee1273ce9d6675e8f4a24e49e0d89ec49f003f464d22828af48915dd8f6dbf")


def kanji(exe: Path, out: Path, env: dict) -> None:
    font = ROOT / "tools" / "fonts" / "jiskan16.bdf"
    try:
        if not (font.exists() and toolchain.sha256(font) == KANJI_FONT[1]):
            JOB.say("downloading the JIS kanji bitmap font (1 MB)")
            font.parent.mkdir(parents=True, exist_ok=True)
            import urllib.request
            tmp = font.with_suffix(".part")
            with urllib.request.urlopen(KANJI_FONT[0], timeout=60) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
            if toolchain.sha256(tmp) != KANJI_FONT[1]:
                tmp.unlink(missing_ok=True)
                raise RuntimeError("font download damaged")
            os.replace(tmp, font)
        run([sys.executable, str(ROOT / "scripts" / "make_kanji.py"), str(exe), str(font), str(out)], env)
    except Exception as e:      # optional: the game runs without it, the kanji are blank
        JOB.say(f"kanji font skipped ({e}) - Japanese kanji texts will be blank")


# ---- installing onto the PSP --------------------------------------------------------------------------------------

def psp_root(p: str) -> Path:
    """The memory stick root for a path the user gave: the drive itself, its PSP folder or PSP/GAME."""
    r = Path(p).expanduser()
    if r.name.upper() == "GAME" and r.parent.name.upper() == "PSP":
        return r.parent.parent
    if r.name.upper() == "PSP":
        return r.parent
    return r


def detect_psp() -> list[str]:
    """Mounted memory sticks: drives / volumes with a PSP folder."""
    cands: list[Path] = []
    if os.name == "nt":
        import string
        cands = [Path(f"{c}:\\") for c in string.ascii_uppercase[3:]]
    else:
        user = os.environ.get("USER", "")
        for base in ("/Volumes", f"/media/{user}", "/media", f"/run/media/{user}", "/mnt"):
            try:
                cands += [p for p in Path(base).iterdir() if p.is_dir()]
            except OSError:
                pass
    out = []
    for c in cands:
        try:
            if (c / "PSP").is_dir():
                out.append(str(c))
        except OSError:
            pass
    return out


def same_file(a: Path, b: Path) -> bool:
    """b (on the PSP) already holds a's content: equal size, and equal bytes for small files / equal samples spread
    over the whole file for the data packs (reading 700 MB back from a memory stick would take minutes)."""
    try:
        if not b.is_file() or a.stat().st_size != b.stat().st_size:
            return False
        size = a.stat().st_size
        with open(a, "rb") as fa, open(b, "rb") as fb:
            if size <= 32 << 20:
                while True:
                    x, y = fa.read(1 << 20), fb.read(1 << 20)
                    if x != y:
                        return False
                    if not x:
                        return True
            for k in range(65):           # 65 x 64 KB from the start to the end
                off = min(size - (64 << 10), (size - (64 << 10)) * k // 64)
                fa.seek(off)
                fb.seek(off)
                if hashlib.sha1(fa.read(64 << 10)).digest() != hashlib.sha1(fb.read(64 << 10)).digest():
                    return False
        return True
    except OSError:
        return False


def install(psp: str, folders: list[str]) -> None:
    """Copy output/PSP/GAME/<folder> onto the memory stick. Only files that differ are written (a new runtime build:
    EBOOT.PBP and the PRX; the data pack only when it changed); everything else in the PSP folder (the other disc's
    files, settings.txt) and the save data stay untouched. Files are written under a temporary name first."""
    root = psp_root(psp)
    if not root.is_dir():
        raise RuntimeError(f"PSP not found at {root} - is it connected in USB mode?")
    if not (root / "PSP").is_dir():
        raise RuntimeError(f"{root} has no PSP folder - pick the memory stick's drive.")
    work: list[tuple[Path, Path]] = []
    for folder in folders:
        src = OUTPUT / "PSP" / "GAME" / folder
        if not src.is_dir():
            raise RuntimeError(f"Nothing to install for {folder} - convert first.")
        dst = root / "PSP" / "GAME" / folder
        new = not dst.exists()
        JOB.say(f"== {folder}: {'new folder' if new else 'updating the folder already on the PSP'} ({dst})")
        for f in sorted(src.iterdir()):
            if not f.is_file():
                continue
            t = dst / f.name
            if same_file(f, t):
                JOB.say(f"   {f.name}: unchanged")
            else:
                work.append((f, t))
    need = sum(f.stat().st_size - (t.stat().st_size if t.exists() else 0) for f, t in work)
    free = shutil.disk_usage(root).free
    if need > free - (8 << 20):
        raise RuntimeError(f"Not enough space on the memory stick: {need / 2**20:.0f} MB more needed, "
                           f"{free / 2**20:.0f} MB free.")
    total = sum(f.stat().st_size for f, _ in work) or 1
    done = 0
    t0 = time.time()
    for f, t in work:
        t.parent.mkdir(parents=True, exist_ok=True)
        tmp = t.with_name(t.name + ".part")
        JOB.say(f"   {f.name}: copying {f.stat().st_size / 2**20:.1f} MB")
        with open(f, "rb") as fi, open(tmp, "wb") as fo:
            while chunk := fi.read(4 << 20):
                fo.write(chunk)
                done += len(chunk)
                with JOB.lock:
                    JOB.install = {"running": True, "progress": done / total, "file": f.name}
            fo.flush()
            os.fsync(fo.fileno())
        os.replace(tmp, t)
    secs = time.time() - t0
    JOB.say(f"Installed: {len(work)} file(s), {total / 2**20:.0f} MB in {secs:.0f} s" if work
            else "Installed: everything was already up to date.")
    with JOB.lock:
        JOB.install = {"running": False, "progress": 1.0, "done": True, "copied": [f.name for f, _ in work],
                       "target": str(root / "PSP" / "GAME")}


def start(fn, *args) -> bool:
    def work():
        try:
            fn(*args)
            with JOB.lock:
                JOB.done = True
        except Exception as e:       # reported to the page
            JOB.say(f"ERROR: {e}")
            with JOB.lock:
                JOB.error = str(e)
                if JOB.install.get("running"):
                    JOB.install = {"running": False, "error": str(e)}
        finally:
            with JOB.lock:
                JOB.running = False
                JOB.step = ""

    with JOB.lock:
        if JOB.running:
            return False
        JOB.running, JOB.done, JOB.error, JOB.log, JOB.progress = True, False, "", [], 0.0
        JOB.install = {}
    threading.Thread(target=work, daemon=True).start()
    return True


def install_only(psp: str, folders: list[str]) -> None:
    with JOB.lock:
        JOB.steps = [{"id": "install", "state": "run"}]
        JOB.step = "install"
        JOB.install = {"running": True, "progress": 0.0}
    install(psp, folders)
    with JOB.lock:
        JOB.steps = [{"id": "install", "state": "done"}]
        JOB.progress = 1.0


# ---- web page ------------------------------------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a) -> None:      # quiet console
        pass

    def send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def js(self, obj) -> None:
        self.send(200, json.dumps(obj).encode(), "application/json")

    def do_GET(self) -> None:
        if self.path == "/" or self.path == "/index.html":
            self.send(200, (WEB / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/api/info":
            out = OUTPUT / "PSP" / "GAME"
            built = sorted(p.name for p in out.iterdir() if p.is_dir()) if out.is_dir() else []
            self.js({"toolchain": toolchain.state(), "dialogs": DIALOGS.available, "psp": load_settings().get("psp", ""),
                     "built": built})
        elif self.path == "/api/status":
            self.js(JOB.state())
        else:
            self.send(404, b"not found", "text/plain")

    def do_POST(self) -> None:
        n = int(self.headers.get("Content-Length", "0") or 0)
        try:
            req = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            req = {}
        if self.path == "/api/pick":
            kind = "folder" if req.get("kind") == "folder" else f"file:{req.get('which', '')}"
            initial = load_settings().get("psp" if kind == "folder" else "discdir", "")
            picked = DIALOGS.ask(kind, initial)
            if picked and kind != "folder":
                save_settings(discdir=str(Path(picked[0]).parent))
            self.js({"paths": picked, "available": DIALOGS.available})
        elif self.path == "/api/add":
            raw = str(req.get("path", "")).strip().strip('"')
            if not raw:
                return self.js({"key": None})
            key = str(Path(raw))
            threading.Thread(target=verify, args=(key,), daemon=True).start()
            self.js({"key": key})
        elif self.path == "/api/psp-detect":
            self.js({"found": detect_psp()})
        elif self.path == "/api/convert":
            psp = req.get("psp") or None
            if psp:
                save_settings(psp=psp)
            ok = start(convert, req.get("sim"), req.get("arc"), psp)
            self.js({"started": ok})
        elif self.path == "/api/install":
            psp = req.get("psp", "")
            save_settings(psp=psp)
            folders = req.get("folders") or []
            ok = start(install_only, psp, folders)
            self.js({"started": ok})
        elif self.path == "/api/open-output":
            p = OUTPUT / "PSP" / "GAME" / JOB.folders[0] if len(JOB.folders) == 1 else OUTPUT / "PSP" / "GAME"
            try:
                if sys.platform.startswith("win"):
                    os.startfile(p)          # noqa: S606 (local folder)
                elif sys.platform == "darwin":
                    subprocess.Popen(["open", str(p)])
                else:
                    subprocess.Popen(["xdg-open", str(p)])
            except OSError:
                pass
            self.js({"ok": True})
        else:
            self.send(404, b"not found", "text/plain")


def main() -> None:
    if os.name == "nt":
        # no modal Windows error dialogs (e.g. "bad image") from the toolchain: they would stall the conversion.
        # Child processes inherit the error mode. SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x8000)
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    url = f"http://127.0.0.1:{PORT}/"
    print(f"GT2 PSP converter running at {url}  (Ctrl+C to stop)")
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        DIALOGS.serve()              # file dialogs on the main thread
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
