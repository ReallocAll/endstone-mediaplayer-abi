"""Run generated-header MediaPlayer and its smoke plugin in a fresh Endstone/BDS."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path


def _reader(stream, messages: "queue.Queue[str]", log) -> None:
    try:
        for line in iter(stream.readline, ""):
            log.write(line)
            log.flush()
            messages.put(line)
    finally:
        stream.close()


def _send(process: subprocess.Popen[str], command: str) -> None:
    if process.poll() is not None or process.stdin is None:
        raise RuntimeError(f"Endstone exited before command {command!r}")
    process.stdin.write(command + "\n")
    process.stdin.flush()


def _stop(process: subprocess.Popen[str], timeout: float) -> tuple[bool, bool]:
    if process.poll() is not None:
        return process.returncode == 0, False
    try:
        _send(process, "stop")
        process.wait(timeout=timeout)
        return process.returncode == 0, False
    except (BrokenPipeError, OSError, RuntimeError, subprocess.TimeoutExpired):
        pass
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    return False, True


def run(args: argparse.Namespace) -> dict[str, object]:
    server_dir = args.server_dir.resolve()
    log_path = args.log.resolve()
    if server_dir.exists() and any(server_dir.iterdir()):
        raise RuntimeError(f"server directory must be fresh and empty: {server_dir}")
    server_dir.mkdir(parents=True, exist_ok=True)
    plugins_dir = server_dir / "plugins"
    plugins_dir.mkdir()

    staged_plugins: list[str] = []
    for value in args.plugin:
        plugin = value.resolve()
        if not plugin.is_file():
            raise RuntimeError(f"consumer plugin does not exist: {plugin}")
        staged = plugins_dir / plugin.name
        if staged.exists():
            raise RuntimeError(f"duplicate staged plugin name: {plugin.name}")
        shutil.copy2(plugin, staged)
        staged_plugins.append(plugin.name)

    executable = args.executable or shutil.which("endstone")
    if not executable:
        raise RuntimeError("endstone executable was not found")
    command = [executable, "--server-folder", str(server_dir), "--yes", "--no-interactive"]
    environment = os.environ.copy()
    messages: "queue.Queue[str]" = queue.Queue()
    observed: list[str] = []
    required = {
        "on_load": "ABI_CONSUMER_SMOKE_ON_LOAD",
        "server_access": "ABI_CONSUMER_SMOKE_SERVER_ACCESS",
        "logical_api": "ABI_CONSUMER_SMOKE_PASS",
        "mediaplayer_enable": "MediaPlayer v",
    }
    command_markers = {"mpm help": "/mpm", "mpv help": "/mpv"}
    started_at = time.monotonic()
    forced = False
    graceful = False

    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        log.write("HARNESS_COMMAND=" + json.dumps(command) + "\n")
        log.flush()
        process = subprocess.Popen(
            command,
            cwd=server_dir,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        assert process.stdout is not None
        reader = threading.Thread(target=_reader, args=(process.stdout, messages, log), daemon=True)
        reader.start()
        try:
            startup_deadline = started_at + args.timeout
            combined = ""
            while time.monotonic() < startup_deadline:
                if process.poll() is not None:
                    break
                try:
                    line = messages.get(timeout=0.25)
                except queue.Empty:
                    continue
                observed.append(line.rstrip())
                combined += line
                if "ABI_CONSUMER_SMOKE_FAIL" in line:
                    raise RuntimeError(f"runtime smoke plugin failed; see {log_path}")
                if all(marker in combined for marker in required.values()):
                    break
            else:
                raise RuntimeError(f"consumer runtime startup timed out; see {log_path}")
            if process.poll() is not None:
                raise RuntimeError(f"Endstone exited during consumer startup with {process.returncode}")
            missing = [name for name, marker in required.items() if marker not in combined]
            if missing:
                raise RuntimeError("missing runtime markers: " + ", ".join(missing))

            for console_command, marker in command_markers.items():
                before = len(combined)
                _send(process, console_command)
                deadline = time.monotonic() + args.command_timeout
                while time.monotonic() < deadline and marker not in combined[before:]:
                    if process.poll() is not None:
                        raise RuntimeError(f"Endstone exited after {console_command!r} with {process.returncode}")
                    try:
                        line = messages.get(timeout=0.25)
                    except queue.Empty:
                        continue
                    observed.append(line.rstrip())
                    combined += line
                    if "ABI_CONSUMER_SMOKE_FAIL" in line:
                        raise RuntimeError(f"runtime smoke plugin failed; see {log_path}")
                if marker not in combined[before:]:
                    raise RuntimeError(f"no output marker for command {console_command!r}; see {log_path}")

            graceful, forced = _stop(process, args.stop_timeout)
            disable_deadline = time.monotonic() + 5
            while time.monotonic() < disable_deadline:
                try:
                    line = messages.get(timeout=0.25)
                except queue.Empty:
                    if process.poll() is not None:
                        break
                    continue
                observed.append(line.rstrip())
                combined += line
            if "ABI_CONSUMER_SMOKE_ON_DISABLE passed=true" not in combined:
                raise RuntimeError("successful smoke onDisable marker was not observed")
            if "MediaPlayer disabled!" not in combined:
                raise RuntimeError("MediaPlayer disable marker was not observed")
        finally:
            if process.poll() is None:
                _, forced = _stop(process, args.stop_timeout)
            reader.join(timeout=5)

    if not graceful or forced or process.returncode != 0:
        raise RuntimeError(f"Endstone did not stop cleanly (exit={process.returncode}, forced={forced})")
    log_hash = hashlib.sha256(log_path.read_bytes()).hexdigest()
    return {
        "schema_version": 1,
        "status": "PASS",
        "platform": args.platform,
        "run_id": args.run_id,
        "fresh_server": True,
        "player_required": False,
        "player_joined": False,
        "plugins": staged_plugins,
        "commands": list(command_markers),
        "markers": required,
        "logical_screen_lifecycle": "PASS",
        "command_sender_message": "PASS",
        "graceful_shutdown": True,
        "forced": False,
        "exit_code": process.returncode,
        "media_player_commit": args.media_player_commit,
        "endstone_commit": args.endstone_commit,
        "endstone_version": args.endstone_version,
        "bds_version": args.bds_version,
        "log": str(log_path),
        "log_sha256": log_hash,
        "elapsed_seconds": round(time.monotonic() - started_at, 3),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-dir", required=True, type=Path)
    parser.add_argument("--plugin", required=True, action="append", type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux"))
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--media-player-commit", required=True)
    parser.add_argument("--endstone-commit", required=True)
    parser.add_argument("--endstone-version", required=True)
    parser.add_argument("--bds-version", required=True)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--executable")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--command-timeout", type=float, default=30)
    parser.add_argument("--stop-timeout", type=float, default=30)
    args = parser.parse_args(argv)
    try:
        result = run(args)
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"run_consumer_smoke: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
