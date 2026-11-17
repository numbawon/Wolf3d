#!/usr/bin/env python3
"""Fetch pinned build dependencies into .build/deps (gitignored scratch).

Idempotent: if the marker and all artifacts exist, nothing is downloaded.
Every archive is hash-verified before extraction. Dependencies live outside
the tracked tree on purpose (repo stores finished code only).

Artifacts:
  sdl3/SDL3.dll               SDL3 prebuilt (win32-arm64)
  sdl3/SDL3/*.h               SDL3 public headers (from the matching source)
  vulkan/vulkan/*.h           Vulkan-Headers (branch pinned to the loader version)
  vulkan_loader/vulkan-1.dll  Vulkan loader, built here from the pinned source
                              (arm64; the NVIDIA driver's native ICD needs a
                              matching-architecture loader)
  glslang/bin/glslang.exe     GLSL->SPIR-V compiler (x86_64, build-time only)
"""
import hashlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

DEPS = Path(__file__).resolve().parent.parent / ".build" / "deps"
MARKER = DEPS / "VERSIONS.txt"
BUILD = Path(__file__).resolve().parent.parent / ".build"

# (file, url, sha256)
PINNED = [
    (
        "sdl3.zip",
        "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/"
        "SDL3-3.4.20-win32-arm64.zip",
        "51c292eafc114ea3c63519fa2f5413a486979769d5444a636296f8a61c737267",
    ),
    (
        "sdl3-src.tar.gz",
        "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/"
        "SDL3-3.4.20.tar.gz",
        "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f",
    ),
    (
        "vulkan_headers.tar.gz",
        "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/heads/"
        "vulkan-sdk-1.4.357.tar.gz",
        "7d00e17738fae59d04de751f562ee2f7391f29740752d0f87d26cfde43f2283a",
    ),
    (
        "vulkan_loader_src.tar.gz",
        "https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/"
        "vulkan-sdk-1.4.357.0.tar.gz",
        "54f2537df22313768da0317dda2abdaaab7711b4081c48c869a79db343d0ae70",
    ),
    (
        # x86_64 prebuilt (runs under Prism): a build-time tool only.
        "glslang.zip",
        "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/"
        "glslang-16.6.0-windows-x86_64-release.zip",
        "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b",
    ),
    (
        # miniaudio single header (v0.11.x, before the 0.12 split .c/h).
        # Header-only: compiles into the translation unit that includes it.
        "miniaudio/miniaudio.h",
        "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h",
        "6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e",
    ),
]

# Artifacts that must exist for the marker to count.
REQUIRED = [
    "sdl3/SDL3.dll",
    "sdl3/SDL3/SDL.h",
    "vulkan/vulkan/vulkan.h",
    "vulkan_loader/vulkan-1.dll",
    "glslang/bin/glslang.exe",
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(name: str, url: str, want: str) -> None:
    path = DEPS / name
    if path.exists():
        got = sha256(path)
        if got == want:
            print(f"  {name}: present, hash verified")
            return
        print(f"  {name}: hash mismatch, re-downloading")
        path.unlink()
    print(f"  fetching {url}")
    tmp = DEPS / (name + ".part")
    urllib.request.urlretrieve(url, tmp)
    got = sha256(tmp)
    if got != want:
        tmp.unlink()
        raise SystemExit(f"  {name}: downloaded hash {got} != pinned {want}")
    tmp.rename(path)
    print(f"  {name}: downloaded, hash verified")


def main() -> int:
    if MARKER.exists() and all((DEPS / r).exists() for r in REQUIRED):
        print("w3d deps: marker + artifacts present, nothing to do")
        return 0

    DEPS.mkdir(parents=True, exist_ok=True)
    for name, url, want in PINNED:
        fetch(name, url, want)

    # SDL3 prebuilt (arm64 DLL)
    with zipfile.ZipFile(DEPS / "sdl3.zip") as z:
        z.extractall(DEPS / "sdl3")
    # SDL3 public headers from the matching source (3.4.x: include/SDL3/*.h)
    tmp = DEPS / "sdl3-src-tmp"
    with tarfile.open(DEPS / "sdl3-src.tar.gz") as t:
        t.extractall(tmp)
    for sub in ("SDL3", "build_config"):
        shutil.copytree(tmp / "SDL3-3.4.20" / "include" / sub,
                        DEPS / "sdl3" / sub, dirs_exist_ok=True)
    shutil.rmtree(tmp)
    # Vulkan headers (pinned branch; take include/vulkan + include/vk_video)
    tmp = DEPS / "vulkan-src-tmp"
    with tarfile.open(DEPS / "vulkan_headers.tar.gz") as t:
        t.extractall(tmp)
    src = tmp / "Vulkan-Headers-vulkan-sdk-1.4.357" / "include"
    for sub in ("vulkan", "vk_video"):
        shutil.copytree(src / sub, DEPS / "vulkan" / sub, dirs_exist_ok=True)
    shutil.rmtree(tmp)
    # glslang (x86_64, build-time only: compiles GLSL to portable SPIR-V)
    with zipfile.ZipFile(DEPS / "glslang.zip") as z:
        z.extractall(DEPS / "glslang")

    # Vulkan loader (arm64): extract pinned source, build once.
    loader_src = DEPS / "Vulkan-Loader-vulkan-sdk-1.4.357.0"
    if not loader_src.exists():
        with tarfile.open(DEPS / "vulkan_loader_src.tar.gz") as t:
            t.extractall(DEPS)
    loader_dll = DEPS / "vulkan_loader" / "vulkan-1.dll"
    if not loader_dll.exists():
        print("  building Vulkan loader (arm64) from pinned source")
        bat = BUILD / "build_loader.bat"
        r = subprocess.run(["cmd", "/c", str(bat)], cwd=BUILD,
                           capture_output=True, text=True)
        if r.returncode != 0 or not loader_dll.exists():
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            raise SystemExit("w3d deps: Vulkan loader build failed")
        print("  Vulkan loader built")

    MARKER.write_text(
        "sdl3=3.4.20\nvulkan_headers=vulkan-sdk-1.4.357\n"
        "vulkan_loader=vulkan-sdk-1.4.357.0\nglslang=16.6.0\n",
        encoding="ascii",
    )
    for r in REQUIRED:
        if not (DEPS / r).exists():
            raise SystemExit(f"w3d deps: {r} missing after extraction")
    print("w3d deps: ready")
    return 0


if __name__ == "__main__":
    sys.exit(main())
