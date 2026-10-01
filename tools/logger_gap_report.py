#!/usr/bin/env python3
"""Report data holes in a WiCAN OBD logger .db (read-only, stdlib only).

    python3 tools/logger_gap_report.py obd_log_20260919_083905_000.db [more.db ...]

A "dropout" is a stretch where EVERY parameter is silent although the car was
moving on both sides of it (VehicleSpeed > 0 and EngineRPM > 500 before and after)
- so it cannot be a parked car. Gaps with the car standing still are counted
separately (ignition-off stops, start-stop restarts); they are not data loss.
A key-off never logs a final RPM of 0, the ECU just stops answering, so "RPM was
still > 500" alone does not mean the engine was running. Two signatures matter:

  * 3s-tick dropouts: duration within a hair of a whole number of 3 s. The
    firmware reads the supply voltage every 3 s (sleep_mode.c) and the
    low-voltage pause options act on that reading, so a pause can only start and
    end on a tick. Finding these means polling was paused by voltage.
  * everything else: bus/ECU trouble, a short ignition-off, a reboot.

Files written before the "log every sample" change were delta-gated (an
unchanged value wrote no row), so a gap in a SINGLE slow parameter is ambiguous
there; only whole-file silences are reliable. Newer files write every sample.
"""
import collections
import datetime
import sqlite3
import sys

RPM_NAMES = ("EngineRPM",)
SPEED_NAMES = ("VehicleSpeed",)
MIN_GAP_S = 2.5          # shorter than this is ordinary scheduling jitter
MAX_GAP_S = 3600.0       # longer is a parked car, not a pause
RUNNING_RPM = 500.0
TICK_S = 3.0
TICK_TOL_LOW = 0.35      # how far below / above a tick multiple still counts
TICK_TOL_HIGH = 0.90     # (resume adds up to ~0.9 s of re-init latency)


def to_ms(ts):
    # Pre-millisecond logs stored seconds; 1e11 ms is 1973, 1e11 s is year 5138.
    return ts if ts > 1e11 else ts * 1000


def on_tick(duration_s):
    resid = duration_s - round(duration_s / TICK_S) * TICK_S
    return -TICK_TOL_LOW <= resid <= TICK_TOL_HIGH


def print_settings(con):
    """Settings the firmware had while this file was written (settings_log)."""
    try:
        rows = con.execute("SELECT timestamp, uptime_ms, source, key, old_value, new_value, event "
                           "FROM settings_log ORDER BY rowid").fetchall()
    except sqlite3.OperationalError:
        print("\nsettings_log: none (file written by firmware that predates it)")
        return
    snap = [r for r in rows if r[6] == "snapshot"]
    changes = [r for r in rows if r[6] == "change"]
    print(f"\nsettings_log: {len(snap)} settings listed, {len(changes)} changes")
    for r in changes:
        when = datetime.datetime.fromtimestamp(r[0] / 1000, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        old = "(unset)" if r[4] is None else r[4][:60]
        new = "(removed)" if r[5] is None else r[5][:60]
        print(f"  {when} UTC  {r[2]}: {r[3]}  {old} -> {new}")


def analyse(path):
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    names = dict(con.execute("SELECT Id, Name FROM param_info"))
    rpm_ids = {i for i, n in names.items() if n in RPM_NAMES}
    if not rpm_ids:
        print(f"{path}: no EngineRPM parameter, cannot tell when the engine ran")
        return

    speed_ids = {i for i, n in names.items() if n in SPEED_NAMES}

    rows_per_param = collections.Counter()
    active_s = 0.0
    dropouts = []                 # (start_ms, duration_s)  car moving both sides
    stops = []                    # (start_ms, duration_s)  car standing still
    pending = None                # [start_ms, gap_s, rpm_before, speed_before, end_ms]
    last_ts = None
    last_rpm = 0.0
    last_speed = 0.0
    first_ts = last_seen = None

    def settle(p, rpm_after, speed_after):
        if rpm_after > RUNNING_RPM:
            (dropouts if p[3] > 0 and speed_after > 0 else stops).append((p[0], p[1]))

    cur = con.execute("SELECT timestamp, param_id, value FROM param_data ORDER BY timestamp")
    for raw_ts, pid, value in cur:
        ts = to_ms(raw_ts)
        rows_per_param[pid] += 1
        if first_ts is None:
            first_ts = ts
        last_seen = ts
        # The row that ends a gap is the first of a burst; VehicleSpeed follows
        # within ~100 ms, so judge the car's state a second later.
        if pending is not None and pending[4] is not None and ts > pending[4] + 1000:
            settle(pending, pending_rpm_after, last_speed)
            pending = None
        if last_ts is not None:
            gap = (ts - last_ts) / 1000.0
            if gap < MIN_GAP_S:
                active_s += gap
            elif gap <= MAX_GAP_S and last_rpm > RUNNING_RPM and pending is None:
                pending = [last_ts, gap, last_rpm, last_speed, ts]
                pending_rpm_after = last_rpm
        if pid in rpm_ids:
            if pending is not None and pending[4] == ts:
                pending_rpm_after = value
            last_rpm = value
        elif pid in speed_ids:
            last_speed = value
        last_ts = ts
    if pending is not None:
        settle(pending, pending_rpm_after, last_speed)

    def day(ms):
        return datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc).strftime("%Y-%m-%d")

    span = (last_seen - first_ts) / 86400000.0 if first_ts else 0
    print(f"\n=== {path}")
    print(f"rows {sum(rows_per_param.values())}, span {span:.1f} days, "
          f"{day(first_ts)} .. {day(last_seen)}")
    print(f"time covered by rows (gaps < {MIN_GAP_S}s): {active_s / 3600:.1f} h")

    lost = sum(d[1] for d in dropouts)
    ticked = [d for d in dropouts if on_tick(d[1])]
    lost_t = sum(d[1] for d in ticked)
    driving_s = active_s + lost
    print(f"\ndropouts while driving (all parameters silent, {MIN_GAP_S}-{MAX_GAP_S:.0f}s): "
          f"{len(dropouts)} events, {lost / 3600:.2f} h = {100 * lost / driving_s:.1f}% of driving time")
    if dropouts:
        share = 100 * len(ticked) / len(dropouts)
        print(f"  on the 3s voltage tick: {len(ticked)} events ({share:.0f}%), {lost_t / 3600:.2f} h  "
              f"[chance level for a random duration: ~{100 * (TICK_TOL_LOW + TICK_TOL_HIGH) / TICK_S:.0f}%]")
        print(f"  other: {len(dropouts) - len(ticked)} events, {(lost - lost_t) / 3600:.2f} h")

        per_day = collections.defaultdict(lambda: [0, 0.0])
        for start, dur in dropouts:
            per_day[day(start)][0] += 1
            per_day[day(start)][1] += dur
        print("\n  per day (UTC):        events   minutes")
        for d in sorted(per_day):
            n, secs = per_day[d]
            print(f"    {d}   {n:9d}   {secs / 60:7.1f}")
    print(f"\nsilent gaps with the car standing still (not data loss): {len(stops)} events, "
          f"{sum(d for _, d in stops) / 3600:.2f} h")

    print("\nrows per parameter:")
    for pid, n in sorted(rows_per_param.items()):
        print(f"  {names.get(pid, pid):28s} {n:10d}")
    silent = [names[i] for i in names if rows_per_param.get(i, 0) == 0]
    if silent:
        print("  (no rows: " + ", ".join(silent) + ")")
    print_settings(con)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        analyse(p)
