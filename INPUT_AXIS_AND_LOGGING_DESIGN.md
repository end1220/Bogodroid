# Controller Axis Calibration and Log Filtering - Design

This document records the proposed unityloader-level solution for incomplete
analog-stick range and excessive diagnostic logging. It is an implementation
plan, not a description of features that are already available.

## 1. Observed Loong controller behavior

Samurai II was tested with the SDL controller mapping:

```text
1900fe3c039900001399000002010000,Loong Gamepad,...,leftx:a0,lefty:a1,...
```

The diagnostic build sampled the left stick after SDL had converted the Linux
input event to `Sint16`:

| Value | Observed range |
|---|---:|
| X normalized | -0.70532 to +0.77338 |
| Y normalized | -0.69852 to +0.81658 |
| Vector magnitude | maximum sampled 0.84746 |

The current rate-limited logger can miss the final event at the physical stop,
so these numbers must not yet be treated as exact calibration constants.
Nevertheless, the in-game behavior confirms that the effective stick magnitude
does not reliably reach 1.0. Samurai II uses that magnitude as a movement-speed
multiplier, while synthesized D-pad axes are exactly -1 or +1. This explains a
running animation whose movement remains slower than D-pad movement.

The value named `raw` in the diagnostic log is raw only relative to
unityloader. It is an SDL `SDL_ControllerAxisEvent.value`, not the original
Linux evdev value. A calibration UI may use evdev directly, normalize against
its own learned range, or draw its indicator at the edge before its numeric
value reaches 1.0. Its visual result therefore does not disprove the SDL data.

## 2. Axis calibration belongs in the generic input layer

Calibration should be implemented in `platform/common/input_backend.cpp` and
configured per controller. It must not be:

- hard-coded for the Loong controller;
- tied to Samurai II game code;
- implemented in `samurai2_offline.so`;
- added to `gamecontrollerdb.txt`, whose job is control assignment, not analog
  range correction.

Stick axes are signed values in `[-1, 1]`. Only their absolute value and vector
magnitude are in `[0, 1]`. Triggers remain a separate `[0, 1]` input type and
must not pass through stick calibration.

### 2.1 Proposed TOML schema

Use the SDL joystick GUID so the same port configuration remains correct when
used with another controller. A generic fallback can be supported later, but
GUID-specific calibration should take precedence.

```toml
[input.axis_calibration."1900fe3c039900001399000002010000"]
enabled = true

# Values are SDL-normalized measurements, not Linux evdev integers.
left_x_negative = 0.71
left_x_positive = 0.78
left_y_negative = 0.70
left_y_positive = 0.82

# Optional; omitted right-stick values mean identity mapping.
# right_x_negative = 1.0
# right_x_positive = 1.0
# right_y_negative = 1.0
# right_y_positive = 1.0

clamp_vector = true
```

The measured constants above are illustrative. Final values must come from an
exact peak-capture run, with a small safety margin to avoid normal noise causing
permanent saturation.

### 2.2 Mapping algorithm

For each component, use a separate positive and negative endpoint because the
observed Loong ranges are asymmetric:

```text
scale = value < 0 ? negative_endpoint : positive_endpoint
calibrated = clamp(value / scale, -1, 1)
```

After both components are updated, optionally clamp the vector to the unit
circle:

```text
r = sqrt(x*x + y*y)
if r > 1: (x, y) = (x/r, y/r)
```

This preserves direction, makes cardinal endpoints reachable, and prevents a
diagonal from exceeding magnitude 1 after independent component scaling.
Apply calibration before populating Android `MotionEvent.axisValues`; keep the
advertised Android ranges at `[-1, 1]`. D-pad HAT values, triggers, mouse input,
and optional axis-to-KeyEvent synthesis remain behaviorally separate.

### 2.3 Deadzone and response-curve trade-offs

A simple endpoint scale increases all nonzero values, including gentle walking.
That is predictable and is the appropriate first implementation. Do not also
apply a new inner deadzone by default: the Android `MotionRange` already
advertises `flat=0.12`, and Unity/game code may apply it.

If testing shows that low-speed control becomes too sensitive, add an optional
piecewise mapping later. It should keep an anchor region unchanged and expand
only the outer range. Do not silently introduce nonlinear response in the first
version.

Runtime auto-calibration is not recommended for normal play. A range that grows
as the player moves produces changing sensitivity and saves accidental spikes.
Use a diagnostic peak-capture mode to collect values, then store reviewed values
explicitly in TOML.

### 2.4 Better peak diagnostics

The current 250 ms periodic logger can omit a final peak because SDL stops
sending events once the stick stops moving. Replace it with an accumulator:

1. While a stick is outside the threshold, update per-axis signed extrema and
   maximum vector magnitude without logging.
2. When the stick returns below the threshold, emit one summary line.
3. Reset the accumulator for the next movement.

This is both more accurate and substantially quieter. A timeout summary can be
added for a stick that never returns to center. For comparison with the kernel,
capture `ABS_X`/`ABS_Y` ranges and values with `evtest` on the handheld; the TF
card alone cannot expose `/dev/input/event*` state.

