#!/usr/bin/env python3
"""Summarize a PERF RECORD profile (rdrvr/profiles/profile-*.csv, native/src/diagnostics/PerfRecorder.h).

Usage:
  python tools/perf_profile.py PROFILE.csv [--top 12] [--seconds-csv out.csv]
  python tools/perf_profile.py DIR            (the newest profile-*.csv in DIR or DIR/profiles, also a COLLECT_REPORT folder)

Prints: the session overview (FPS, frame-time percentiles, time per context), where the frame time goes (main thread / render
thread / eye stages / GPU), the MARK NOW moments with the seconds before them, the worst seconds, the biggest single-frame
spikes, the slowest places on the map and the clock / temperature / battery timeline. Python 3 standard library only.
"""
import argparse
import bisect
import csv
import glob
import math
import os
import statistics
import sys
from collections import defaultdict

WEAPONS = ["UNKNOWN", "PISTOL", "DUAL_PISTOL", "RIFLE", "REPEATER", "SNIPER", "SHOTGUN", "THROWN", "DYNAMITE", "BOW", "MELEE",
           "LASSO", "TURRET", "CANNON"]
FLAG_SYNTH, FLAG_HALF, FLAG_MENU, FLAG_CUT, FLAG_DEADEYE, FLAG_POS = 1, 2, 4, 8, 16, 32
STAGES = ["ldepth_us", "llight_us", "lcolor_us", "rdepth_us", "rlight_us", "rcolor_us"]


def find_profile(path):
    if os.path.isfile(path):
        return path
    pats = [os.path.join(path, "profile-*.csv"), os.path.join(path, "profiles", "profile-*.csv"),
            os.path.join(path, "**", "profile-*.csv")]
    for p in pats:
        hits = sorted(glob.glob(p, recursive=True), key=lambda f: os.path.basename(f))
        if hits:
            return hits[-1]
    sys.exit(f"no profile-*.csv under {path}")


def num(v):
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)  # also nan / inf (a position the engine had not set)
        except ValueError:
            return v


def fmean(values):
    # statistics.mean is exact (Fraction sums) and far too slow on ~700k frames of a 3 h profile
    values = values if isinstance(values, list) else list(values)
    return sum(values) / len(values) if values else 0


def load(path):
    cols, rows, meta = {}, defaultdict(list), {"cfg": {}}
    with open(path, encoding="utf-8", errors="replace") as fp:
        for raw in fp:
            line = raw.rstrip("\n")
            if not line:
                continue
            if line.startswith("#") and len(line) > 2 and line[1] in "FSTME" and line[2] == ",":
                cols[line[1]] = line[3:].split(",")
                continue
            if line.startswith("# cfg "):
                parts = line[6:].split(" ", 1)
                meta["cfg"][parts[0]] = parts[1] if len(parts) > 1 else ""
                continue
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta[k] = v
                if "start_local=" in line:
                    meta["start_local"] = line.split("start_local=", 1)[1].split(" file=")[0]
                continue
            parts = line.split(",")
            kind = parts[0]
            if kind == "T":
                threads = []
                for item in parts[2:]:
                    if ":" in item:
                        n, p = item.rsplit(":", 1)
                        threads.append((n, num(p)))
                rows["T"].append({"t_ms": float(parts[1]), "threads": threads})
            elif kind == "E":
                rows["E"].append({"t_ms": float(parts[1]), "what": parts[2] if len(parts) > 2 else ""})
            elif kind in cols:
                names = cols[kind]
                if len(parts) - 1 < len(names):
                    continue  # a torn last line (the game was killed mid-write)
                rows[kind].append({n: num(v) for n, v in zip(names, parts[1:])})
    for k in rows:
        rows[k].sort(key=lambda r: r["t_ms"])
    return meta, rows


def pct(values, p):
    if not values:
        return 0
    s = sorted(values)
    return s[min(len(s) - 1, max(0, int(math.ceil(p / 100.0 * len(s))) - 1))]


