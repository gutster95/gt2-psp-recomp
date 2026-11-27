"""PSP toolchain for the converter: find it, or download the pinned package for this platform and verify it
(release plan step 3). Packages: converter/toolchains.json (url + sha256 per platform). Windows uses our own build
(scripts/wintc/build.sh), Linux/macOS the official pspdev release.

Unpacked into tools/pspdev-<platform>/pspdev; nothing outside the project folder is touched.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""


def platform_key() -> str:
    m = platform.machine().lower()
    arch = "arm64" if m in ("arm64", "aarch64") else "x86_64"
    if sys.platform.startswith("win"):
        return f"windows-{arch}"
    if sys.platform == "darwin":
        return f"macos-{arch}"
    return f"linux-{arch}"


def install_dir() -> Path:
    return ROOT / "tools" / f"pspdev-{platform_key()}"


def valid(p: Path) -> bool:
    return (p / "bin" / f"psp-gcc{EXE}").exists() and (p / "psp" / "sdk" / "lib" / "prxspecs").exists()


def find() -> Path | None:
    """An installed toolchain: our download, or PSPDEV from the environment (developers)."""
    for p in (install_dir() / "pspdev", Path(os.environ["PSPDEV"]) if os.environ.get("PSPDEV") else None):
        if p and valid(p):
            if p == install_dir() / "pspdev":
                drop_bad_plugins(p)
            return p
    return None


def drop_bad_plugins(p: Path) -> None:
    """Windows: binutils (ar, nm, ld) try to load every file in lib/bfd-plugins as a DLL. Our first Windows package
    shipped binutils' static libdep.a there; loading it pops up a modal 'bad image' (0xc000012f) dialog that stalls
    the build. Only DLLs belong there on Windows."""
    if os.name != "nt":
        return
    d = p / "lib" / "bfd-plugins"
    if d.is_dir():
        for f in d.iterdir():
            if f.is_file() and f.suffix.lower() != ".dll":
                f.unlink(missing_ok=True)


def glibc() -> tuple[int, ...]:
    lib, ver = platform.libc_ver()
    try:
        return tuple(int(x) for x in ver.split(".")) if lib == "glibc" else (0,)
    except ValueError:
        return (0,)


def package() -> dict | None:
    """The package for this machine; one with min_glibc falls back to its 'fallback' entry on an older system."""
    packages = json.loads((Path(__file__).with_name("toolchains.json")).read_text())["packages"]
    pkg = packages.get(platform_key())
    while pkg and pkg.get("min_glibc") and glibc() < tuple(int(x) for x in pkg["min_glibc"].split(".")):
        pkg = packages.get(pkg.get("fallback", ""))
    return pkg


def missing_libs(p: Path) -> str:
    """Linux/macOS: run the compiler proper once; a missing shared library shows up here (official pspdev builds)."""
    cc1 = p / "libexec" / "gcc" / "psp" / "15.2.0" / f"cc1{EXE}"
    if os.name == "nt" or not cc1.exists():
        return ""
    r = subprocess.run([str(cc1), "--version"], capture_output=True, text=True)
    if r.returncode == 0:
        return ""
    return (r.stderr.strip().splitlines() or ["cc1 does not start"])[-1]


def state() -> dict:
    p = find()
    if p:
        return {"ok": True, "path": str(p), "message": f"PSP toolchain ready ({p.name}, {platform_key()})"}
    pkg = package()
    if not pkg or not pkg.get("url"):
        return {"ok": False, "download": False,
                "message": f"No PSP toolchain package for this platform ({platform_key()}) yet."}
    return {"ok": True, "download": True, "path": "",
            "message": f"The PSP toolchain (~{pkg['size_mb']} MB) is downloaded when you start the conversion."}


def ensure(say: Callable[[str], None], progress: Callable[[float], None] | None = None) -> Path:
    p = find()
    if p:
        check(p)
        return p
    pkg = package()
    if not pkg or not pkg.get("url"):
        raise RuntimeError(f"no PSP toolchain package for {platform_key()}")
    dst = install_dir()
    dst.mkdir(parents=True, exist_ok=True)
    name = pkg["url"].rsplit("/", 1)[-1]
    archive = dst / name
    if not (archive.exists() and sha256(archive) == pkg["sha256"]):
        say(f"downloading {pkg['url']}")
        tmp = archive.with_suffix(archive.suffix + ".part")
        with urllib.request.urlopen(pkg["url"], timeout=60) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            while chunk := r.read(1 << 20):
                f.write(chunk)
                done += len(chunk)
                if progress and total:
                    progress(done / total)
        tmp.replace(archive)
        got = sha256(archive)
        if got != pkg["sha256"]:
            archive.unlink()
            raise RuntimeError(f"toolchain download damaged (sha256 {got}, expected {pkg['sha256']})")
    say(f"unpacking {name}")
    if (dst / "pspdev").exists():
        shutil.rmtree(dst / "pspdev")
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            safe_extract(z.namelist(), dst)
            z.extractall(dst)
    else:
        with tarfile.open(archive) as t:
            safe_extract(t.getnames(), dst)
            t.extractall(dst, filter="tar") if hasattr(tarfile, "data_filter") else t.extractall(dst)
    if sys.platform == "darwin":       # files from the internet: clear Gatekeeper's quarantine flag
        subprocess.run(["xattr", "-rd", "com.apple.quarantine", str(dst / "pspdev")], capture_output=True)
    archive.unlink(missing_ok=True)
    p = find()
    if not p:
        raise RuntimeError("the toolchain package did not contain pspdev/bin/psp-gcc")
    check(p)
    return p


def check(p: Path) -> None:
    err = missing_libs(p)
    if err:
        pkg = package() or {}
        hint = f" Install: {pkg['needs']}." if pkg.get("needs") else ""
        raise RuntimeError(f"the PSP compiler cannot start: {err}.{hint}")


def safe_extract(names: list[str], dst: Path) -> None:
    for n in names:
        target = (dst / n).resolve()
        if not str(target).startswith(str(dst.resolve())):
            raise RuntimeError(f"unsafe path in toolchain archive: {n}")


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while chunk := f.read(4 << 20):
            h.update(chunk)
    return h.hexdigest()
