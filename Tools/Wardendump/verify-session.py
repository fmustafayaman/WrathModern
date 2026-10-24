#!/usr/bin/env python3
"""Audit one real native-client stability run; no launch, input, or server emulation.

A run directory contains run.json (log offsets/executable identity) and client.pid.
By default inspect its immutable Logs/ snapshot; --live reads the game directory instead.
Exit 0 requires >=30 minutes of one continuously connected native player, advancing render
frames, continuing server traffic, answered challenges and no logged emulator failure.
A successful audit is evidence for this test window, not a promise about all game content.
"""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


HEALTH = re.compile(r"^(\d+) (.*)$")
PACKET = re.compile(r"^(\d+) (SMSG ([0-9a-f]+)|CMSG) size=(\d+): (.*)$")
ERROR = re.compile(
    r"no (?:native )?bridge|(?:read|write|fetch) fault|emu error|"
    r"delivered (?:function|handler).*failed|handler failed|native content hook: failed|"
    r"FrameXML hook:.*failed|unknown allocation|could not prepare native|rejected \d+ changed rows|"
    r"(?:instruction|execution) budget exhausted|module took a path the emulator cannot follow|"
    r"import .* was called but has no behavior|(?:read|write) failed at [0-9a-f]+",
    re.IGNORECASE,
)


def read_log(run, meta, name, live):
    root = Path(meta["game_directory"]) / "Logs" if live else run / "Logs"
    path = root / name
    if not path.exists():
        return ""
    data = path.read_bytes()
    # Snapshots already contain only the new bytes. Live files can be truncated at startup;
    # timestamp filtering below is authoritative for all protocol/health logs.
    if live:
        before = run / "before" / name
        prefix = before.read_bytes() if before.exists() else b""
        if prefix and data.startswith(prefix):
            data = data[len(prefix):]
    return data.decode(errors="replace")


def timed_lines(text, since):
    for line in text.splitlines():
        first = line.split(" ", 1)[0]
        if first.isdecimal() and int(first) >= since:
            yield line


