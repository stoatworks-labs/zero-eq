# Zero EQ parameter reference

A plugin's public interface is its **automation parameter surface** — the IDs a host, a
control surface or a script uses to read and write its state. This is the complete list.

Parameter IDs are stable and versioned (`juce::ParameterID(..., 1)`), so automation written
against them survives updates.

`getAllParameterIDs()` in `Source/PluginParameters.h` returns the full list programmatically.

---

## Per-band parameters

There are **8 bands**, indexed `0`–`7` in the IDs (the GUI labels them **Band 1**–**Band
8**, so `band0_freq` is "Band 1 Freq"). Substitute the index for `<i>`.

| Parameter ID | Type | Range | Default | Unit |
|---|---|---|---|---|
| `band<i>_type` | choice | see *FilterType* | — | |
| `band<i>_freq` | float | 20 – 20000 (skew 0.25) | per band, see below | Hz |
| `band<i>_gain` | float | -24 – +24 | 0 | dB |
| `band<i>_q` | float | 0.1 – 18 (skew 0.4) | 0.707 | |
| `band<i>_character` | choice | see *FilterCharacter* | Modern | |
| `band<i>_slope` | choice | see *FilterSlope* | Slope12 | |
| `band<i>_active` | bool | | **true** | |
| `band<i>_solo` | bool | | false | |
| `band<i>_harmonic_blend` | float | 0 – 1 | 0.5 | |

### Dynamic EQ, per band

| Parameter ID | Type | Range | Default | Unit |
|---|---|---|---|---|
| `band<i>_dyn_active` | bool | | false | |
| `band<i>_dyn_direction` | choice | see *DynamicDirection* | Downward | |
| `band<i>_dyn_threshold` | float | -60 – 0 | -24 | dB |
| `band<i>_dyn_ratio` | float | 1 – 20 (skew 0.5) | 2 | :1 |
| `band<i>_dyn_attack` | float | 0.1 – 200 (skew 0.3) | 10 | ms |
| `band<i>_dyn_release` | float | 5 – 1000 (skew 0.3) | 100 | ms |
| `band<i>_dyn_range` | float | 0 – 24 | 12 | dB |
| `band<i>_dyn_sidechain` | bool | | false | |

Default band frequencies are **60, 150, 400, 1000, 2500, 5000, 9000, 15000 Hz** for bands
0–7, with per-band default types.

---

## Global parameters

| Parameter ID | Type | Range | Default | Unit |
|---|---|---|---|---|
| `input_gain` | float | -24 – +24 | 0 | dB |
| `output_gain` | float | -24 – +24 | 0 | dB |
| `eq_active` | bool | | **true** | |

## Compressor

| Parameter ID | Type | Range | Default | Unit |
|---|---|---|---|---|
| `comp_active` | bool | | false | |
| `comp_threshold` | float | -60 – 0 | -18 | dB |
| `comp_ratio` | float | 1 – 20 (skew 0.5) | 2 | :1 |
| `comp_attack` | float | 0.1 – 200 (skew 0.3) | 10 | ms |
| `comp_release` | float | 5 – 1000 (skew 0.3) | 100 | ms |
| `comp_knee` | float | 0 – 24 | 6 | dB |
| `comp_makeup` | float | -24 – +24 | 0 | dB |
| `comp_auto_makeup` | bool | | **true** | |
| `comp_detector` | choice | see *DetectorType* | **RMS** (index 1) | |

> **`comp_makeup` is ignored while `comp_auto_makeup` is on**, which it is by default.
> Automating makeup gain with auto-makeup enabled does nothing — turn auto off first.

---

## Enumerations

Choice parameters are indexed from 0, in the order below.

### FilterType
| Index | Value |
|---|---|
| 0 | Bell |
| 1 | LowShelf |
| 2 | HighShelf |
| 3 | HighPass |
| 4 | LowPass |
| 5 | Notch |
| 6 | BandPass |
| 7 | TiltShelf |

### FilterCharacter
| Index | Value | Meaning |
|---|---|---|
| 0 | Modern | Independent Q, textbook RBJ response |
| 1 | Vintage | Proportional Q — bandwidth widens as gain increases, console/passive style |
| 2 | Harmonic | Modern's linear response **plus** gain-driven harmonics from this band's own region |

### FilterSlope
| Index | Value | Cascaded biquads |
|---|---|---|
| 0 | Slope12 | 1 |
| 1 | Slope24 | 2 |
| 2 | Slope36 | 3 |
| 3 | Slope48 | 4 |

### DynamicDirection
| Index | Value | Behaviour |
|---|---|---|
| 0 | Downward | Duck — gain reduces as the band's own signal rises above threshold |
| 1 | Upward | Boost — gain increases as the band's own signal falls below threshold |

### DetectorType
| Index | Value |
|---|---|
| 0 | Peak |
| 1 | RMS |

---

## Behavioural notes for automation

- **`band<i>_gain` does nothing on HighPass, LowPass, Notch and BandPass** — those types have
  no gain term. Gain applies to Bell, the shelves and TiltShelf.
- **`band<i>_slope` only applies to HighPass and LowPass.**
- **`band<i>_harmonic_blend` only has an effect when `band<i>_character` is Harmonic**, where
  0 = pure even harmonics and 1 = pure odd. At 0 dB band gain, a Harmonic band is fully
  transparent regardless of blend.
- **Soloing any band mutes the others.** Solo is resolved across all eight bands before any
  band processes, so it can't be reasoned about one band at a time.
- **`band<i>_dyn_sidechain` falls back cleanly** to internal detection when no external
  sidechain audio is actually connected — it doesn't fail, it just uses the internal signal.
- **Parameter changes glide over 20 ms** (frequency, gain and Q) to avoid clicks, except on
  the first block after playback starts, which snaps to current values.

## Latency

**Zero, always.** The plugin reports zero added latency to the host under every setting —
verified across a full factory-preset sweep. There is no configuration in which it asks the
host for delay compensation.

## Buses

Main stereo in/out, plus an **optional sidechain input bus**. The sidechain may legitimately
be absent or empty; bands with `dyn_sidechain` on fall back to internal detection.
