"""GT2 PSP converter: a small local web app (Python standard library only) that turns the user's own Gran Turismo 2
discs into a ready-to-copy PSP folder.

    python converter/server.py          (or the start scripts convert.sh / convert.command / convert.bat)

Steps: verify the .bin images (SHA-1 of the known good dumps) -> extract the file system -> recompile the game code
(python -m gt2psp recomp) -> convert the data (python -m gt2psp pack) -> compile for the PSP with the pspdev toolchain
(scripts/build_psp.py) -> output/PSP/GAME/<folder of the edition>. Each supported dump has a disc profile and belongs to an
edition (src/gt2psp/profiles.py DISCS / EDITIONS): GT2 (USA) -> GT2PSP, GT2 Plus (USA) -> GT2PLUS, each folder with its
own launcher and save data. Discs of two editions give two folders. Nothing of the game is contained in this project: every
game-derived file is produced here, on the user's machine, from the user's discs.
"""

from __future__ import annotations

import hashlib
import json
import os
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
from gt2psp.profiles import DISCS, EDITIONS, PROFILES  # noqa: E402  (supported dumps, editions, disc profiles)

WEB = Path(__file__).resolve().parent / "web"
INPUT = ROOT / "input"
OUTPUT = ROOT / "output"
PORT = int(os.environ.get("GT2PSP_PORT", "8642"))

# .bin sizes of the supported dumps: which card a disc goes to before it is hashed
SIZES = {691850208: "sim", 729423408: "arc", 639694608: "sim", 739809840: "arc", 710226384: "sim", 667965648: "arc",
         669522672: "arc", 698057136: "sim", 725234496: "arc"}


def bin_of(cue: Path) -> Path | None:
    """The data track file named in a .cue sheet."""
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