def context_of(f):
    fl = f["flags"]
    parts = []
    if f["game_state"] not in (3, -1):
        parts.append("LOADING" if f["game_state"] == 5 else f"STATE{f['game_state']}")
    if fl & FLAG_MENU:
        parts.append("MENU")
    if fl & FLAG_CUT:
        parts.append("CUTSCENE")
    if fl & FLAG_DEADEYE:
        parts.append("DEADEYE")
    foot = f["foot"]
    parts.append("ON FOOT" if foot == 2 else ("NO PLAYER" if foot < 0 else f"MOUNTED/OTHER({foot})"))
    held = f["held"]
    if held >= 0:
        parts.append(WEAPONS[held] if held < len(WEAPONS) else f"WEAPON{held}")
    return " ".join(parts)


def where(f):
    if not (f["flags"] & FLAG_POS) and f["x"] == 0 and f["z"] == 0:
        return "pos ?"
    return f"pos {f['x']:.0f},{f['y']:.0f},{f['z']:.0f}"


def frame_budget_us(frames):
    # display period: half rate (AppSW) 36 Hz pairs at 72 Hz; the median frame interval is the practical target
    med = statistics.median([f["frame_us"] for f in frames]) if frames else 27778
    return med


def verdict(frames, srow, budget):
    """Which side is the bottleneck in these frames (heuristic)."""
    if not frames:
        return "no stereo frames (loading / theater / menu)"
    fr = fmean(f["frame_us"] for f in frames)
    busy = fmean(f["busy_us"] for f in frames)
    rcpu = fmean(f["rcpu_us"] for f in frames)
    gpus = [f["gpu_us"] for f in frames if f["gpu_us"] > 0]
    gpu = fmean(gpus) if gpus else 0
    mainmax = fmean(f["main_max_us"] for f in frames)
    stages = fmean(sum(f[s] for s in STAGES) for f in frames)
    right = fmean(f["rdepth_us"] + f["rlight_us"] + f["rcolor_us"] for f in frames)
    mainframes = sum(f["main_frames"] for f in frames)
    maincpu = sum(f["main_cpu_us"] for f in frames) / mainframes if mainframes else 0  # CPU per game update
    notes = []
    if mainmax > 0.85 * fr:
        kind = "busy" if maincpu > 0.6 * mainmax else "mostly WAITING (render thread / streaming / locks)"
        notes.append(f"MAIN THREAD (game update {mainmax / 1000:.1f} ms of {fr / 1000:.1f} ms, CPU {maincpu / 1000:.1f} ms: {kind})")
    if busy > 0.85 * fr:
        if rcpu > 0.6 * busy:
            notes.append(f"RENDER THREAD (busy {busy / 1000:.1f} ms, cpu {rcpu / 1000:.1f} ms, eye stages {stages / 1000:.1f} ms, right eye {right / 1000:.1f} ms)")
        else:
            notes.append(f"render thread WAITS (busy {busy / 1000:.1f} ms but CPU only {rcpu / 1000:.1f} ms: blocked on the main thread / GPU fence / locks)")
    if gpu > 0.85 * fr:
        notes.append(f"GPU ({gpu / 1000:.1f} ms)")
    if srow:
        caps = []
        cmax = [srow.get(f"cpu{i}_max", -1) for i in range(4)]
        cmax = [c for c in cmax if c and c > 0]
        if cmax and max(cmax) < 2000:
            caps.append(f"CPU clock cap {max(cmax)} MHz (battery / heat)")
        if 0 < srow.get("gpu_max_mhz", -1) < 500:
            caps.append(f"GPU clock cap {srow['gpu_max_mhz']} MHz")
        if srow.get("gpu_tpl", 0) and srow.get("gpu_tpl", 0) > 0:
            caps.append(f"thermal level {srow['gpu_tpl']}")
        if caps:
            notes.append("THROTTLE: " + ", ".join(caps))
    if not notes:
        notes.append(f"no single side saturated (busy {busy / 1000:.1f} / main {mainmax / 1000:.1f} / gpu {gpu / 1000:.1f} ms)")
    return "; ".join(notes)


_keys = {}


def nearest(rows, t_ms, window_ms=1500):
    # rows are sorted by t_ms (load); bisect instead of a scan: the per-second table asks once per second of a long session
    keys = _keys.get(id(rows))
    if keys is None or len(keys) != len(rows):
        keys = _keys[id(rows)] = [r["t_ms"] for r in rows]
    i = bisect.bisect_left(keys, t_ms)
    best, bd = None, window_ms
    for j in (i - 1, i):
        if 0 <= j < len(rows):
            d = abs(keys[j] - t_ms)
            if d <= bd:
                best, bd = rows[j], d
    return best


