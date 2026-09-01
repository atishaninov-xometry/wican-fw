# ND3 AutoPID custom profile — notes & reasoning

Reference/debug notes for `nd3_wican_autopid.json` (WiCAN Pro AutoPID → Vehicle Specific tab).
The JSON itself is kept comment-free; everything non-obvious lives here.

## What's in the active profile (7DF-only)
| PID (req) | Param | Expression | Notes |
|-----------|-------|------------|-------|
| `012F` | `FuelLevel_L` / `FuelEmpty_L` | `B3*45/255` / `45-(B3*45/255)` | 45 = assumed tank litres; recalibrate (see below) |
| `01A4` | `GearStatus` | `B4/16` | 0 = Neutral/clutch-in, 1..6 = engaged gear |
| `0165` | `RecommendedGear` | `B4/16` | 0 = none, 2..6 = shift-up hint |

All use `pid_init: ATSH7DF` (functional engine header) + trailing frame-count digit on the request (`012F` → `012F1`). Per-param `period` (ms) is carried in this file: FuelLevel/FuelEmpty 10000, GearStatus 1000, RecommendedGear 2000.

## ⚠️ Import FORMAT — must be the array/"converted" form, NOT shorthand
The file uses the full `parameters: [ {name, expression, unit, class, min, max, type, period, send_to} ]` array form. **Do NOT use the shorthand `parameters: {Name: "expr"}` map** — the web UI's importer detects shorthand ("Shorthand single-profile detected. Loading parameter metadata…") and then does an **un-timed `fetch` to `raw.githubusercontent.com/.../.vehicle_profiles/params.json`** (main.js:468). Offline (phone on the WiCAN AP with no internet route), that await HANGS forever — the import never completes and nothing is stored. The array form hits main.js:454 (`Array.isArray(parameters) → return false`) so it skips the fetch and imports offline. This is why the earlier trimmed shorthand JSON "loaded parameter metadata" then died.

## ⚠️ Why the DSC (760) PIDs were removed — header conflict
On the v4.51 / IDF 6 build, a 52-min drive log (`obd_log_20260827_213421`) showed **every Standard-PIDs-tab engine PID (RPM/speed/throttle/…) logged exactly once at startup, then stopped for the whole session**, while only the 7DF custom PIDs kept logging (~6 s).

Cause: a **760 (DSC) PID leaves the ECU header on the chassis module**, and the Standard-PIDs-tab PIDs have no per-poll header reset — so after the first 760 poll they query the wrong ECU and get NO DATA for the rest of the drive. The 7DF custom PIDs survive because each one re-sends `ATSH7DF`.

**Fix = keep the custom profile 7DF-only.** Dropped: Brake_Pressure, Steering_deg, Steer2034_deg, Steer201D_raw, Steer201F_raw (all 760), plus ambient (7DF but NO DATA). CONFIRMED on the 08-29 drives: with vehicle-specific off/7DF-only, the standard PIDs log continuously again (no more one-sample stall).

## Poll rate / logging cadence — capped by the sequential poll CYCLE, not by log_period or per-PID Period
Last clean session (`obd_log_20260827_213421`, 08-29 15:51→16:18, vehicle-specific off): every standard PID logs at **medgap 10 s** uniformly (RPM 10×112/11×11), even with RPM/speed/throttle set to Period=1000 and coolant/fuel to 10000. So the per-PID Period makes NO difference.