def audit(run, live=False, minimum_seconds=1800):
    run = Path(run)
    meta = json.loads((run / "run.json").read_text())
    pid = int((run / "client.pid").read_text().strip())
    issues = []
    samples = []
    for line in read_log(run, meta, "session-health.log", live).splitlines():
        match = HEALTH.match(line)
        if not match:
            continue
        fields = dict(re.findall(r"(\w+)=([^ ]+)", match[2]))
        if int(fields.get("pid", "-1")) != pid:
            continue
        required = ("connected", "net", "player", "readyMs", "frames", "frameDelta",
                    "maxIdleGapMs", "x", "y", "z")
        if any(field not in fields for field in required):
            issues.append("incomplete health sample")
            continue
        samples.append((int(match[1]), fields))

    ready = next((i for i, (_, value) in enumerate(samples)
                  if value["connected"] == "1" and int(value["player"], 16) and int(value["readyMs"])), None)
    if ready is None:
        return {"passed": False, "issues": issues + ["no native world-ready samples"], "pid": pid}

    samples = samples[ready:]
    first_time, first = samples[0]
    start = first_time - int(first["readyMs"])
    end, last = samples[-1]
    guid = first["player"]
    elapsed = end - start
    frames = 0
    max_gap = 0
    rates = []
    for index, (stamp, value) in enumerate(samples):
        if value["connected"] != "1" or value["net"] != "5" or value["player"] != guid:
            issues.append("connection/player changed at %u" % stamp)
        if abs((stamp - int(value["readyMs"])) - start) > 2:
            issues.append("continuous ready timer reset at %u" % stamp)
        if int(value["maxIdleGapMs"]) > 2000:
            issues.append("main event-loop stall exceeded 2 seconds at %u" % stamp)
        if index:
            gap = stamp - samples[index - 1][0]
            max_gap = max(max_gap, gap)
            delta = int(value["frames"]) - int(samples[index - 1][1]["frames"])
            if gap <= 0 or gap > 20000 or delta <= 0 or delta != int(value["frameDelta"]):
                issues.append("missing/non-advancing render sample at %u" % stamp)
            else:
                rates.append(1000 * delta / gap)
                frames += delta
    if elapsed < minimum_seconds * 1000:
        issues.append("world duration %0.3f seconds is below %u" % (elapsed / 1000, minimum_seconds))
    if not rates or min(rates) < 15:
        issues.append("render rate fell below 15 FPS over a health interval")

    # Count the actual wire traffic, not timers or a process merely remaining alive.
    packets = []
    for line in read_log(run, meta, "realm-packets.log", live).splitlines():
        match = PACKET.match(line)
        if not match or int(match[1]) < start:
            continue
        raw = bytes.fromhex(match[5])
        if len(raw) != int(match[4]):
            issues.append("incomplete packet capture at %s" % match[1])
            continue
        inbound = bool(match[3])
        opcode = int(match[3], 16) if inbound else int.from_bytes(raw[:4], "little")
        if inbound and (b"Unauthorized software" in raw or b"modifications to your game client" in raw):
            issues.append("server unauthorized-software warning received at %s" % match[1])
        packets.append((int(match[1]), inbound, opcode, len(raw)))
    incoming = [row for row in packets if row[1]]
    counts_in = Counter(row[2] for row in incoming)
    counts_out = Counter(row[2] for row in packets if not row[1])
    if not incoming or incoming[-1][0] < end - 30000:
        issues.append("no recent server world traffic near the end of the health window")
    if any(row[2] in (0x003B, 0x004D) for row in incoming):
        issues.append("character list or logout-complete received after world entry")
    if counts_in[0x041B] == 0 or counts_in[0x041B] != counts_out[0x041B]:
        issues.append("world buck challenges were missing or not all replied to")
    if counts_in[0x02E6] == 0 or counts_out[0x02E7] == 0:
        issues.append("no bidirectional world Warden checks captured")
    network = list(timed_lines(read_log(run, meta, "net-events.log", live), start))
    if any("DISCONNECTED" in line or "SEND_DROPPED" in line or "CONNECT this=" in line for line in network):
        issues.append("disconnect, reconnect or dropped sends occurred during the run")

    errors = []
    # buck-data is timestamped; Warden's module-internal log is not, so the snapshot/live
    # prefix subtraction in read_log must isolate its session before looking for failures.
    for name in ("buck-data.log", "warden.log"):
        lines = read_log(run, meta, name, live).splitlines()
        if name == "buck-data.log":
            lines = list(timed_lines("\n".join(lines), start))
        errors.extend(name + ": " + line for line in lines if ERROR.search(line))
    if errors:
        issues.append("emulator/bridge failure found in the live logs")

    executable = Path(meta["executable"])
    executable_matches = executable.exists() and hashlib.sha256(executable.read_bytes()).hexdigest() == meta["executable_sha256"]
    if live and not executable_matches:
        issues.append("deployed executable changed during this run")
    if live:
        health = Path(meta["game_directory"]) / "Logs/session-health.log"
        if time.time() - health.stat().st_mtime > 30:
            issues.append("live health log is stale by more than 30 seconds")
        try:
            os.kill(pid, 0)
            process = subprocess.run(["ps", "-p", str(pid), "-o", "command="],
                                     capture_output=True, text=True, check=False, timeout=5)
            if process.returncode or process.stdout.strip() != str(executable):
                issues.append("live PID does not name the expected native client")
        except (OSError, subprocess.TimeoutExpired):
            issues.append("live native client process is not running")
    coordinates = {(sample["x"], sample["y"], sample["z"]) for _, sample in samples}
    if len(coordinates) < 2:
        issues.append("no native player position change observed")

    return {
        "passed": not issues, "issues": list(dict.fromkeys(issues)), "pid": pid,
        "player_guid": guid, "world_start_ms": start, "world_end_ms": end,
        "continuous_world_seconds": elapsed / 1000, "health_samples": len(samples),
        "max_health_sample_gap_ms": max_gap, "presented_frames_between_samples": frames,
        "max_event_loop_gap_ms": max(int(value["maxIdleGapMs"]) for _, value in samples),
        "minimum_interval_fps": round(min(rates), 2) if rates else 0,
        "mean_interval_fps": round(sum(rates) / len(rates), 2) if rates else 0,
        "distinct_native_positions": len(coordinates), "server_packets": len(incoming),
        "last_server_packet_ms": incoming[-1][0] if incoming else None,
        "buck_world_challenges": counts_in[0x041B], "buck_world_replies": counts_out[0x041B],
        "warden_world_packets": counts_in[0x02E6], "warden_world_replies": counts_out[0x02E7],
        "executable_sha256": meta["executable_sha256"], "executable_still_matches": executable_matches,
        "errors": errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--live", action="store_true")
    args = parser.parse_args()
    try:
        result = audit(args.run, args.live)
    except (OSError, ValueError, KeyError) as error:
        result = {"passed": False, "issues": [str(error)]}
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
