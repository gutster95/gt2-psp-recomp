"""GT2 PSP converter: a small local web app (Python standard library only) that turns the user's own Gran Turismo 2
discs into a ready-to-copy PSP folder.

    python converter/server.py          (or the start scripts convert.sh / convert.command / convert.bat)

Steps: verify the .bin images (SHA-1 of the known good dumps) -> extract the file system -> recompile the game code
(python -m gt2psp recomp) -> convert the data (python -m gt2psp pack) -> compile for the PSP with the pspdev toolchain
(scripts/converter-build.sh; release: scripts/build_psp.py, no bash) -> output/PSP/GAME/GT2PSP. Nothing of the game is contained in this project: every
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

ROOT = Path(__file__).resolve().parent.parent
WEB = Path(__file__).resolve().parent / "web"
INPUT = ROOT / "input"
OUTPUT = ROOT / "output"
PORT = int(os.environ.get("GT2PSP_PORT", "8642"))

# SHA-1 of the whole .bin (single track, MODE2/2352) of the supported USA discs
KNOWN = {
    "c4813703d2ad440ed36670c1ac2c1a66c9dc673d": ("sim", "Gran Turismo 2 (USA) Simulation Mode, Rev 2"),
    "a617b948e23c198a4d0eb521b874d0f563987d98": ("arc", "Gran Turismo 2 (USA) Arcade Mode"),
}
SIZES = {691850208: "sim", 729423408: "arc"}   # quick hint before hashing


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

    def say(self, line: str) -> None:
        with self.lock:
            self.log.append(line.rstrip())
            del self.log[:-400]

    def state(self) -> dict:
        with self.lock:
            return {"running": self.running, "step": self.step, "steps": self.steps, "log": self.log[-120:],
                    "done": self.done, "error": self.error, "progress": self.progress, "hashes": self.hashes,
                    "output": str(OUTPUT / "PSP" / "GAME" / "GT2PSP")}


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
    if not b:
        return {"ok": False, "message": "The .bin file named in this .cue was not found next to it."}
    digest = sha1_with_progress(b, str(cue))
    if digest in KNOWN:
        disc, name = KNOWN[digest]
        r = {"ok": True, "disc": disc, "name": name, "sha1": digest}
    else:
        r = {"ok": False, "sha1": digest,
             "message": "Unknown dump. Supported are the USA discs: Simulation Mode Rev 2 and Arcade Mode "
                        "(single .bin per disc, Redump-style)."}
    with JOB.lock:
        JOB.hashes[str(cue)] = {"progress": 1.0, **r}
    return r


def scan() -> list[dict]:
    INPUT.mkdir(exist_ok=True)
    out = []
    for cue in sorted(INPUT.rglob("*.cue")):
        b = bin_of(cue)
        size = b.stat().st_size if b else 0
        out.append({"cue": str(cue), "name": cue.stem, "bin": str(b) if b else None, "size": size,
                    "guess": SIZES.get(size)})
    return out


def toolchain_state() -> dict:
    """PSP toolchain for the compile step. Release plan (docs/RELEASE-PLAN.md) step 2/3: pinned pspdev downloads
    (Linux/macOS) and our own Windows build; until then only an existing local toolchain is detected."""
    for p in (ROOT / "tools" / "pspdev", Path(os.environ.get("PSPDEV", "/nonexistent"))):
        if (p / "bin").exists():
            return {"ok": True, "path": str(p), "message": f"PSP toolchain found ({p})"}
    return {"ok": False, "message": "The PSP toolchain is downloaded on first use (coming with the release build)."}


def run(cmd: list[str], env: dict | None = None, cwd: Path = ROOT) -> None:
    JOB.say("$ " + " ".join(cmd))
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors="replace")
    assert p.stdout
    for line in p.stdout:
        JOB.say(line)
    if p.wait() != 0:
        raise RuntimeError(f"step failed: {' '.join(cmd[:4])} ... (exit {p.returncode})")


def convert(sim: str | None, arc: str | None) -> None:
    py = [sys.executable, "-m", "gt2psp"]
    env = dict(os.environ, PYTHONPATH=str(ROOT / "src"))
    plan = []
    discs = [(d, Path(c)) for d, c in (("sim", sim), ("arc", arc)) if c]
    for d, _ in discs:
        plan += [f"verify-{d}"]
    for d, _ in discs:
        plan += [f"extract-{d}", f"recomp-{d}", f"pack-{d}"]
    plan += ["toolchain", "compile", "folder"]
    with JOB.lock:
        JOB.steps = [{"id": s, "state": "wait"} for s in plan]

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
    for d, cue in discs:
        step(f"verify-{d}", i)
        r = verify(cue)
        if not r["ok"] or r["disc"] != d:
            raise RuntimeError(r.get("message") or f"{cue.name} is the other disc")
        JOB.say(f"{cue.name}: {r['name']} (sha1 {r['sha1']})")
        finished(f"verify-{d}")
        i += 1
    for d, cue in discs:
        ext = ROOT / ("extracted" if d == "sim" else "extracted-arcade")
        step(f"extract-{d}", i); i += 1
        run(py + ["extract", str(cue), "--out", str(ext)], env)
        finished(f"extract-{d}")
        step(f"recomp-{d}", i); i += 1
        if d == "sim":
            run(py + ["recomp"], env)
        else:
            run(py + ["recomp", "--disc", "arc", "--files", str(ext / "files"), "--out", str(ROOT / "generated" / "arcade"),
                      "--no-memprof"], env)
        finished(f"recomp-{d}")
        step(f"pack-{d}", i); i += 1
        run(py + ["pack", str(cue), "--out", str(ROOT / "build" / ("GT2.PAK" if d == "sim" else "GT2A.PAK"))], env)
        finished(f"pack-{d}")
    step("toolchain", i); i += 1
    tc = toolchain_state()
    if not tc["ok"]:          # release plan step 2/3: download of the pinned toolchain goes here
        raise RuntimeError(tc["message"])
    finished("toolchain")
    step("compile", i); i += 1
    run(["bash", "scripts/converter-build.sh"], dict(os.environ, PSPDEV=tc["path"]))   # -> build_psp.py (plan step 1)
    finished("compile")
    step("folder", i); i += 1
    dst = OUTPUT / "PSP" / "GAME" / "GT2PSP"
    dst.mkdir(parents=True, exist_ok=True)
    rel = ROOT / "build" / "release"
    files = [rel / "EBOOT.PBP"]
    for d, _ in discs:
        files.append(rel / ("GT2SIM.PRX" if d == "sim" else "GT2ARC.PRX"))
        files.append(ROOT / "build" / ("GT2.PAK" if d == "sim" else "GT2A.PAK"))
    for f in files:
        JOB.say(f"copy {f.name}")
        shutil.copyfile(f, dst / f.name)
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
            self.js({"discs": scan(), "input": str(INPUT), "toolchain": toolchain_state()})
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
            if cue.suffix.lower() != ".cue" or not cue.exists():
                return self.js({"ok": False, "message": "no such .cue file"})
            threading.Thread(target=verify, args=(cue,), daemon=True).start()
            self.js({"started": True})
        elif self.path == "/api/convert":
            start(req.get("sim"), req.get("arc"))
            self.js({"started": True})
        elif self.path == "/api/open-output":
            p = OUTPUT / "PSP" / "GAME" / "GT2PSP"
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
