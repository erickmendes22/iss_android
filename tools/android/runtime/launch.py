"""Android/glibc launcher for the original inFAMOUS Second Son PC runtime.

Never require GoW3/Last Guardian resource layouts or silently enable their patches.
The ARM64 Python and probe are always run through the explicit Linux/glibc loader.
"""
import os
from pathlib import Path
import subprocess
import sys

BASE = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE / "scripts"))

# Defaults verified in the inFAMOUS PC launcher (port/launcher/infamous_run.py).
# Explicit settings from the Android launcher or user are not overwritten.
ISS_DEFAULTS = {
    "ISS_GC_BUDGET_MB": "3500",
    "ISS_READBACKS": "2",
    "ISS_EAGER_READBACK": "0",
    "ISS_WRITE_MERGE": "1",
    "ISS_DEFERRED_READBACK": "1",
    "ISS_STICK_DEADZONE": "0.10",
    "ISS_PIPELINE_BINARIES": "0",
    "ISS_UPSCALER": "off",
    "ISS_JITTER": "0",
    "ISS_OBJECT_MOTION": "0",
    # No TLG_FRAME_STATS entry in production. A dedicated diagnostic run
    # may explicitly set TLG_FRAME_STATS=1 before launching the process.
}


def check_game_layout(game):
    """Quick, game-specific read checks; linking still verifies ELF, PRX and TLS formats."""
    game = Path(game)
    if not game.is_dir():
        raise RuntimeError(f"ISS game preflight: selected directory not accessible: {game}")
    for relative in ("eboot.bin", "sce_sys/param.sfo",
                     "sce_module/libc.prx", "sce_module/libSceFios2.prx"):
        path = game / relative
        try:
            if not path.is_file() or path.stat().st_size == 0:
                raise RuntimeError(f"ISS game preflight: missing/empty game file: {path}")
            # is_file() can succeed via stat even when reading later gets EACCES.
            with path.open("rb") as stream:
                if not stream.read(1):
                    raise RuntimeError(f"ISS game preflight: empty game file: {path}")
        except OSError as exc:
            raise RuntimeError(f"ISS game preflight: cannot read {path}: {exc}") from exc

    # inFAMOUS uses art/cache, art/movies, art/ui; it does NOT require dvdroot_ps4 or PSARC.
    art = game / "art"
    if not art.is_dir():
        raise RuntimeError(f"ISS game preflight: missing inFAMOUS resource folder: {art}")
    try:
        # Verify permissions on the large resource subtree using only metadata.
        with os.scandir(art) as entries:
            first = next(entries, None)
            if first is None:
                raise RuntimeError(f"ISS game preflight: empty art/ folder: {art}")
    except OSError as exc:
        raise RuntimeError(f"ISS game preflight: cannot list {art}: {exc}") from exc
    print(f"ISS game layout OK: {game} (eboot, game modules, sce_sys and art/ accessible)", flush=True)


def setup_environment(env, user_dir):
    for key, value in ISS_DEFAULTS.items():
        env.setdefault(key, value)
    # Save/config and shader caches are in the APK's private writable directory.
    env.setdefault("ISS_CONFIG", str(user_dir / "tlg.ini"))
    env.setdefault("TLG_CONFIG", env["ISS_CONFIG"])
    env.setdefault("TLG_GPU_USER_DIR", str(user_dir))
    env.setdefault("ISS_APP_NAME", "inFAMOUS Second Son")
    env.setdefault("ISS_APP_ID", "com.daedalus.iss")
    # TLG_ variables, when present, take priority in probe.c's ISS_ aliasing.
    # Deliberately do not set GoW3's frame-rate, resolution, or texture patches.
    return env


def main():
    env = os.environ.copy()
    game = Path(env["TLG_GAME_DIR"])
    data = Path(env["TLG_DATA_DIR"])
    out = data / "out"
    root = Path(env["FXD_ROOT"])
    loader = root / "usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
    python = root / "usr/bin/python3.13"
    check_game_layout(game)
    if not loader.is_file() or not python.is_file():
        raise SystemExit("ISS runtime preflight: ARM64 glibc loader or Python missing.")
    out.mkdir(parents=True, exist_ok=True)
    user = data / "user"
    user.mkdir(parents=True, exist_ok=True)
    setup_environment(env, user)
    print("ISS boot defaults: inFAMOUS PC preset, precise readbacks, temporal upscale off", flush=True)

    def run_step(script, *arguments):
        name = BASE / "scripts" / (script + ".py")
        if not name.is_file():
            raise SystemExit(f"ISS package incomplete: missing {name}")
        print(f"ISS preparation: {script}", flush=True)
        subprocess.run([str(loader), str(python), str(name), *map(str, arguments)],
                       check=True, env=env)
        print(f"ISS preparation complete: {script}", flush=True)

    run_step("prepare", game, "--out", out)
    # link_modules uses boot.bin directly; link_libc is not a prerequisite.
    run_step("link_modules", game, "--out", out)
    run_step("content_profile", game, "--out", out, "--sku", "full")
    run_step("patches", "--out", out, "--extra", env.get("ISS_PATCHES", ""),
             "--game-dir", game, "--patches-dir", data / "patches",
             "--patches-config", data / "patches.json")
    image = out / "boot-linked.bin"
    if not image.is_file() or image.stat().st_size == 0:
        raise SystemExit("ISS preparation: boot-linked.bin was not produced.")
    for artifact in ("content.bin", "patches.bin"):
        if not (out / artifact).is_file():
            raise SystemExit(f"ISS preparation: missing {artifact}")

    args = [str(loader), str(BASE / "bin/iss-probe"), str(image),
            "--content-profile", str(out / "content.bin"),
            "--patches", str(out / "patches.bin"),
            "--app0", str(game), "--user", str(user), "--timeout", "0"]
    print("ISS preparation complete; entering ARM64 FEX game loader", flush=True)
    sys.stdout.flush()
    sys.stderr.flush()
    os.execve(str(loader), args, env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"ISS launch stopped: {error}") from error