def top_threads(trows, t_ms):
    r = nearest(trows, t_ms, 2500)
    if not r:
        return ""
    return " ".join(f"{n}:{p}%" for n, p in r["threads"][:4])


def fmt_s(srow):
    if not srow:
        return ""
    cpu = "/".join(str(srow.get(f"cpu{i}_mhz")) for i in range(4) if srow.get(f"cpu{i}_mhz", -1) > 0)
    return (f"gpu {srow['gpu_mhz']}MHz(max {srow['gpu_max_mhz']}) {srow['gpu_temp10'] / 10 if srow['gpu_temp10'] > 0 else '?'}C "
            f"cpu {cpu}MHz batt {srow['batt_pct']}%{' CHG' if srow['charging'] == 1 else ''} proc {srow['proc_cpu_pct']}%")


def per_second(frames):
    sec = defaultdict(list)
    for f in frames:
        sec[int(f["t_ms"] // 1000)].append(f)
    return sec


def describe_window(title, frames, srows, trows, t_mid):
    print(f"  {title}")
    if not frames:
        print("    no stereo frames in this window (loading screen, theater cutscene or pause menu)")
        return
    sec = per_second(frames)
    for s in sorted(sec):
        fs = sec[s]
        worst = max(fs, key=lambda f: f["frame_us"])
        print(f"    t={s:>6d}s fps {len(fs):>3d}  worst {worst['frame_us'] / 1000:6.1f} ms  main {fmean(f['main_max_us'] for f in fs) / 1000:5.1f}"
              f"  busy {fmean(f['busy_us'] for f in fs) / 1000:5.1f}  gpu {fmean([f['gpu_us'] for f in fs if f['gpu_us'] > 0] or [0]) / 1000:5.1f} ms"
              f"  {context_of(worst)}  {where(worst)}")
    srow = nearest(srows, t_mid, 2000)
    print(f"    -> {verdict(frames, srow, frame_budget_us(frames))}")
    if srow:
        print(f"    clocks: {fmt_s(srow)}")
    th = top_threads(trows, t_mid)
    if th:
        print(f"    busiest threads: {th}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="profile-*.csv or a folder that contains one")
    ap.add_argument("--top", type=int, default=12, help="rows in the worst-seconds / spikes / places lists")
    ap.add_argument("--seconds-csv", help="also write one row per second to this CSV")
    ap.add_argument("--cell", type=float, default=100.0, help="map cell size in metres for the slow-places list")
    a = ap.parse_args()
    path = find_profile(a.path)
    meta, rows = load(path)
    F, S, T, M, E = rows["F"], rows["S"], rows["T"], rows["M"], rows["E"]
    print(f"PROFILE {path}")
    print(f"  started {meta.get('start_local', '?')}, {len(F)} frames, {len(S)} s of clock samples, {len(M)} marks")
    for k, v in meta["cfg"].items():
        print(f"  {k}: {v[:300]}")
    ends = [e["what"] for e in E if e["what"] in ("END", "FULL", "WRITE_ERROR")]
    if not ends:
        print("  (no END row: the game was closed / killed while recording - the file is still complete up to the last 0.5 s)")
    if not F:
        print("no frame rows")
        return
    dur = (F[-1]["t_ms"] - F[0]["t_ms"]) / 1000.0
    ft = [f["frame_us"] for f in F]
    med = statistics.median(ft)
    print("\nOVERVIEW")
    print(f"  {dur / 60:.1f} min of stereo frames, mean {len(F) / max(dur, 1e-3):.1f} FPS (game frames = new stereo pairs)")
    print(f"  frame time p50 {med / 1000:.1f}  p90 {pct(ft, 90) / 1000:.1f}  p99 {pct(ft, 99) / 1000:.1f}  max {max(ft) / 1000:.1f} ms")
    hitch = [f for f in F if f["frame_us"] > 1.5 * med]
    print(f"  {len(hitch)} frames > 1.5x median ({100.0 * len(hitch) / len(F):.1f} %), {sum(1 for f in F if f['frame_us'] > 100000)} frames > 100 ms")
    synth = sum(1 for f in F if f["flags"] & FLAG_SYNTH)
    half = sum(1 for f in F if f["flags"] & FLAG_HALF)
    print(f"  AppSW synthesizing in {100.0 * synth / len(F):.0f} % of frames, half rate in {100.0 * half / len(F):.0f} %")
    ctx = defaultdict(list)
    for f in F:
        c = context_of(f)
        c = " ".join(w for w in c.split() if w not in WEAPONS and not w.startswith("WEAPON"))
        ctx[c].append(f)
    print("  by context:")
    for c, fs in sorted(ctx.items(), key=lambda kv: -len(kv[1])):
        secs = sum(f["frame_us"] for f in fs) / 1e6
        print(f"    {c:<34s} {secs / 60:6.1f} min  {len(fs) / max(secs, 1e-3):5.1f} FPS  p99 {pct([f['frame_us'] for f in fs], 99) / 1000:6.1f} ms")
    wctx = defaultdict(list)
    for f in F:
        wctx[WEAPONS[f["held"]] if 0 <= f["held"] < len(WEAPONS) else "NO WEAPON"].append(f)
    print("  by held weapon:")
    for c, fs in sorted(wctx.items(), key=lambda kv: -len(kv[1])):
        secs = sum(f["frame_us"] for f in fs) / 1e6
        print(f"    {c:<34s} {secs / 60:6.1f} min  {len(fs) / max(secs, 1e-3):5.1f} FPS")

    print("\nWHERE THE FRAME TIME GOES (means per stereo frame, ms)")
    def m(key, fs=F):
        return fmean(f[key] for f in fs) / 1000

    gpus = [f["gpu_us"] for f in F if f["gpu_us"] > 0]
    print(f"  frame {m('frame_us'):.1f} = render busy {m('busy_us'):.1f} + paced sleep {m('slept_us'):.1f};  render CPU {m('rcpu_us'):.1f}")
    print(f"  eye stages: L depth {m('ldepth_us'):.1f} light {m('llight_us'):.1f} color {m('lcolor_us'):.1f} | R depth {m('rdepth_us'):.1f} light {m('rlight_us'):.1f} color {m('rcolor_us'):.1f}")
    print(f"  main thread: worst update per frame {m('main_max_us'):.1f}, CPU {m('main_cpu_us'):.1f}, {fmean(f['main_frames'] for f in F):.2f} updates per frame")
    print(f"  GPU (engine frame span) {fmean(gpus) / 1000 if gpus else 0:.1f}  p99 {pct(gpus, 99) / 1000:.1f}")
    print(f"  -> {verdict(F, S[len(S) // 2] if S else None, med)}")

    print("\nMARKS (MARK NOW presses; the window ends at the menu open = when the drop was seen)")
    if not M:
        print("  none")
    for mk in M:
        t_open = mk["menu_open_t_ms"]
        win = [f for f in F if t_open - 10000 <= f["t_ms"] <= t_open + 1000]
        describe_window(f"MARK {mk['mark']} at {mk['t_ms'] / 1000:.1f} s (menu opened {t_open / 1000:.1f} s):", win, S, T, t_open - 2000)

    sec = per_second(F)
    full = [s for s in sec if s - 1 in sec and s + 1 in sec]  # skip edge seconds (loading in / out)
    print(f"\nWORST SECONDS (lowest FPS, top {a.top})")
    worst = sorted(full, key=lambda s: (len(sec[s]), -max(f["frame_us"] for f in sec[s])))[:a.top]
    for s in sorted(worst):
        fs = sec[s]
        w = max(fs, key=lambda f: f["frame_us"])
        srow = nearest(S, s * 1000 + 500, 1500)
        print(f"  t={s:>6d}s ({s / 60:5.1f} min) fps {len(fs):>3d} worst {w['frame_us'] / 1000:6.1f} ms  {context_of(w)}  {where(w)}")
        print(f"      {verdict(fs, srow, med)}")
        if srow:
            print(f"      {fmt_s(srow)}  |  {top_threads(T, s * 1000 + 500)}")

    print(f"\nSPIKES (longest single frames, top {a.top})")
    for f in sorted(F, key=lambda f: -f["frame_us"])[:a.top]:
        st = sum(f[s] for s in STAGES)
        print(f"  t={f['t_ms'] / 1000:8.1f}s {f['frame_us'] / 1000:7.1f} ms: busy {f['busy_us'] / 1000:.1f} cpu {f['rcpu_us'] / 1000:.1f} stages {st / 1000:.1f}"
              f" (R {(f['rdepth_us'] + f['rlight_us'] + f['rcolor_us']) / 1000:.1f}) main max {f['main_max_us'] / 1000:.1f} x{f['main_frames']} gpu {f['gpu_us'] / 1000:.1f}"
              f"  {context_of(f)}  {where(f)}")

    cells = defaultdict(list)
    for f in F:
        if f["flags"] & FLAG_POS or f["x"] or f["z"]:
            if math.isfinite(f["x"]) and math.isfinite(f["z"]):
                cells[(int(f["x"] // a.cell), int(f["z"] // a.cell))].append(f)
    rated = []
    for c, fs in cells.items():
        secs = sum(f["frame_us"] for f in fs) / 1e6
        if secs >= 5:
            rated.append((len(fs) / secs, secs, c, fs))
    print(f"\nSLOWEST PLACES ({a.cell:.0f} m cells with >= 5 s, top {a.top}; x,z = map, y = height)")
    for fps, secs, c, fs in sorted(rated)[:a.top]:
        cx, cz = (c[0] + .5) * a.cell, (c[1] + .5) * a.cell
        y = fmean(f["y"] for f in fs)
        print(f"  {cx:7.0f},{y:5.0f},{cz:7.0f}  {fps:5.1f} FPS over {secs:6.0f} s  p99 {pct([f['frame_us'] for f in fs], 99) / 1000:6.1f} ms  ({verdict(fs, None, med)})")

    if S:
        print("\nCLOCKS / HEAT / BATTERY (per minute)")
        per_min, f_min = defaultdict(list), defaultdict(list)
        for s in S:
            per_min[int(s["t_ms"] // 60000)].append(s)
        for f in F:
            f_min[int(f["t_ms"] // 60000)].append(f)
        for mnt in sorted(per_min):
            ss = per_min[mnt]
            fs = f_min.get(mnt, [])
            secs = sum(f["frame_us"] for f in fs) / 1e6
            last = ss[-1]
            cpu = "/".join(str(max(x.get(f"cpu{i}_max", -1) for x in ss)) for i in range(4) if last.get(f"cpu{i}_max", -1) > 0)
            print(f"  min {mnt:4d}: {len(fs) / secs if secs > 0 else 0:5.1f} FPS  gpu {fmean(x['gpu_mhz'] for x in ss):4.0f} MHz (cap {min(x['gpu_max_mhz'] for x in ss)})"
                  f"  cpu cap {cpu}  temp {max(x['gpu_temp10'] for x in ss) / 10:.1f} C  tpl {max(x['gpu_tpl'] for x in ss)}"
                  f"  batt {last['batt_pct']}%{' CHG' if last['charging'] == 1 else ''}  rss {last['rss_mb']} MB  dropped {last['dropped']}")

    if a.seconds_csv:
        with open(a.seconds_csv, "w", newline="", encoding="utf-8") as out:
            w = csv.writer(out)
            w.writerow(["t_s", "fps", "worst_ms", "busy_ms", "main_max_ms", "gpu_ms", "right_eye_ms", "context", "x", "y", "z", "gpu_mhz", "cpu_mhz", "temp_c", "batt"])
            for s in sorted(sec):
                fs = sec[s]
                wf = max(fs, key=lambda f: f["frame_us"])
                srow = nearest(S, s * 1000 + 500, 1500) or {}
                gp = [f["gpu_us"] for f in fs if f["gpu_us"] > 0]
                w.writerow([s, len(fs), wf["frame_us"] / 1000, round(fmean(f["busy_us"] for f in fs) / 1000, 2),
                            round(fmean(f["main_max_us"] for f in fs) / 1000, 2), round(fmean(gp) / 1000, 2) if gp else 0,
                            round(fmean(f["rdepth_us"] + f["rlight_us"] + f["rcolor_us"] for f in fs) / 1000, 2), context_of(wf),
                            wf["x"], wf["y"], wf["z"], srow.get("gpu_mhz", ""), srow.get("cpu2_mhz", srow.get("cpu0_mhz", "")),
                            srow.get("gpu_temp10", 0) / 10 if srow else "", srow.get("batt_pct", "")])
        print(f"\nper-second table written to {a.seconds_csv}")


if __name__ == "__main__":
    main()