class Job:
    """One conversion at a time; the web page polls its state."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.running = False
        self.step = ""
        self.steps: list[dict] = []
        self.log: list[str] = []
        self.done = False
        self.error = ""
        self.progress = 0.0
        self.hashes: dict[str, dict] = {}
        self.folders: list[str] = []     # PSP/GAME folders written by the last conversion

    def say(self, line: str) -> None:
        with self.lock:
            self.log.append(line.rstrip())
            del self.log[:-400]

    def state(self) -> dict:
        with self.lock:
            return {"running": self.running, "step": self.step, "steps": self.steps, "log": self.log[-120:],
                    "done": self.done, "error": self.error, "progress": self.progress, "hashes": self.hashes,
                    "output": ", ".join(str(OUTPUT / "PSP" / "GAME" / f) for f in self.folders),
                    "folders": self.folders}


JOB = Job()


def sha1_with_progress(path: Path, key: str) -> str:
    h = hashlib.sha1()
    total = path.stat().st_size or 1
    done = 0
    with open(path, "rb") as f:
        while chunk := f.read(4 << 20):
            h.update(chunk)
            done += len(chunk)
            with JOB.lock:
                JOB.hashes[key] = {"progress": done / total}
    return h.hexdigest()


def verify(cue: Path) -> dict:
    b = bin_of(cue)
    if not b and ecm_for(cue):
        # an ECM-compressed image (.bin.ecm, common for European dumps): restore the .bin and write a .cue first
        from gt2psp.ecm import decode
        src = ecm_for(cue)
        dst = src.with_suffix("")
        def prog(f: float) -> None:
            with JOB.lock:
                JOB.hashes[str(cue)] = {"progress": f * 0.5}
        try:
            decode(src, dst, prog)
        except (ValueError, OSError) as e:
            dst.unlink(missing_ok=True)
            r = {"ok": False, "message": f"Could not unpack {src.name}: {e}"}
            with JOB.lock:
                JOB.hashes[str(cue)] = {"progress": 1.0, **r}
            return r
        if not cue.exists():
            cue.write_text(f'FILE "{dst.name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        b = bin_of(cue)
    if not b:
        return {"ok": False, "message": "The .bin file named in this .cue was not found next to it."}
    digest = sha1_with_progress(b, str(cue))
    d = DISCS.get(digest)
    if d:
        ed = EDITIONS[d.edition]
        r = {"ok": True, "disc": PROFILES[d.profile].kind, "profile": d.profile, "edition": d.edition,
             "folder": ed.folder, "name": d.title, "sha1": digest}
    else:
        r = {"ok": False, "sha1": digest,
             "message": "Unknown dump. Supported: GT2 USA (Simulation Mode Rev 2, Arcade Mode), GT2 Europe and Japan "
                        "(both discs), GT2 Plus Beta v6.1 (USA, Arcade Europe) - one .bin per disc."}
    with JOB.lock:
        JOB.hashes[str(cue)] = {"progress": 1.0, **r}
    return r


def ecm_for(cue: Path) -> Path | None:
    """The .bin.ecm a not yet existing .cue stands for (scan lists ECM images under the .cue they will get)."""
    e = cue.with_suffix(".bin.ecm")
    return e if e.exists() else None


def scan() -> list[dict]:
    INPUT.mkdir(exist_ok=True)
    out = []
    for cue in sorted(INPUT.rglob("*.cue")):
        b = bin_of(cue)
        size = b.stat().st_size if b else 0
        e = None if b else ecm_for(cue)          # .cue next to a .bin.ecm: unpacked when it is checked
        out.append({"cue": str(cue), "name": cue.stem + (" (ECM)" if e else ""), "bin": str(b or e) if b or e else None,
                    "size": size, "guess": SIZES.get(size)})
    for e in sorted(INPUT.rglob("*.bin.ecm")):
        cue = e.with_suffix("").with_suffix(".cue")
        if not cue.exists():       # unpacked when it is checked
            out.append({"cue": str(cue), "name": cue.stem + " (ECM)", "bin": str(e), "size": 0, "guess": None})
    return out


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


def convert(sim: str | None, arc: str | None) -> None:
    py = [sys.executable, "-m", "gt2psp"]
    env = dict(os.environ, PYTHONPATH=str(ROOT / "src"))
    discs = [(d, Path(c)) for d, c in (("sim", sim), ("arc", arc)) if c]
    plan = [f"verify-{d}" for d, _ in discs]
    for d, _ in discs:
        plan += [f"extract-{d}", f"recomp-{d}", f"pack-{d}"]
    plan += ["toolchain", "compile", "folder"]
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
    for d, cue in discs:
        step(f"verify-{d}", i)
        r = verify(cue)
        if not r["ok"] or r["disc"] != d:
            raise RuntimeError(r.get("message") or f"{cue.name} is the other disc")
        JOB.say(f"{cue.name}: {r['name']} (sha1 {r['sha1']}, profile {r['profile']})")
        found.append((d, cue, r))
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
    icon = ROOT / "converter" / "ICON0.PNG"
    for ed_name, members in editions.items():
        ed = EDITIONS[ed_name]
        JOB.say(f"== {ed.title} -> PSP/GAME/{ed.folder}")
        build = [sys.executable, str(ROOT / "scripts" / "build_psp.py"), "--pspdev", str(tc), "--launcher",
                 "--title", ed.title, "--out", str(ROOT / "build" / "release" / ed_name)]
        for d, r in members:
            build += ["--game", f"{d}={paths(r)['gen']}"]
        if icon.exists():
            build += ["--icon", str(icon)]
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
        with JOB.lock:
            JOB.folders.append(ed.folder)
    finished("folder")
    with JOB.lock:
        JOB.progress = 1.0


def start(sim: str | None, arc: str | None) -> None:
    def work():
        try:
            convert(sim, arc)
            with JOB.lock:
                JOB.done = True
        except Exception as e:       # reported to the page
            JOB.say(f"ERROR: {e}")
            with JOB.lock:
                JOB.error = str(e)
        finally:
            with JOB.lock:
                JOB.running = False
                JOB.step = ""

    with JOB.lock:
        if JOB.running:
            return
        JOB.running, JOB.done, JOB.error, JOB.log, JOB.progress = True, False, "", [], 0.0
    threading.Thread(target=work, daemon=True).start()


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
        elif self.path == "/api/scan":
            self.js({"discs": scan(), "input": str(INPUT), "toolchain": toolchain.state()})
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
        if self.path == "/api/verify":
            cue = Path(req.get("cue", ""))
            if cue.suffix.lower() != ".cue" or not (cue.exists() or ecm_for(cue)):
                return self.js({"ok": False, "message": "no such .cue file"})
            threading.Thread(target=verify, args=(cue,), daemon=True).start()
            self.js({"started": True})
        elif self.path == "/api/convert":
            start(req.get("sim"), req.get("arc"))
            self.js({"started": True})
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
    INPUT.mkdir(exist_ok=True)
    if os.name == "nt":
        # no modal Windows error dialogs (e.g. "bad image") from the toolchain: they would stall the conversion.
        # Child processes inherit the error mode. SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x8000)
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    url = f"http://127.0.0.1:{PORT}/"
    print(f"GT2 PSP converter running at {url}  (Ctrl+C to stop)")
    print(f"Put your .cue/.bin files into: {INPUT}")
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
