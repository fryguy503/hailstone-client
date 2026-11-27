#!/usr/bin/env python3
"""Nightly performance tracking (docs/ci.md "Nightly performance").

Runs the client's --bench (app/frame_bench.h) in a few fixed scenes, appends each scene's frame times and memory to a
CSV kept between nights, and compares them with the median of the scene's last nights. Exit status 0, 1 when a
measure went up past its threshold, 2 when no scene gave a result.

  tools/perf_track.py --hailstone build-perf/hailstone/hailstone --csv ~/.cache/hailstone-nightly/perf.csv \\
      [--zones qeytoqrg,qeynos2] [--live HOST[:PORT]] [--commit SHA] [--logs DIR]

The scenes:
  viewer:<zone>  the zone viewer (--zone) at its start point, noon, no sound, the client's modern lighting (the
                 viewer's own default is classic): the zone renderer alone, no server.
  client:<zone>  with --live, the client logged in with HAILSTONE_LIVE_USER, HAILSTONE_LIVE_PASSWORD and
                 HAILSTONE_LIVE_CHARACTER (read from the environment, never given here), timed where the character
                 stands once the zone has settled: actors, UI and the connection too.

EQ_GAME_DIR must be set. Each scene runs --warmup times uncounted, then --repeat times, and keeps each measure's
median. The client scene is flagged on memory only: its frame times are recorded, not compared (FRAME_MEASURES). The window is SDL's offscreen
driver (Vulkan's headless surface) unless --display: the nightly service has no display, and a compositor's pacing
would make nights incomparable. Frame times are only comparable on the same machine and settings.
"""
import argparse
import csv
import datetime
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

FIELDS = ["time", "commit", "scene", "frames", "mean_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms", "hitches",
          "resident_mib", "commit_mib", "gpu_mib"]
MEASURES = FIELDS[3:]
# What is compared, and how much it may grow over the median before it counts: a share, and an amount below which a
# change is noise whatever its share (a 0.2 ms frame doubling to 0.4 ms is not worth a morning's look).
THRESHOLDS = {
    "mean_ms": (0.10, 0.25),
    "p99_ms": (0.15, 0.50),
    "resident_mib": (0.05, 48.0),
    "gpu_mib": (0.05, 32.0),
}
# The logged-in client's frame times swing by half from run to run with the same build (spawns walking through the
# view; measured 3 to 7 ms in one zone), so its scene is flagged on memory alone and its frame times only recorded.
FRAME_MEASURES = ("mean_ms", "p99_ms")
DEFAULT_ZONES = "qeytoqrg,qeynos2,gfaydark,poknowledge"

# The bench line itself, not the client's "memory checkpoint: bench: ..." lines that follow it.
BENCH_RE = re.compile(r"(?:^|\] )bench: (label=.*)$")


def parse_bench(text):
    """The last "bench:" line's key=value pairs, numbers as floats; None when there is none."""
    found = None
    for line in text.splitlines():
        m = BENCH_RE.search(line)
        if m:
            found = m.group(1)
    if found is None:
        return None
    out = {}
    for pair in found.split():
        key, sep, value = pair.partition("=")
        if not sep:
            continue
        try:
            out[key] = float(value)
        except ValueError:
            out[key] = value
    return out


def combine(results):
    """Each measure's median over the runs that reported it."""
    out = {}
    for key in MEASURES:
        values = [r[key] for r in results if isinstance(r.get(key), float)]
        if values:
            out[key] = statistics.median(values)
    return out


def read_history(path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def append_rows(path, rows):
    new = not path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new:
            w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in FIELDS})


def compare(history, scene, current, window=7, least=3):
    """(baselines, regressions) of `current` against the median of the scene's last `window` rows in `history`.
    No regressions with fewer than `least` earlier rows, nor for a client scene's frame times. A regression is
    (measure, now, median)."""
    past = [r for r in history if r.get("scene") == scene][-window:]
    baselines, regressions = {}, []
    for measure, (share, floor) in THRESHOLDS.items():
        values = []
        for r in past:
            try:
                values.append(float(r[measure]))
            except (KeyError, TypeError, ValueError):
                pass
        if len(values) < least or measure not in current:
            continue
        median = statistics.median(values)
        baselines[measure] = median
        now = current[measure]
        if scene.startswith("client:") and measure in FRAME_MEASURES:
            continue
        if now > median * (1.0 + share) and now - median > floor:
            regressions.append((measure, now, median))
    return baselines, regressions


def split_host(text):
    host, sep, port = text.rpartition(":")
    if not sep or not port.isdigit():
        return text, 5999
    return host, int(port)


def scenes(args):
    """(name, command, timeout seconds) for each scene."""
    common = ["--no-vsync", "--window-size", args.window_size, "--no-audio",
              "--game-dir", os.environ["EQ_GAME_DIR"]]
    out = []
    for zone in [z for z in args.zones.split(",") if z]:
        out.append((f"viewer:{zone}", [args.hailstone, "--zone", zone, "--time", "12", "--lighting", "modern",
                                          "--bench", str(args.frames)] + common, 240))
    if args.live:
        host, port = split_host(args.live)
        # 120 s: login and zone-in, the settle, the frames, then /camp desktop (up to 30 s for a player).
        out.append(("client", [args.hailstone, "--host", host, "--port", str(port),
                               "--user", os.environ["HAILSTONE_LIVE_USER"],
                               "--password", os.environ["HAILSTONE_LIVE_PASSWORD"],
                               "--character", os.environ["HAILSTONE_LIVE_CHARACTER"],
                               "--bench", str(args.client_frames)] + common, 180))
    return out


