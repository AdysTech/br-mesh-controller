"""
PlatformIO pre-upload script: flash the LittleFS SPA *before* the firmware,
but only when the SPA **source** is newer than the last upload (the lora-relay
pattern). This is what keeps the two in step: `pio run -t upload` can no longer
ship a new firmware against a stale SPA (the "new fw + old UI" trap — the
ColourPicker landing in the SPA but not on the board, 2026-11-20).

Change detection is mtime-vs-marker, like gcc skipping unchanged sources:
  - newest SPA *source* mtime > .littlefs_uploaded marker  -> uploadfs first
  - otherwise                                              -> skip (fast upload)
`pio run -t uploadfs` still works standalone; it also refreshes the marker.

Watch ONLY real SPA source — never the build artifacts in data/ (index.html.gz,
spa.version), which build_spa.py rewrites on every build and would trip the
check on their own. e2e/ is test code, not shipped bytes, so it is not watched.
"""
Import("env")

import os
import sys


def upload_fs_if_changed(source, target, env):
    """Flash LittleFS only when the SPA source is newer than the marker."""
    current_env = env["PIOENV"]
    build_dir = env.subst("$BUILD_DIR")
    marker = os.path.join(build_dir, ".littlefs_uploaded")
    project_dir = env.subst("$PROJECT_DIR")

    # Directories of actual SPA source (changes here trigger a LittleFS re-upload)
    source_dirs = [
        os.path.join(project_dir, "frontend", "src"),
    ]

    # Individual SPA build inputs (not walked, so no risk of picking up data/
    # artifacts produced by build_spa.py).
    source_files = [
        os.path.join(project_dir, "frontend", "index.html"),
        os.path.join(project_dir, "frontend", "package.json"),
        os.path.join(project_dir, "frontend", "vite.config.js"),
        os.path.join(project_dir, "tools", "build_spa.py"),
    ]

    latest_mtime = 0.0
    found = False
    for base in source_dirs:
        if not os.path.isdir(base):
            continue
        for root, _dirs, files in os.walk(base):
            for f in files:
                fp = os.path.join(root, f)
                try:
                    mt = os.path.getmtime(fp)
                    if mt > latest_mtime:
                        latest_mtime = mt
                    found = True
                except OSError:
                    pass
    for fp in source_files:
        try:
            mt = os.path.getmtime(fp)
            if mt > latest_mtime:
                latest_mtime = mt
            found = True
        except OSError:
            pass

    marker_age = os.path.getmtime(marker) if os.path.exists(marker) else 0.0

    if not found or latest_mtime <= marker_age:
        print("[auto_uploadfs] SPA source unchanged — skipping LittleFS upload")
        return

    print("=" * 60)
    print(f"[auto_uploadfs] SPA source changed — flashing LittleFS for '{current_env}' BEFORE firmware...")
    print("=" * 60)

    cmd = f'"{sys.executable}" -m platformio run -t uploadfs -e {current_env}'
    if env.Execute(cmd) != 0:
        print("[auto_uploadfs] ERROR: Failed to flash the LittleFS SPA partition!")
        env.Exit(1)

    # Record the source state we just shipped so the next upload skips.
    with open(marker, "w") as f:
        f.write(str(int(latest_mtime)))

    print("[auto_uploadfs] LittleFS SPA uploaded — continuing with the firmware upload")
    print("=" * 60)


env.AddPreAction("upload", upload_fs_if_changed)