Ruled out: `log_period` was = 1 the whole time (user-confirmed); the std-PID Period IS parsed (`json_item_to_u32` handles the UI's string, autopid_config.c:181) and applied to `param->timer`; a NO-DATA PID does NOT stall 12 s (autopid_parser pushes "error" fast, autopid.c:2806).

**Real cause = the AutoPID poll loop is strictly sequential** (autopid.c ~3890+): one request → wait response → 100 ms gap (line 4095), for EVERY enabled PID, over the slow ND3 gateway. With the current enabled set, one full pass ≈ 10 s. A PID's timer can't fire faster than one full pass, and since the pass (~10 s) exceeds every configured Period, all timers are always expired by the next pass → everything samples at ~10 s and the 1000-vs-10000 distinction is erased. **Effective rate ≈ max(per-PID Period, full-cycle time), and full-cycle time dominates.**

**FIX = enable FEWER PIDs.** Cycle time ≈ (enabled PID count) × (~per-request round-trip on ND3). To get RPM/speed/throttle near ~1–2 s, enable ONLY those few and disable everything else (the slow-changing ones you set to 10 s, plus the NO-DATA `9D-EngineFuelRate`). True 1 Hz for a large PID set is not achievable at the OBD port — it's one sequential request/response on one CAN channel; only raw broadcast frames (internal bus tap, which ND3 doesn't expose at OBD) give many signals at high rate.

To bring **brake** back, the firmware needs a header reset before each standard-PID poll (TODO, not done). Once fixed, re-add:
```json
{ "pid": "222B0D1", "pid_init": "ATSH760;ATFCSH760;ATFCSD300000;ATFCSM1;", "parameters": { "Brake_Pressure": "[S4:S5]" } }
```
`Brake_Pressure` = raw signed pressure (real ~0–217; the 0x8000 no-reading sentinel reads as −32768 → filter negatives). The `/2.3` % conversion was dropped per request.

## Steering — dead at the OBD port (confirmed)
On-car test (2026-08-27): `22 2033` and `22 2034` **respond** (web-UI Test showed "Expression eval failed" = data arrived, not "NO DATA") but their values **never vary**; `22 201D` = constant `4`; `22 201F` = NO DATA. So the ND3 DSC does not expose live steering angle at the OBD port — it needs a behind-dash internal-bus tap. drewid74 saw the same (parked `760/20 20` cluster didn't vary).

Correct signed decode (if ever revisited): `Steering_deg = [S4:S5]/10` — **not** `([B4:B5]-(B4>127)*65536)/10`, which fails eval (see parser note).

## Ambient (01 46 @ 7DF, `A-40`)
drewid74 confirms it works **with the engine running** (27–35 °C). On this car, tested parked, it returned NO DATA. It's 7DF-safe (no header conflict) — re-add and test engine-on if you want ambient temp:
```json
{ "pid": "01461", "pid_init": "ATSH7DF;", "parameters": { "AmbientAir_C": "B3-40" } }
```

## Expression parser capabilities (`main/expression_parser.c`)
- Operators: `+ - * /`, bitwise `& | ^`, shifts `<< >>`, parentheses.
- Operands: number, `V`, `Bn` (unsigned byte n), `Sn` (SIGNED byte n), `[Bn:Bm]` (unsigned multi-byte, big-endian), `[Sn:Sm]` (SIGNED multi-byte: int8/16/32/64 by width), `Bn:bit` (single bit).
- **NO** comparison operators (`< > ==`), **NO** functions (no floor/abs/if).
- For a signed value use `[Sn:Sm]` or `Sn` — never `(Bn>127)*65536` (fails eval; `>` isn't an operator).

## Byte index reference
mode01: A=B3, B=B4, C=B5, D=B6. mode22: A=B4, B=B5 (C=B6, D=B7).

## Gear decode
Gear is in the **high nibble of byte B** → `÷16`. Verified from a real log: GearStatus raw ∈ {0,16,32,48,64}, RecommendedGear raw ∈ {0,32,48,64,80,96} = 16×gear. The Standard-tab "A4 gear RATIO" is noisy on ND3 — use this enum.

## Fuel calibration (TODO)
`FuelLevel_L = B3 * CAP / 255`, `FuelEmpty_L = CAP − FuelLevel_L`, with `CAP` = usable tank litres. Currently 45 (assumed). To calibrate: `CAP = litres_added / (level%_after − level%_before)` from a known fill vs the `2F-FuelTankLevel` % readings.

## Source
Validated ND3 PID reference: https://github.com/drewid74/2024-nd3-mazda-obdii (local copies: `ND3_drewid74_README.md`, `ND3_nd3_candidates.csv`, `nd3-pid-reference.md`).