## 3. Log filtering: build-time versus configuration

The two mechanisms solve different problems and should be used together.

### 3.1 Build-time controls define available logging

Existing CMake switches provide the upper bound:

| Build option | Purpose |
|---|---|
| `BD_ENABLE_LOG=OFF` | Compiles out all `BD_LOG` calls. Smallest and quietest production binary, but removes useful startup and input diagnostics too. |
| `BD_ENABLE_LOG=ON` | Keeps normal categorized logs available. |
| `BD_ENABLE_TRACE=ON` | Adds `BD_DEBUG`, warnings, and boot tracing; leave off for normal releases. |
| `BD_ENABLE_VERBOSE=ON` | Adds very verbose output; leave off for normal releases. |

Build-time per-category switches are not recommended. They create many binary
variants and require rebuilding merely to troubleshoot one port. Release/LTO
and log availability are independent: a stripped Release binary can still use
`BD_ENABLE_LOG=ON`, as demonstrated by the axis diagnostic build.

### 3.2 TOML should select categories at runtime

Add an exact-match category denylist read once after `init_config()`:

```toml
[logging]
exclude = [
  "RENDER_FRAME",
  "EGL_SWAP",
  "TEX",
  "AUDIO_EVENT",
  "HKVIEW_TRACE",
  "INPUT_EVENT",
]
```

Recommended semantics:

- absent `[logging]` means current behavior for backward compatibility;
- matching is exact, so excluding `INPUT` must not exclude `INPUT-AXIS`;
- category lookup uses an in-memory set populated once at startup;
- the hot path performs no TOML access, allocation, or file I/O;
- fatal errors and crash-handler output are never filterable;
- plugin `api->log()` must pass through the same host filter, so `HKVIEW_*`
  behaves like core categories;
- malformed entries produce one warning and are ignored.

An optional environment override can be added for failures occurring before
TOML has been parsed, but TOML is the primary per-port interface.

### 3.3 Split telemetry from status and errors first

Filtering the current broad categories directly is unsafe. For example,
`EGL_SDL` contains both periodic `eglSwapBuffers` timing and context/window
creation failures; `INPUT` contains both controller button events and SDL
initialization failures. Suppressing either whole category removes valuable
failure evidence.

Before enabling the default denylist, rename high-volume success/event entries:

| Current category | Proposed high-volume category | Keep under original category |
|---|---|---|
| `RENDER` | `RENDER_FRAME` | renderer startup/exit/failure |
| `EGL_SDL` | `EGL_SWAP` | display, context and surface setup/errors |
| `TEX` | `TEX_TRACE` | unsupported/failure messages |
| `AUDIO` | `AUDIO_EVENT` where applicable | backend/device setup/errors |
| `HKVIEW` | `HKVIEW_TRACE` for repeated camera state | plugin load/hook/errors |
| `INPUT` | `INPUT_EVENT` for buttons | mapping/device setup/errors |

`INPUT-AXIS` remains a dedicated diagnostic category and can be excluded after
calibration. The current Samurai II log contained 131 `EGL_SDL` and 118
`RENDER` entries; separating swap/frame telemetry removes most noise without
losing initialization evidence.

### 3.4 Why launcher-side grep is only a temporary workaround

A launch script can pipe stderr through `grep -Ev`, but that approach obscures
the process exit status unless Bash `PIPESTATUS` is handled, depends on tool
availability, and duplicates policy in every port script. It also cannot express
severity inside a category. Filtering centrally in unityloader covers core and
plugin logs consistently and keeps launchers simple.

## 4. Recommended rollout

1. Replace periodic axis logging with peak-on-return summaries and measure exact
   Loong endpoints in all four cardinal directions and diagonals.
2. Add generic GUID-specific axis calibration with identity defaults, endpoint
   validation, and unit-circle clamping.
3. Test Loong and Anbernic devices with calibration absent, disabled, and
   enabled. Absence must be bit-for-bit equivalent to current axis output.
4. Rename high-volume event categories while keeping setup/error categories.
5. Add the shared TOML denylist and route plugin logs through it.
6. Build normal stripped Release binaries with `BD_ENABLE_LOG=ON`, trace and
   verbose disabled, and a per-port runtime denylist.
7. For a final minimal distribution where field diagnostics are unwanted,
   rebuild with `BD_ENABLE_LOG=OFF` rather than relying on TOML.

## 5. Acceptance criteria

- A calibrated Loong stick reaches magnitude 1 in each cardinal direction
  without exceeding 1 on diagonals.
- Gentle stick movement still permits Samurai II walking and transitions
  monotonically to running.
- Anbernic behavior is unchanged when no matching GUID table exists.
- D-pad, right stick, and triggers are unchanged unless explicitly configured.
- Invalid or zero endpoints fall back to identity mapping and log one warning.
- Default filtered logs retain controller discovery, EGL initialization, audio
  device setup, plugin hook status, fatal errors, and crash output.
- Frame/swap/button/camera telemetry can be disabled from TOML without a rebuild.
