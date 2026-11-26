# Dirt Delay v0.5.1

**English** | [日本語](README.ja.md)

A Schwung Audio FX (`audio_fx_api_v2`) that started from a Daisy Versio patch
(OD → OD/BC → Delay → 2 kHz LP → OD/BC → Delay → SVF LP → Tap Boost, Dry/Wet).
44.1 kHz / 128 frames / stereo int16, left and right processed independently.

In v0.3 the original's two low-passes (the 2 kHz OnePole and the SVF) and Tap
Boost were removed, Input Gain was added, Level Match was made always on, and
every parameter was fitted onto one page of 8 knobs. The parts that remain
(DaisySP's `Overdrive` / `Decimator` / `DelayLine`) are rewritten in C with the
same formulas down to each operation (`dsp/dirt_engine.c`). There is no
dependency on DaisySP itself.

## Build / test

Run from the root of this repository. Needs Linux/WSL, Python 3, GCC,
aarch64-linux-gnu-gcc, and Node.js for the UI check. Schwung's plugin API
headers (MIT) are bundled in `third_party/schwung/`. The UI check needs a
Schwung checkout (for its page planner): `../schwung` next to this repository,
or `SCHWUNG_ROOT=/path/to/schwung`.

```bash
bash scripts/test.sh
bash scripts/build.sh arm64
```

`test.sh` runs a native build plus the engine tests under ASan/UBSan, an ABI
smoke test that dlopens the real `.so`, and a contract check with Schwung's page
planner. The package is `dist/dirt-delay-module.tar.gz`. The chain host loads
`audio_fx/<id>/<id>.so`, so the shared library is named `dirt-delay.so`. It is
not part of Schwung's own distribution and is provided as a separate package.

## Install

```bash
python3 scripts/install.py http://move.local:7700 --check
python3 scripts/install.py http://move.local:7700
```

This uses the Manager's `POST /modules/install-custom` (tarball upload) and
unpacks to `modules/audio_fx/dirt-delay/`. You can also upload it by hand with
Custom install on the Manager's /modules page.

## Knobs (one page only)

| Knob | Name | Range | What it does | Original |
|---|---|---|---|---|
| 1 | Mix | 0–1 | Dry/Wet (equal power, constant level) | KNOB_0 |
| 2 | Drive | 0.3–1 | OD drive **and** BC downsampling (shared, as in the original) | KNOB_2 |
| 3 | Time | 0–250 ms | Shared by both delays (left uses 3/4 of it) | KNOB_3 |
| 4 | Feedback | 0–0.9 | Same range as the original's `min(knob, 0.9)` | KNOB_4 |
| 5 | Crush | 0–1 | BC bit reduction (0–16 bit) | KNOB_1 |
| 6 | Input Gain | 0 to +24 dB | Input gain of the wet path. Sets how deep the distortion goes | none |
| 7 | FX 1 | Off / OD / BC | Before the first delay | sw[0] |
| 8 | FX 2 | Off / OD / BC | Between the two delays | sw[1] |

Signal flow: input × Input Gain → bass boost (fixed) → OD (Drive, always on) →
FX 1 → Delay 1 → FX 2 → Delay 2 → Level Match → equal-power Mix with the dry.

**How Input Gain acts** (110 Hz sine, -12 dBFS input, Drive 0.3): about 1 %
distortion at 0 dB, about 33 % at +24 dB (including v0.4's bass boost). Level
Match keeps the output level the same (0.0 dB difference); only the distortion
changes. Input Gain applies only to the wet path; the dry passes untouched.

**Stereo (v0.5, no knob, fixed)**: the left pair of delays runs at 3/4 of Time
and the right pair at Time. Left and right echoes return at different times, so
the wet spreads across left and right even from a mono source (most of Move's
synths). Measured with noise, the wet's left/right correlation fell from 1.0
(identical) to 0.18. The first echo arrives at 2×Time on the right and
1.5×Time on the left. Because the two sides differ in time, listening in mono
(such as Move's built-in speaker) can make the echoes split in two or sound a
little dull. The dry is not affected.

**Bass boost (v0.4, no knob, fixed)**: a low shelf lifting +6 dB around 100 Hz
(RBJ biquad, S = 1) sits **before** the wet path's distortion. The low end is
pushed harder into the distortion, giving the fat lows of a fuzz with a bass
boost. Measured with 55 Hz and 880 Hz together: in v0.3 both came out equally,
in v0.4 55 Hz comes out about +6 to +11 dB ahead of 880 Hz (more with deeper
Drive). Level Match evens out the whole wet level, so the overall level does not
change, only the balance of lows against mids and highs. The dry is not affected.

## Level Match (always on)

Moving Mix does not change the output level.

1. **Equal-power crossfade** (dry = cos, wet = sin). The original's linear
   crossfade drops about 3 dB in the middle for two uncorrelated signals.
2. **Automatic wet level matching**. The wet level moves a lot with Input Gain,
   Drive and Feedback, so the wet power is matched to the dry power **2×Time
   earlier** (the stretch that became the first echo).
   - The target comes from a symmetric slow follower (0.4 s). Correction range
     -36 to +24 dB.
   - Sudden increases (raising Drive or Input Gain) are **held to +3 dB at once**
     by a fast follower judged within the same block. The gain falls fast and
     rises slowly (0.3 s).
   - It learns only from blocks where both dry and wet carry signal, so the
     tail after the input stops is not corrected and decays as it is.

Measurements (`tests/test_engine.c`): white noise in 6 settings × 5 Mix points,
and repeated decaying hits (Drive 0.3/0.5/0.9), all within ±0.66 dB. Even with
sudden changes of Drive 0.3→1.0, Input Gain 0→+24 dB and Feedback 0→0.9 the
10 ms window maximum is +3.4 dB. Near Time 0 (wet ≈ dry) dry and wet are
correlated, so the middle of Mix is slightly louder.

## What changed from the original

- **No low-pass**. The original had a 2 kHz OnePole on Delay 1's output and an
  SVF LP (Cutoff/Res) on Delay 2's output; both are gone. The distortion's
  harmonics come straight through. The SVF's anti-blow-up guard went with it.
- **No Tap Boost**.
- **Drive 0.3–1**. DaisySP's `Overdrive` has zero pre-gain at drive 0, and the
  input-stage OD is always on, so in the original the wet was silent at Drive 0.
  0.3 is almost clean; 0.5 is a small-signal gain of about 24× (+27 dB).
- **Time 0 = 1 sample**. `DelayLine` reads 12000 samples back (about 0.27 s) at
  zero delay, so turning the knob all the way down jumped to the longest delay.
- **Knob smoothing**. Move's encoders send stepped values, so Mix/Input
  Gain/Drive/Feedback follow over 10 ms and Time over 60 ms (a fractional delay,
  giving a tape-like pitch change instead of a click).
- A clamp before the Decimator's float→int conversion and an unsigned left shift
  (only to avoid undefined behaviour in C; the sound is the same).
- Values below 1e-15 are rounded to 0 (denormal protection, -300 dB).
- The output hard-clips at ±1.

## Behaviour kept from the original

- **The first echo returns at twice Time** (two delays in series with the same
  time). The wet contains no direct sound.
- **FX 2's OD has no drive of its own**. `overdrive_l/_r` are shared by FX 1 and
  FX 2, and FX 2 uses whatever value was set last. It follows Drive only when
  FX 1 is OD; otherwise it stays at the initial 0.5.

## Realtime safety

A static pool with no malloc even in `create_instance` (16 instances, about
3 MB of BSS in total). No file I/O, logging or locks. Measured natively at about
4 ms per second of audio.

State JSON is version 3. States from v0.1 (version 1) and v0.2 (version 2) can
also be read: the removed keys (cutoff / res / boost / match) are ignored, Input
Gain is restored as 0 dB, and Drive below 0.3 is raised to 0.3.
