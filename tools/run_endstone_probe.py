"""Run a probe in a fresh Endstone/BDS process and preserve lifecycle evidence."""

from __future__ import annotations

import argparse
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


def _stop(process: subprocess.Popen[str], timeout: float) -> tuple[bool, bool]:
    """Return (graceful, forced), targeting only the owned child process."""
    if process.poll() is not None:
        return process.returncode == 0, False
    try:
        assert process.stdin is not None
        process.stdin.write("stop\n")
        process.stdin.flush()
        process.wait(timeout=timeout)
        return process.returncode == 0, False
    except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
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
    plugin = args.plugin.resolve()
    report = args.report.resolve()
    log_path = args.log.resolve()
    if not plugin.is_file():
        raise RuntimeError(f"probe plugin does not exist: {plugin}")
    if server_dir.exists() and any(server_dir.iterdir()):
        raise RuntimeError(f"server directory must be fresh and empty: {server_dir}")
    server_dir.mkdir(parents=True, exist_ok=True)
    plugins = server_dir / "plugins"
    plugins.mkdir(parents=True, exist_ok=True)
    staged = plugins / plugin.name
    shutil.copy2(plugin, staged)

    report.parent.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    for stale in (report, Path(str(report) + ".tmp")):
        if stale.exists():
            raise RuntimeError(f"refusing stale report path: {stale}")

    executable = args.executable or shutil.which("endstone")
    if not executable:
        raise RuntimeError("endstone executable was not found")
    command = [executable, "--server-folder", str(server_dir), "--yes", "--no-interactive"]
    environment = os.environ.copy()
    environment["ABI_PROBE_OUTPUT"] = str(report)
    environment["ABI_PROBE_RUN_ID"] = args.run_id
    environment["ABI_PROBE_CLEAN_START"] = "1"

    messages: "queue.Queue[str]" = queue.Queue()
    marker = f"ABI_PROBE_COMPLETE run_id={args.run_id}"
    saw_marker = False
    started_at = time.monotonic()
    forced = False
    graceful = False
    exit_code: int | None = None
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
        deadline = started_at + args.timeout
        try:
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    line = messages.get(timeout=0.25)
                    if marker in line:
                        saw_marker = True
                except queue.Empty:
                    pass
                if saw_marker and report.is_file():
                    try:
                        payload = json.loads(report.read_text(encoding="utf-8"))
                    except (OSError, json.JSONDecodeError):
                        continue
                    if payload.get("run_id") == args.run_id and payload.get("complete") is True:
                        break
            graceful, forced = _stop(process, args.stop_timeout)
            exit_code = process.returncode
        finally:
            if process.poll() is None:
                _, forced = _stop(process, args.stop_timeout)
                exit_code = process.returncode
            reader.join(timeout=5)

    if not saw_marker:
        raise RuntimeError(f"probe completion marker not observed; see {log_path}")
    if not report.is_file():
        raise RuntimeError(f"probe report was not produced; see {log_path}")
    payload = json.loads(report.read_text(encoding="utf-8"))
    if payload.get("run_id") != args.run_id:
        raise RuntimeError("probe report run_id does not match the fresh invocation")
    if payload.get("complete") is not True:
        raise RuntimeError("probe report is not complete")
    if not graceful or forced or exit_code != 0:
        raise RuntimeError(f"Endstone did not stop cleanly (exit={exit_code}, forced={forced})")
    return {
        "schema_version": 1,
        "run_id": args.run_id,
        "server_dir": str(server_dir),
        "plugin": staged.name,
        "report": str(report),
        "log": str(log_path),
        "probe_complete_marker": True,
        "graceful_shutdown": True,
        "forced": False,
        "exit_code": exit_code,
        "elapsed_seconds": round(time.monotonic() - started_at, 3),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-dir", required=True, type=Path)
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--executable")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--stop-timeout", type=float, default=30)
    args = parser.parse_args(argv)
    try:
        result = run(args)
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"run_endstone_probe: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