# A run without a result is tried once more. Back-to-back logins of one character are fine on a stock server (its
# zone has finished the camped session long before the next client reaches the world); what has ended a logged-in
# run is a second login of the same account from elsewhere while it played: the world drops the older session
# ("Dropped by world CLE subsystem"), and the test login then waits at character select until the timeout.
def run_client(command, timeout, log_path, env):
    for attempt in (1, 2):
        r = run_once(command, timeout, log_path.with_suffix(f".try{attempt}.log") if attempt > 1 else log_path, env)
        if r is not None:
            return r
    return None


def run_once(command, timeout, log_path, env):
    try:
        p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env,
                           text=True, errors="replace")
        text = p.stdout
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        text += f"\nperf_track: timed out after {timeout} s\n"
    log_path.write_text(text)
    return parse_bench(text)


def fmt(row):
    parts = [f"mean {row['mean_ms']:.2f} ms", f"p99 {row['p99_ms']:.2f} ms"]
    if "resident_mib" in row:
        parts.append(f"resident {row['resident_mib']:.0f} MiB")
    if "gpu_mib" in row:
        parts.append(f"GPU {row['gpu_mib']:.0f} MiB")
    return ", ".join(parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--hailstone", required=True, help="the client to run (a Release build's dev tree)")
    ap.add_argument("--csv", required=True, type=Path, help="the history, appended to")
    ap.add_argument("--zones", default=DEFAULT_ZONES, help=f"viewer scenes, comma-separated (default {DEFAULT_ZONES})")
    ap.add_argument("--live", help="HOST[:PORT]: also the client scene on this login server")
    ap.add_argument("--frames", type=int, default=600, help="frames timed per viewer run (default 600)")
    ap.add_argument("--client-frames", type=int, default=3000, help="frames timed per client run (default 3000)")
    ap.add_argument("--repeat", type=int, default=3, help="runs per scene (default 3)")
    ap.add_argument("--warmup", type=int, default=1,
                    help="runs per scene first, not counted (default 1: the first after a build fills the caches)")
    ap.add_argument("--window-size", default="1920x1080")
    ap.add_argument("--window", type=int, default=7, help="earlier nights compared with (default 7)")
    ap.add_argument("--commit", default="", help="recorded with the rows")
    ap.add_argument("--logs", type=Path, default=Path("perf-logs"), help="each run's log (default ./perf-logs)")
    ap.add_argument("--display", action="store_true", help="use the desktop's display, not the offscreen driver")
    args = ap.parse_args(argv)

    if not os.environ.get("EQ_GAME_DIR"):
        print("perf_track: set EQ_GAME_DIR to the game folder", file=sys.stderr)
        return 2
    if args.live and not all(os.environ.get(k) for k in
                             ("HAILSTONE_LIVE_USER", "HAILSTONE_LIVE_PASSWORD", "HAILSTONE_LIVE_CHARACTER")):
        print("perf_track: --live needs HAILSTONE_LIVE_USER, _PASSWORD and _CHARACTER set", file=sys.stderr)
        return 2
    env = dict(os.environ)
    if not args.display:
        env["SDL_VIDEO_DRIVER"] = "offscreen"
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
    args.logs.mkdir(parents=True, exist_ok=True)

    history = read_history(args.csv)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    rows, regressed, measured = [], False, 0
    for name, command, timeout in scenes(args):
        results, label = [], "unknown"
        run = run_client if name == "client" else run_once
        for i in range(max(0, args.warmup)):
            run(command, timeout, args.logs / f"{name.replace(':', '-')}-warmup{i + 1}.log", env)
        for i in range(max(1, args.repeat)):
            log = args.logs / f"{name.replace(':', '-')}-{i + 1}.log"
            r = run(command, timeout, log, env)
            if r is not None:
                results.append(r)
                label = str(r.get("label", label))
        if not results:
            print(f"perf {name}: no result ({args.logs}/{name.replace(':', '-')}-*.log)")
            continue
        # The client's scene is named by where the character stood: nights in different zones are not compared.
        scene = f"client:{label}" if name == "client" else name
        current = combine(results)
        measured += 1
        baselines, regressions = compare(history, scene, current, args.window)
        line = f"perf {scene}: {fmt(current)}"
        if baselines:
            line += f" (median of last {args.window}: mean {baselines.get('mean_ms', 0):.2f} ms, " \
                    f"p99 {baselines.get('p99_ms', 0):.2f} ms)"
        print(line)
        for measure, now, median in regressions:
            regressed = True
            print(f"REGRESSION {scene} {measure}: {now:.2f} against a median of {median:.2f} "
                  f"(+{100.0 * (now - median) / median:.0f}%)")
        rows.append(dict(current, time=stamp, commit=args.commit, scene=scene))
    append_rows(args.csv, rows)
    if not measured:
        return 2
    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
