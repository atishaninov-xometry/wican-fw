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
- **Human-friendly parameter names** (renamed from the original abbreviated set):
  `ThrottlePos` -> `ThrottlePlatePosition` (physical throttle-body sensor - on this
  car it never reaches 100% even at full pedal, topping out at ~92%), `ThrottleCmd`
  -> `AcceleratorPedalPosition` (PID `01 4C`, the ECU's *commanded* throttle - this
  is the one that actually reads 100% at full pedal effort, confirmed from a log
  where the pedal was held at max with the engine off/ignition on), `Gear` ->
  `CurrentGear`, `CoolantTemp` -> `CoolantTemperature`, `IntakeAirTemp` ->
  `IntakeAirTemperature`, `Battery` -> `BatteryVoltage`, `FuelPct` ->
  `FuelLevelPct`, `BrakePressure` -> `BrakeFluidPressure`. Names are used as MQTT
  JSON keys and as the logger's lookup key (`obd_logger_record_sample`), both plain
  string matches with no schema, so this is a profile-only rename with no firmware
  change - just re-import the JSON.
- **Fuel calibration** (`FuelLevelLiters = B3*50.5/255`, `FuelToEmptyLiters =
  (255-B3)*50.5/255`, renamed from `FuelLevel_L`/`FuelEmpty_L`), from two fills:
  - **2026-09-06, 10.0 L**: clean 2m10s key-off-to-refuel gap
    (`obd_log_20260906_131801_000.db`). B3 settled at 176 for ~19s before the fill,
    228 recurs as the post-fill ceiling. 52 counts = 10.0 L -> 0.1923 L/count -> byte
    255 = 49.04 L +/-1.89 (+/-2-count anchors).
  - **2026-09-12, 27.06 L, "fullest I can get it on flat surface", inside Berlin**:
    a clean 4m30s engine-off gap (`obd.db`, 16:44:24-16:48:54 local) matching both a
    GPS position hold and `BatteryVoltage` dropping to ~12.8V within seconds of the
    RPM->0 transition (alternator off, battery on its own) - confirming the identified
    stop is real and not a start-stop-system traffic-light blip (every other RPM=0 in
    that Berlin drive was under 45s; several of *those* brief stops sag to a similar
    ~12.7-12.9V too, since a lead-acid battery's post-charging voltage settles within
    seconds, not minutes - so voltage confirms the alternator state, but it's the
    stop's outlier *duration* that actually picks it out from city stop-start noise).
    Post-fill instant reading was raw **228 again** - the same ceiling as the first
    fill, on a different amount, on a different day, which is what actually pins 228
    as the sender's physical top rather than a one-off.
    First attempt at a pre-fill reading used the raw instantaneous samples right at
    shutdown and wrongly blamed "highway slosh" (sd 19.3 raw counts) for not matching
    the first fill's precision - wrong on two counts: this fill was city driving, not
    autobahn, and the car had already been stationary a couple of minutes before
    pumping started, so there should have been nothing left to slosh. The actual
    problem was mixing in samples taken *while still moving* through Berlin traffic.
    Filtering to only the samples where `VehicleSpeed==0` at that instant (32 of 111,
    from the preceding ~30 min) cuts the spread to sd 7.8 and gives a stable **median
    raw 92**, reproduced identically whether using all 32 stopped samples or just the
    7 from the last 5 minutes before shutdown. 136 counts = 27.06 L -> 0.1990 L/count
    -> byte 255 = 50.74 L +/-0.75.
  - **2026-09-15, 15.0 L.** Engine genuinely was shut off - first analysis pass
    wrongly concluded otherwise (see the errata note below). The tell that it
    really was a key-off: **three enormous `BrakePressure` spikes** (6871, 7179,
    6513 kPa at 20:57:12.6/19.9/24.3 local) that dwarf every ordinary driving-stop
    peak in the same session (300-1136 kPa) - the driver's own description of
    pressing the pedal hard three times before getting out. The last spike ends
    20:57:26.8; `EngineRPM` and `Battery` both go silent a few seconds later and
    stay silent for ~166-170s (20:57:26/30 -> 21:00:16), matching a real
    ignition-off, not idling. `VehicleSpeed`==0 the whole time (20:56:28-21:00:40,
    252s, still the longest stop that session by far).
    `VehicleSpeed==0`-filtered medians, before-window now extended to the true
    20:57:30 cutoff: before = raw 133 (13 samples over the preceding 30 min, sd
    6.2 - unchanged by the extra minute, the one additional sample lands mid-pack),
    after = raw 209 (12 samples over the following 30 min - settles there after one
    228 outlier right at pull-away). 76 counts = 15.0 L -> 0.1974 L/count -> byte
    255 = 50.33 L +/-1.32.
    This also resolves the "half tank -> almost full" confusion the fill prompted:
    against the raw 0-255 byte scale 133->209 is only 52%->82%, but against the
    sender's actual physical ceiling (228, confirmed three times now) it's
    58%->92% of *achievable* full - which is exactly "about half" to "almost full".
  - **Combined, all three fills** (inverse-variance weighted): **0.1979 L/count,
    byte 255 = 50.47 L +/-0.62** - the third fill lands within 0.2 L of the
    two-fill estimate above, so the constant in the profile (50.5) stands unchanged.
    Anchored over 16.8-43.9 L combined; linearity below ~17 L is still assumed, not
    measured.
  - Don't sanity-check a settled reading against samples taken while the car is
    moving - swings of 100+ raw counts from slosh alone are normal even in slow
    stop-start city traffic, not just highway driving.
  - **Errata, worth remembering:** the first pass on the third fill searched for a
    literal `EngineRPM`==0 *row* and found none, and wrongly concluded the engine
    had been left idling (a real, embarrassing miss - the driver said outright they
    shut it off). A genuine key-off doesn't reliably log an explicit 0 first: once
    ignition drops, the ECU stops answering entirely, so the log just goes silent -
    no 0 row, just a gap in *every* parameter, `Battery` included. Check for that
    gap, not for a `==0` transition. Large `BrakePressure` spikes right at the end
    of a drive are also worth scanning for in general - a hard, deliberate press
    reads nothing like ordinary braking-to-a-stop and can pinpoint the moment a
    driver parked to within a couple of seconds.
- **`TrueSpeed` (`= B3*1.018`, same `01 0D` PID/byte as `VehicleSpeed`, so no extra
  request)**: `VehicleSpeed` is the raw ECU wheel-speed PID, not the dash display, and
  it reads a couple percent *below* true ground speed on this car - the opposite
  direction from the dash (which is legally padded to never read low). Calibrated
  against a dashcam GPS log (`gps-visualize` format: local-time CSV, speed in
  **knots** - confirmed by regressing GPS-derived speed against the file's own speed
  field until the unit conversion made the slope land at 1) over a 2.5h Leipzig-Berlin
  drive including the car's ~200 km/h limiter plateaus. Best estimate from 38
  independent steady-speed segments (cruise/limiter stretches >=15s, GPS held within
  3 km/h): OBD reads **-1.77% vs GPS, 95% CI [-1.87%, -1.68%]** -> `1/0.9823 = 1.018`.
  Four other methods (regression, through-origin ratio, distance integral, per-bin
  ratio 5-210 km/h) agree within -1.5% to -2.2%, flat across the whole range with a
  small trend (-1.5% under 100 km/h, -1.9% above). Two independent GPS logs (2026-09-10
  and 2026-09-12) landed on the same dashcam-clock drift (~11-12s, corrected by
  cross-correlating the two speed series) and the same sign/magnitude of difference,
  so this isn't a one-off fluke - but it's still a single car's single set of tires at
  one point in time, not a general OBD/GPS constant; re-derive if tires are replaced.
  **Confirmed again 2026-09-12** on the return leg (Berlin-Leipzig, same day): the
  dashcam clock had *also* drifted differently on this leg (-1.3s vs the outbound
  leg's -11.9s, most likely a GPS time refix after the ~2.5h Berlin stop) - a single
  whole-day lag search only gets r=0.989 and must not be used; aligning each leg to
  its own best-fit lag recovers r=0.9998-0.99994 on both. With that fixed, two of the
  driver's own cruise/limiter holds landed exactly where predicted before analysis -
  the clearest confirmation this calibration has had:
  - "123 km/h" stretch, ~17:13 local: cleanest plateau (23s, GPS sd 0.26) gives
    GPS=119.47, OBD=117.47 (-1.67%); `OBD*1.018=119.58`, 0.09% from GPS.
  - "83 km/h" stretch, ~17:35 local: GPS=79.77, OBD=78.52 (-1.57%); `OBD*1.018=79.94`,
    0.21% from GPS.
  - Whole-day re-run, 82 independent plateaus (both legs): -1.83%, 95% CI
    [-1.89%,-1.78%] -> multiplier 1.019 - within noise of the 1.018 already in the
    profile, so left as-is.
  - Bonus, unasked-for but explains the original 3% guess: the dash/cruise **display**
    for those same two stretches was 123 and 83 - i.e. the *dash* reads about +3%
    high vs GPS (123/119.5, 83/80), the classic EU speedometer-tolerance direction.
    OBD's -1.8% and the dash's +3% are two different, opposite biases on the same
    wheel-speed signal - one upstream of the dash's legally-mandated padding, one
    downstream of it.
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

- SQLite under `/sdcard/obd_logs`, decoded PIDs only, **every accepted sample is
  written** (see "Holes in the data" below for why; files from before that change
  are delta-gated and only rows whose value moved exist), `synchronous=OFF` during
  inserts - nothing is durable until checkpoint/unmount, hence the
  safe-eject/sleep-flush/auto-remount handling.
- Param IDs are per-file (each rotated `.db` recreates `param_info`) - map through
  each file's own `param_info` when merging across files. `db_index.json` is the file
  manifest, and also names the `current_db` that the next boot **reopens and appends
  to** - a `.db` is not one session, and at 128MB/file it can hold many drives, so its
  filename's timestamp is only the moment the file was created.
- Extract a log cleanly: engine-off (auto-flush) or safe-eject (solid green LED)
  before downloading, else a live download of the active `.db` can be 0 bytes or
  missing WAL rows.

## Dashboard viewer: raw downloads must not depend on sql.js parsing

At 128MB/file (see rotation above), the dashboard's chart viewer (`dashboard.js`)
could hang the whole page and, with it, the download button: `DOMContentLoaded` used
to `await initSqlJs(...)` and then immediately auto-parse every `.db` matching the
default 7-day range through `new SQL.Database(arrayBuffer)`, which decodes the whole
file into a WASM heap in one synchronous call with no yield point. On a big file that
pegs the single JS main thread for a long time, and *everything* else on the page -
including the "Download Selected Database Files" button, which only existed because
of a `setTimeout(addDownloadButton, 1500)` racing that same parse - is stuck behind it
until it finishes. A big file was therefore locking users out of the one thing that
would have let them grab it and inspect it elsewhere.

Fixed by making the chart path fully opt-in and independent of the download path:
- `initSqlJs()` is now lazy (`ensureSqlJs()`, called only from
  `loadAndStoreDatabaseFile()`) - nothing sql.js-related runs until the user actually
  picks a range and hits Apply on the date picker. Page load no longer auto-calls
  `fetchRelevantDatabases()` at all.
- A new **Raw Database Files** panel lists every file from the `/obd_logs` index
  (filename, size, an actual browser `<a href download>` per file) - built directly
  from that small JSON, never sql.js, and rendered the moment the index arrives.
  `/obd_logs/<filename>` was already served chunked from the card
  (`obd_logger_db_file_handler`, `fread`/`httpd_resp_send_chunk` in a bounded loop,
  not buffered whole into RAM) - the ESP32 side was never the bottleneck, only the
  browser-side parse was.
- The zip-multiple-files download button moved out of the `setTimeout` guess and into
  `displayDatabaseInfo()`, called directly off the same index fetch - no more racing
  a parse that might still be running.
Net effect: opening the Logger Data tab does no parsing at all until asked, so a file
too large (or malformed) for sql.js to handle can still always be downloaded plain.

## Holes in the data: the low-voltage pause, and why unchanged values are now written

A week of ND3 driving (`obd_log_20260919_083905_000.db`, 3.1M rows, 37 h of
logged driving) had holes in every metric, most visibly the slow ones like fuel.
`tools/logger_gap_report.py <db>` reproduces everything below from a dump.

**Cause of the big holes: the Automate tab's "Low-Voltage Behavior".** Its form
default is "Pause PID polling only (Custom Voltage)" (`automate_threshold`,
`pid_polling_min_voltage` 13.1 V when unset), and storing the tab saves whatever the
select shows. Polling is skipped - and with "Pause Automate" the logger too - while
the WiCAN's own ADC reading of the OBD supply is below the threshold. That reading
is refreshed only every 3 s (`sleep_mode.c`), there is no debounce and no
hysteresis. The ND3's smart alternator cycles system voltage between about 12.5 V
and 14.2 V *while driving* (ECU `BatteryVoltage`: 25th percentile 13.08 V, 75th
13.96 V), so every low phase paused the adapter. Evidence, none of it circumstantial:
- 629 silences with the car moving on both sides (VehicleSpeed > 0, RPM > 500),
  **all 629 lasting a whole number of 3 s** (+0 .. +0.9 s for the resume re-init); a
  random duration would match ~42% of the time. Not the 5 s write interval (11%, below
  chance), not the poll rate, not the SD.
- ECU battery voltage just before/after those silences: median 12.86/12.89 V, with
  74%/67% of samples under 13.0 V, against 8% under 13.0 V over all engine-running
  time (median 13.23 V).
- It was absent on the first two days (12.5 h + 5 h of driving, with 45% of the
  voltage samples under 13.1 V on the first) and appears from 09-21 on (4 events on
  09-20), i.e. it was switched on by a config store or firmware change, not a
  property of the car alone. Which one, the dump cannot say.
- Cost: 7.9 h = 17.6% of driving time, every parameter equally (a 100 ms signal
  just looks dense elsewhere). Worst day 09-25: 313 events, 210 min. A silence
  ranged from one 3 s tick to 14 min; one of 7 min happened at 154-173 km/h.

**Fuel specifically** (10 s period, ~16.1k rows expected over the driving time, 10.9k
present = 68%): the pause accounts for ~2.8k of the ~5.2k missing rows, the rest is the
delta gate - a steady value wrote nothing, which looks identical to a lost sample.
The same gate is why the slow, steady signals looked worst: CoolantTemperature 40%
of expected rows, IntakeAirTemperature 52%, BatteryVoltage/Odometer ~70%. Fast
signals (RPM, load, pedal) only show the pauses.

**Fixes (firmware):**
1. `obd_logger_record_sample()` no longer has a "value unchanged" gate. Only the
   per-parameter spacing gate (Logger Sample Interval, default 10 ms) remains, so
   **a row means the ECU answered, and no row means no data.** Cost is roughly 3x the
   rows (~300 MB/week at this polling rate, ~19 h of driving per 128 MB file) - SD
   space was explicitly not a concern. Consequence for any reader: **files written
   before this change are delta-gated and a single file can contain both regimes**
   (the DB manager keeps appending to `current_db` across firmware updates), so a gap
   in one slow parameter is only trustworthy as "no data" for newer rows.
2. The low-voltage pause no longer applies while the vehicle is still answering
   (`autopid_vehicle_recently_answered()`: a value was accepted within
   `AUTOPID_VEHICLE_ALIVE_WINDOW_MS`, 15 s, monotonic clock). After key-off the ECU
   stops answering, 15 s later the voltage test applies as before, so the original
   purpose - not poking a shut-off car - is kept. Covers all three modes
   (Pause Automate, Sleep Voltage, Custom Voltage). Not hardware-tested: only CI-built.
   If it is ever wrong, the zero-risk switch is Automate -> Low-Voltage Behavior ->
   Disable.

**Not the cause, checked:** write path (silences are not on the 5 s flush grid; the
file has no rotation, `integrity_check` ok), polling speed, SD. There is no buffer
overrun at these rates (8192-entry buffer, ~70 rows/s, flushed every 5 s).

**Still unexplained, small:** on 09-19/09-20 there are ~120 back-to-back intervals of
~1.3 s between batches (a whole batch arrives in ~80 ms, then nothing for 1.3 s),
~170 s in total and gone from 09-21. Probably an earlier firmware build; not chased.

**Diagnostic trap worth remembering:** a key-off never logs a final `EngineRPM` of 0,
so "last RPM > 500 before a silence" does not mean the engine was running - 93
silences (7.1 h) in this file were the car standing still. Classify by car *moving*
on both sides before calling a silence a hole.
