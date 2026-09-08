# Fork notes

This is a personal fork of `meatpiHQ/wican-fw` (WiCAN Pro, ESP32-S3, ESP-IDF v6.0.2)
for offline OBD-II telemetry logging. Firmware itself is vehicle-agnostic; the only
vehicle-specific material is the AutoPID profile at
`vehicle_profiles/mazda/nd3-mx5.json` plus the notes below explaining where its numbers
came from. Everything in this file is background/rationale for future work on the
fork - not build instructions (see the repo's own docs for that) and not something the
firmware reads.

## Branch / CI

Active branch: `custom-fork` (base: upstream `v4.51p`, stable). CI
(`.github/workflows/build-firmware.yml`) builds on push to `custom-fork*` (or
`sd-logging-patches-*`, an older naming scheme from before this branch was cut) and
publishes a GitHub Release tagged `vYYYY.MM.DD_MICRO`, where MICRO counts existing
same-day tags so it resets at midnight UTC instead of climbing forever like a bare
`github.run_number` would.

## Vehicle profile: Mazda MX-5 ND3 (2024+)

`vehicle_profiles/mazda/nd3-mx5.json` is 7DF-only (functional engine header,
`ATSH7DF`) by design:

- **DSC (header 760) PIDs break the standard engine PIDs if left enabled** - once a
  760 query runs, standard PIDs have no per-poll header reset and start reading the
  wrong ECU for the rest of the drive. The DSC brake-pressure entry below works around
  this by using the **PCM (7E0)** instead, which needs no flow-control/header dance.
- **Gear decode**: gear is in the high nibble of byte B -> `B4/16`. GearStatus (PID
  01A4): 0=Neutral, 1-6=gear. RecommendedGear (PID 0165): 0=none, 2-6=shift-up hint.
  The Standard-tab "A4 gear ratio" PID is noisy on this car - use these instead.
- **Brake fluid pressure**: `22 280A` on header `7E0`, `[B4:B5]*100/128` in kPa
  (resolution 1/128 bar). Source: https://gist.github.com/agronick/4d01cfe7f94dd41eeeb46f5e5dd204b8,
  confirmed against a Car Scanner reading of 8689 kPa (raw 11122, ×100/128 = 8689.06
  exactly). Superseded an earlier `22 2B0D` on header 760 whose unit was never
  established and which produced impossible (>100%) values at max pedal effort.
- **Steering is dead at the OBD port** on this car - `22 2033`/`22 2034` respond but
  never vary, `201D` is constant, `201F` is NO DATA. Needs an internal bus tap, not
  firmware.
- **Ambient temp (`01 46`)** returned NO DATA parked; untested with the engine
  running.
- **Fuel calibration** (`FuelLevel_L = B3*49/255`, `FuelEmpty_L = (255-B3)*49/255`):
  measured 2026-09-06 from a 10.0 L fill, bracketed by a clean 2m10s data gap
  (key-off-to-refuel) in `obd_log_20260906_131801_000.db`. B3 sat at a settled 176 for
  ~19s before the fill and 228 recurs as the post-fill ceiling (dips below are slosh);
  both anchors are good to about ±1 count. 52 counts = 10.0 L -> 0.1923 L/count -> byte
  255 = 49.04 L (±2 counts -> 47.2-51.0 L). The prior `45/255` guess read the same fill
  as 9.18 L, 8.2% low. Anchored over 33.85-43.85 L; linearity below half a tank is
  assumed, not measured - a second fill from near-empty would confirm the low end.
  Don't use moving readings to sanity-check this: the same unchanged tank read
  anywhere from 138-194 raw while driving in the 70s before the stop (slosh).
- Import format for AutoPID profiles must be the array/converted form
  (`parameters: [{name, expression, ...}]`), not the shorthand map form
  (`parameters: {Name: "expr"}`) - the web UI detects shorthand and does an untimed
  fetch to GitHub for parameter metadata, which hangs forever with no internet route
  (e.g. a phone on the WiCAN's own AP with no upstream connection).
- Expression parser (`main/expression_parser.c`) has no comparison operators or
  functions - use `[Sn:Sm]`/`Sn` for a signed value, never `(Bn>127)*65536`.
- Reference: https://github.com/drewid74/2024-nd3-mazda-obdii (ND3 = CX-60-generation
  electronics; the OBD port only exposes diagnostic req/resp, no raw broadcast frames,
  so anything not reachable via a PID needs an internal bus tap).

## AutoPID batching: the validator bug that hid batching in testing

Batching multiple Mode-01 PIDs into one request (`autopid_poll_std_pids_batched()`)
looked confirmed working from manual Terminal-tab testing, but every batch failed
silently on a real drive. Root cause: `autopid_validate_response_for_cmd()` expected a
multi-byte identifier's extra bytes to sit contiguously right after the positive-
response byte (correct for something like Mode 22 `22 B0 02` -> `62 B0 02`) - but a
batched Mode-01 reply is `41 PID1 data... PID2 data...`, with data *between* the PIDs,
never contiguous. The Terminal-tab path that "confirmed" it bypasses this validator
entirely (`elm327_run_command`, not `autopid_parser`), so the bug was invisible until
the real poll loop exercised it. Fixed by special-casing `service==0x01 && req_len>2`
to just check for a `0x41` positive-response byte, deferring to the batching poller's
own per-PID resync-checked walk for correctness.

Measured round-trip on this car's gateway: ~34-37ms per batched request, flat across
1-5 PIDs (273/273 succeeded on a post-fix drive) - so a small RPM/speed/gear batch is
nowhere near latency-limited, but a large enabled-PID set still costs one full
sequential pass per PID group.

## Why every logging timestamp is UTC

The RTC (RX8130, no backup supply - any power loss returns undefined register
content) and the logger used to interpret time inconsistently: `rtcm_set_time_zone()`
fetched a *local* UTC offset from worldtimeapi.org's IP geolocation and applied it via
`setenv("TZ", ...)`, while the RTC was written/read with `localtime_r()`/`mktime()`.
Since FATFS's `get_fattime()` also uses `localtime_r()`, this meant SD card file
timestamps (and the RTC's own stored value) drifted with whatever timezone happened
to be active at the moment - visible as a phone reading a card's file times as if they
were its own local zone, when the device had actually written a different zone's wall
clock into that same field. Fixed by pinning the process timezone to UTC0 for the
entire process lifetime (set as the first statement in `app_main()`, before anything
touches the clock) and switching every RTC/logger time function to
`gmtime_r()`/a UTC-safe `timegm()` replacement (`rtcm_timegm()` - this libc has no
`timegm()`) instead of the local-time equivalents. The RTC now always stores true
UTC, and there's no remaining code path that changes the process timezone.

## Millisecond-resolution logging rework

Sub-second logging was impossible regardless of configured intervals, for several
compounding reasons, all fixed together:

1. `param_data.timestamp` held Unix seconds via `rtcm_get_unix_timestamp()` (an I2C
   read of the RTC per call). Now holds epoch **milliseconds** from `gettimeofday()`
   (the system clock is set from the RTC at boot). Query/display code that reads this
   column divides by 1000.
2. One timestamp used to be bound once per flush, so every row in a batch shared it -
   now bound per row.
3. Only the newest value per parameter survived a flush, throwing away a fast sample
   rate at write time. Samples now accumulate in a PSRAM double buffer and all of them
   are written.
4. Sampling used to be a timer re-reading a JSON snapshot of all params. Samples are
   now pushed from the acquisition path itself (`obd_logger_record_sample()`, called
   from the single funnel every accepted value passes through), so a row is stamped
   when the value arrived, not when a tick noticed it.
5. The poll loop slept 100ms at the end of every pass, capping every PID at ~10Hz
   regardless of its own configured period; dropped to a couple of ms.
6. Every re-armed poll timer got ±100ms random jitter, making any period under ~200ms
   meaningless; jitter now caps at period/4.
7. A DB rotation used to leave `param_info` empty while the RAM lookup kept the old
   per-file ids, so every rotated file's rows resolved to no parameter - fatal at a
   fast sample rate, since rotation happens every few minutes. Params are now
   re-inserted and ids refreshed on rotation, with buffered samples carrying a lookup
   index resolved to a row id at write time under the DB mutex.
8. The logger task is now a flush supervisor: it flushes on the write interval or when
   the buffer crosses a high-water mark, so a fast sample rate can't overrun a slow
   write interval.

Hard physical limit that remains: the gateway answers in ~35ms per request, so fresh
data caps around ~28Hz regardless of timestamp precision - two rows 10ms apart can
only differ if the gateway actually answered in between.

## Mixed epochs in one log file when the RTC boots wrong

A file with month/day `15`/`00` in its name held 1035 rows dated 2033-02-28 then
24470 dated the real date, with one clean transition partway through: the RTC read
garbage at boot, the logger stamped rows from it, and time sync corrected the system
clock once the station link came up. A plausible-range check alone doesn't catch this
(2033 is inside any sane window). Fixed by anchoring the wall clock against a
monotonic timer; a jump larger than a couple of seconds against elapsed monotonic time
is treated as a clock correction, and the same delta is applied to both the buffered
samples in RAM and the rows already written to the currently-open file - but only to
the rows *this session* wrote (`WHERE rowid >= session_first_rowid`, recorded whenever
the DB is opened: boot, rotation, remount). That bound matters because the DB manager
reuses whatever file `db_index.json` points at rather than starting a fresh one per
boot, so at a 128MB rotation limit the open file can already hold weeks of correctly
stamped rows from earlier sessions; an unbounded `UPDATE param_data SET timestamp =
timestamp + delta` would shift all of them (and rewrite ~5M rows while holding the DB
mutex). A correction still queued when a rotation happens is dropped rather than
applied - its rows are unreachable in the closed file, and the new file's rows are
already on the corrected epoch. Rows in an already-rotated file keep their original
(wrong) epoch. The filename itself still comes from whatever
the clock said at creation time, so a file written across such a boot can still be
named wrongly even though its rows end up correct.

## SD logging facts

- SQLite under `/sdcard/obd_logs`, decoded PIDs only, delta-logging (only changed
  values written), `synchronous=OFF` during inserts - nothing is durable until
  checkpoint/unmount, hence the safe-eject/sleep-flush/auto-remount handling.
- Param IDs are per-file (each rotated `.db` recreates `param_info`) - map through
  each file's own `param_info` when merging across files. `db_index.json` is the file
  manifest, and also names the `current_db` that the next boot **reopens and appends
  to** - a `.db` is not one session, and at 128MB/file it can hold many drives, so its
  filename's timestamp is only the moment the file was created.
- Extract a log cleanly: engine-off (auto-flush) or safe-eject (solid green LED)
  before downloading, else a live download of the active `.db` can be 0 bytes or
  missing WAL rows.
