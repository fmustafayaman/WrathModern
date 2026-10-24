#!/usr/bin/env python3
"""Continuously audit a native session without controlling or keeping the client alive.

Usage: python3 Tools/Wardendump/watch-session.py <private-run-directory>

Unlike the one-shot verifier, this observer continues after a failure so later errors
are not missed. Every audit keeps all verifier rejection gates; an old failed interval
never becomes successful merely because the character logs in again. New problems and
periodic passing windows are snapshotted separately, never over a previous proof.
Private snapshots can contain session keys. Keep the whole run outside the repository.
"""

import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import time

SPEC = importlib.util.spec_from_file_location("verify_session", Path(__file__).with_name("verify-session.py"))
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def classify(result, age):
    if result.get("passed"):
        return "verified"
    issues = result.get("issues", [])
    remaining = [issue for issue in issues if not issue.startswith("world duration ")]
    if issues and not remaining:
        return "measuring"
    startup = {
        "no native world-ready samples",
        "no native player position change observed",
        "no bidirectional world Warden checks captured",
        "world buck challenges were missing or not all replied to",
    }
    if result.get("health_samples", 0) < 2:
        startup.add("render rate fell below 15 FPS over a health interval")
    if age < 120 and remaining and all(issue in startup for issue in remaining):
        return "warming-up"  # Not a pass; the complete failed audit remains recorded.
    return "problem"


def problem_signature(result):
    return sorted({re.sub(r" at \d+", " at <timestamp>", issue)
                   for issue in result.get("issues", []) if not issue.startswith("world duration ")})


def snapshot(run, destination, meta, live_audit=None):
    destination.mkdir(parents=True, mode=0o700, exist_ok=False)
    output = destination / "Logs"
    output.mkdir(mode=0o700)
    for name in ("run.json", "client.pid"):
        shutil.copyfile(run / name, destination / name)
    for source in (Path(meta["game_directory"]) / "Logs").iterdir():
        if not source.is_file() or source.name.startswith("._"):
            continue
        target = output / source.name
        if source.suffix == ".log":
            data = source.read_bytes()
            before = run / "before" / source.name
            prefix = before.read_bytes() if before.exists() else b""
            target.write_bytes(data[len(prefix):] if prefix and data.startswith(prefix) else data)
        else:
            shutil.copyfile(source, target)
    result = VERIFY.audit(destination, live=False)
    atomic_json(destination / "snapshot-audit.json", result)
    if live_audit is not None:
        atomic_json(destination / "live-audit.json", live_audit)
    manifest = {str(path.relative_to(destination)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(destination.rglob("*")) if path.is_file()}
    atomic_json(destination / "sha256.json", manifest)
    return result


def observe_once(run, meta, state, checkpoint_seconds=1800):
    try:
        result = VERIFY.audit(run, live=True)
    except (OSError, ValueError, KeyError) as error:
        result = {"passed": False, "issues": ["observer audit exception: " + str(error)]}
    status = classify(result, time.time() - meta["start_wall_time"])
    stamp = datetime.now(timezone.utc)
    record = {"wall_time": stamp.isoformat(), "status": status, "audit": result}
    with (run / "progress.jsonl").open("a") as output:
        output.write(json.dumps(record) + "\n")
    atomic_json(run / "latest-audit.json", result)
    atomic_json(run / "observer-status.json", {"pid": os.getpid(), "wall_time": record["wall_time"],
                                               "status": status})
    signature = problem_signature(result) if status == "problem" else []
    event = status == "problem" and signature != state.get("last_problem_signature")
    seconds = result.get("continuous_world_seconds", 0)
    checkpoint = status == "verified" and seconds - state.get("last_checkpoint_seconds", 0) >= checkpoint_seconds
    if event or checkpoint:
        kind = "events" if event else "checkpoints"
        destination = run / kind / stamp.strftime("%Y%m%dT%H%M%S%fZ")
        try:
            frozen = snapshot(run, destination, meta, result)
            if event:
                state["last_problem_signature"] = signature
            elif frozen["passed"]:
                state["last_checkpoint_seconds"] = seconds
            record["archive"] = str(destination)
            record["snapshot_passed"] = frozen["passed"]
        except (OSError, ValueError, KeyError) as error:
            record["archive_error"] = str(error)  # Keep observing; retry archiving next poll.
    if status != "problem":
        state["last_problem_signature"] = []
    atomic_json(run / "observer-state.json", state)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--interval", type=float, default=20)
    parser.add_argument("--checkpoint-seconds", type=float, default=1800)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    if args.interval < 1 or args.checkpoint_seconds < 1:
        parser.error("interval/checkpoint seconds must be positive and at least one")
    os.umask(0o077)
    run = args.run.resolve()
    meta = json.loads((run / "run.json").read_text())
    with (run / "observer.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error("another persistent observer already holds this run")
        state_path = run / "observer-state.json"
        state = json.loads(state_path.read_text()) if state_path.exists() else {}
        while True:
            record = observe_once(run, meta, state, args.checkpoint_seconds)
            audit = record["audit"]
            print(json.dumps({"status": record["status"],
                              "seconds": audit.get("continuous_world_seconds", 0),
                              "issues": problem_signature(audit),
                              "archive": record.get("archive"),
                              "archive_error": record.get("archive_error")}), flush=True)
            if args.once:
                return 0 if audit.get("passed") else 1
            time.sleep(args.interval)


if __name__ == "__main__":
    raise SystemExit(main())
