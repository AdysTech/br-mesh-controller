"""
PlatformIO pre-build script: build the Svelte SPA and prepare data/index.html.gz

Flow (the lora-relay pipeline, same shape):
  1. Read FIRMWARE_VERSION from src/FirmwareVersion.h (single source of truth)
  2. Run `npm run build` in frontend/ with SPA_VERSION in the environment
     (vite.config.js bakes it into the bundle as __SPA_VERSION__)
  3. Gzip the output index.html -> data/index.html.gz (for LittleFS upload)
  4. Write data/spa.version (the same version string) — the board reads it
     from the mounted SPA slot at boot and serves it as /api/status "spa"
     (the browser shows its own baked version next to it: stale-SPA check)

Runs before compilation so `pio run -t uploadfs` always has a fresh SPA.
"""
Import("env")

import os
import re
import subprocess
import gzip
import shutil

PROJECT_DIR = env.subst("$PROJECT_DIR")
FRONTEND_DIR = os.path.join(PROJECT_DIR, "frontend")
SPA_HTML = os.path.join(PROJECT_DIR, "html", "index.html")   # vite outDir: ../html
DATA_DIR = os.path.join(PROJECT_DIR, "data")
GZ_PATH = os.path.join(DATA_DIR, "index.html.gz")
VERSION_FILE = os.path.join(PROJECT_DIR, "src", "FirmwareVersion.h")


def read_firmware_version():
    """FIRMWARE_VERSION is the single source of truth — the SPA carries the
    same number so the browser can compare the build it is RUNNING against
    the one the board is SERVING (stale-SPA check)."""
    try:
        with open(VERSION_FILE, "r") as f:
            m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', f.read())
        if m:
            return m.group(1)
    except OSError:
        pass
    print("[build_spa] WARNING: could not read FIRMWARE_VERSION — SPA build tagged 'dev'")
    return "dev"


def build_spa(source, target, env):
    print("=" * 60)
    print("[build_spa] Building Svelte SPA...")
    print("=" * 60)

    spa_version = read_firmware_version()

    if not os.path.exists(os.path.join(FRONTEND_DIR, "node_modules")):
        print("[build_spa] Installing frontend dependencies...")
        subprocess.check_call(["npm", "install"], cwd=FRONTEND_DIR)

    npm_env = dict(os.environ)
    npm_env["SPA_VERSION"] = spa_version
    result = subprocess.run(["npm", "run", "build"], cwd=FRONTEND_DIR,
                            env=npm_env, capture_output=True, text=True)
    if result.returncode != 0:
        print("[build_spa] BUILD FAILED:")
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError("Svelte SPA build failed")

    if not os.path.exists(SPA_HTML):
        raise RuntimeError(f"Build output not found: {SPA_HTML}")

    os.makedirs(DATA_DIR, exist_ok=True)
    with open(SPA_HTML, "rb") as f_in:
        with gzip.open(GZ_PATH, "wb", compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)

    with open(os.path.join(DATA_DIR, "spa.version"), "w") as f:
        f.write(spa_version)

    size_plain = os.path.getsize(SPA_HTML)
    size_gz = os.path.getsize(GZ_PATH)
    ratio = (1 - size_gz / size_plain) * 100 if size_plain > 0 else 0
    print(f"[build_spa] SPA version: {spa_version}")
    print(f"[build_spa] Plain:   {size_plain:,} bytes")
    print(f"[build_spa] Gzipped: {size_gz:,} bytes ({ratio:.1f}% reduction)")
    if size_gz > 96 * 1024:
        print("[build_spa] WARNING: gzipped SPA exceeds the 96 KB spa_a partition — uploadfs will fail")
    print("[build_spa] Ready for LittleFS upload: data/index.html.gz")
    print("=" * 60)


env.AddPreAction("buildprog", build_spa)
env.AddPreAction("upload", build_spa)
