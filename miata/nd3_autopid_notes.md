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

**Brake fluid pressure — 22 280A on 7E0, in kPa (identified 2026-09-04).** Car Scanner's "Mazda SkyActiv" profile shows *Brake Fluid Pressure* in kPa, reading ~8689 kPa at a very hard press. Source found: the [Mazda 3 PID list](https://gist.github.com/agronick/4d01cfe7f94dd41eeeb46f5e5dd204b8) documents **header `7E0`, identifier `22 28 0A`, bytes A,B, `(A*256+B)*100/128`, kPa**. The reading confirms it exactly: 8689.06 kPa requires raw **11122**, and 11122 × 100/128 = 8689.0625. Resolution is 100/128 = **0.78125 kPa/bit** (1/128 bar). Profile entry:
```json
{ "pid": "22280A1", "pid_init": "ATSH7E0;",
  "parameters": [ { "name": "BrakePressure", "expression": "[B4:B5]*100/128",
                    "unit": "kPa", "min": 0, "max": 20000, "period": 200 } ] }
```
`max: 20000` filters garbage and the 0x8000 sentinel (which would read 25600 kPa); real full-effort pressure is ~8700 kPa (87 bar), so there is plenty of headroom. Note this is the **PCM (7E0)**, not the DSC — so no 760 header and no flow-control setup, just one `ATSH7E0`, which is why 200ms is affordable here.

**Superseded: `22 2B 0D` on 760.** That was the previous brake entry, and its unit was never established. drewid74's reference documents it as *"Brake position ... max(0, signed_int16(A,B))/2.3 | % | 0 → 0.43 (light tap)"* — an unexplained divisor fitted to a light tap, with no calibration. Our 2026-09-04 log reached raw **282** at maximum effort, which `/2.3` renders as 122.6% — impossible, which is how the error surfaced. It is a real signal (1 raw/bit, exact 0 at rest, clean pedal trace) but ~39x coarser than 280A and of unknown unit, so it was dropped in favour of the properly scaled PCM value. `git log` this file for the old entry if it's ever wanted.

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

## Fuel calibration (DONE — measured 2026-09-06 with a 10.0 L fill)
`FuelLevel_L = B3*49/255`, `FuelEmpty_L = 45-(B3*49/255)`.

Measured, not assumed. A 10.0 L fill with the engine idling and the car stationary on both sides of it:

| | raw B3 | `2F` % | litres @ 49/255 |
|---|---|---|---|
| before (settled, 13:21:01–13:21:40) | 176 | 69.02 | 33.8 |
| after (13:23:54, engine restarted) | 228 | 89.41 | 43.8 |
| Δ | **52** | 20.39 | **10.0** |

So **0.1923 L per sender count**, i.e. byte 255 = **49.0 L** (±3.4 L from the quantisation below). The old `45/255` guess read that same fill as 9.18 L — 8.2% low.

Three things corroborate it:
- The **highest B3 ever logged across every dump is 230** → 230 × 0.1923 = **44.2 L**, i.e. a physically full tank, against the ND's 45 L nominal spec. The sender's 0–255 range runs ~10% past the fill point, which is why a full tank reads ~90% and not 100%.
- The fill therefore took the tank from 33.8 L to 43.8 L — essentially full, which is exactly what 10.0 L into a 44 L tank at that level should do, and explains why the value pins at 228–230 while driving afterwards.
- Over the preceding 22-minute drive, `FuelRate` integrates to **0.59 L** = 3.1 counts, below the quantum at that level — and the level indeed did not move (median B3 = 176 in both sessions).

**Quantisation / why the uncertainty.** B3 only ever takes values off a lattice, and the step size *shrinks with level*: ~5 counts (≈0.96 L) mid-tank, 2–3 counts near full (215, 217, 219, 223, 226, 228, 230). That is the signature of a byte that is **linear in volume** with an underlying quantisation in float-arm *angle* — litres-per-degree is largest mid-tank and small once the arm is near its stop. (If B3 were linear in angle instead, the count step would be constant.) So the linear formula is the right shape; the residual error is ±2.5 counts on the "before" reading and ±1 on the "after", i.e. Δ = 52 ± 3.5 → full scale 46–53 L.

**Caveat:** the fill anchors the calibration over 33.8–43.8 L. Litres-per-count below half a tank is unverified. A second known fill from near-empty would pin the low end.

**Reading the number off a log:** `2F` is heavily damped — it held 176 for the entire idle before the fill and jumped straight to 228 on restart, and while driving it swings ±50 counts with slosh (one log ranged 103–228 on an unchanged tank). Only compare **stationary, engine-idling, settled** readings; on a moving car take the median, and near full take the maximum (slosh can then only read low).

## Source
Validated ND3 PID reference: https://github.com/drewid74/2024-nd3-mazda-obdii (local copies: `ND3_drewid74_README.md`, `ND3_nd3_candidates.csv`, `nd3-pid-reference.md`).
