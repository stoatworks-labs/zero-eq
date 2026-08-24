# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*Zero EQ — zero-added-latency parametric EQ + dynamic EQ (with external sidechain) + band-isolated harmonic generation + compressor + presets + ballistics-accurate metering VST3/AU plugin (JUCE/C++), ~/Projects/zero-eq, GitHub public repo, tagged releases v0.2.0-v0.7.0. Note the v0.7.0 lesson: single-tone tests cannot verify frequency-selective DSP*

Zero EQ lives at `~/Projects/zero-eq`, GitHub `allansargeant/zero-eq` (public,
pushed 2026-07-15, branch `main`). JUCE/C++ VST3+AU+Standalone plugin: an
8-band zero-latency parametric EQ (Bell/Low Shelf/High Shelf/High Pass/Low
Pass/Notch/Band Pass/Tilt Shelf, HP/LP with selectable 12–48 dB/oct slope,
Modern/Vintage proportional-Q character switch) plus a post-EQ feed-forward
soft-knee compressor and I/O trim. Explicit design goal: **zero added
latency** — everything is minimum-phase IIR, no lookahead, no linear-phase
mode. Full Pro-Q-style GUI: draggable curve nodes over a live pre/post
spectrum analyzer, built with JUCE's CMake integration (JUCE fetched via
`FetchContent`, not vendored).

**Phase 1 status: built and verified working.** VST3/AU/Standalone all
compile clean with zero warnings in project code. DSP verified three ways
via a throwaway numeric test harness (deleted after use): isolated-filter
magnitude check against RBJ/Butterworth cookbook math, full 8-band
composite check, and a live-APVTS-defaults check — all confirmed
mathematically flat/correct. GUI verified by launching the Standalone
build and screenshotting; caught and fixed a real layout bug (knobs
squeezed to slivers because BandControlPanel/CompressorPanel derived knob
size from leftover space instead of using a fixed size) along the way.

**Debugging gotcha worth remembering**: what first looked like a DSP ripple
bug in the rendered curve was actually a **stale persisted Standalone
settings file** (`~/Library/Application Support/Zero EQ.settings` — JUCE's
StandaloneFilterApp autosaves/restores plugin state across launches) left
over from an earlier test run, not a code defect. If a JUCE Standalone
app's behavior doesn't match freshly-read source defaults, delete that
`.settings` file and relaunch before assuming the code is wrong.

**Testing round 2 (2026-07-15, same day): found and fixed a critical
silence bug.** Ran three tests: `pluginval` (strictness 5, VST3+AU, both
pass clean — AU has one benign known warning about `getCurrentProgram()`
returning -1, harmless, no factory presets implemented), loaded in REAPER
(real DAW, AU format, GUI rendered and hosted correctly, confirmed
"2 in+out" bus), and — critically — processed real audio (8s pink noise)
through the actual `ZeroEQAudioProcessor` class via a throwaway
`AudioFileTest` harness. That last test found the output was **total
digital silence** (confirmed independently via `sox stat`), despite
pluginval passing and the GUI curve looking correct. Root cause:
`EQBand::update()` was reassigning `stages[i].state` (a
`juce::dsp::ProcessorDuplicator` pointer) on every parameter change, but
`ProcessorDuplicator`'s per-channel `Filter` instances each capture their
OWN separate reference to the coefficients at `prepare()` time — replacing
the Duplicator's own `.state` pointer afterward never reaches the
per-channel filters, so they silently kept using the original
default-constructed (silent) coefficients forever. Confirmed via JUCE's own
source (`juce_ProcessorDuplicator.h`) and its official `IIRFilterDemo.h`,
which uses `*iir.state = ...` (mutate in place) for live updates, not
`iir.state = ...` (replace pointer). Fixed in `EQBand.cpp` by
dereference-assigning into the existing Coefficients object instead of
replacing the pointer. Pushed as commit `43c46be`.

**Why this matters for future debugging**: pluginval passing and a
mathematically-correct GUI curve did NOT catch this, because neither
exercises the real-time `ProcessorDuplicator`+smoothing audio path with
non-trivial coefficient updates — the GUI curve uses a separate
stateless/pure static function, and pluginval doesn't assert "output isn't
silence" for a generic effect. **The only test that caught it was piping
real audio through the actual shipped processor class and checking the
output isn't silence.** Treat that as a required check any time
`ProcessorDuplicator`-based coefficient-update code changes in this or
future JUCE projects, not just a nice-to-have.

**Not yet done**: no test against actual (non-silent) live audio input in
a real device chain, no preset system, meters are simple peak reads (not
ballistics-accurate), REAPER GUI click-interaction test was only partially
verified due to a screen-coordinate mapping issue in the automation
(same class of AppleScript-coordinate gotcha as [srt router](https://github.com/stoatworks-labs/srt-router/blob/main/docs/NOTES.md) (`srt-router`)'s
browser click issue) — static GUI hosting was confirmed, but a live
drag-to-edit-in-REAPER interaction was not re-verified after the fix.

**Roadmap set 2026-07-15 (commit `2948acd`)**: the two features the user
called out as the major drivers of where this project goes next are (1)
**dynamic EQ bands** — per-band threshold/ratio/attack/release so a band
acts as a frequency-selective compressor/expander instead of a static
curve — and (2) **harmonic-based EQ** — a musical/character-driven mode
that shapes harmonic content, extending past the current Modern/Vintage
proportional-Q system. Both need to preserve the zero-added-latency
guarantee. Also per explicit user request, all "Pro-Q" references were
removed from the repo (README + two source comments) and reworded to
describe the underlying technique instead of naming a competitor product —
apply that same framing (describe behavior, don't name FabFilter/Pro-Q) to
any future docs/code/commits on this project.

**Dynamic EQ bands shipped same day (commit `581e412`).** Any gain-having
band (Bell/Shelf/Tilt — not HP/LP/Notch/BandPass, which have no gain to
modulate) can now run in dynamic mode: threshold/ratio/attack/release/range
plus a Downward (duck) or Upward (boost) direction. New
`Source/DSP/DynamicEQDetector.h/.cpp` — per-band analysis filter
(band-pass at freq/Q for Bell/Tilt, high-pass at the corner for High Shelf,
low-pass at the corner for Low Shelf — a practical approximation of "the
region this band affects," not a claim of matching any commercial product's
exact algorithm) feeding an envelope follower and the same branched
attack/release gain-delta math as `Compressor`. `EQEngine::updateAndProcess`
had to be restructured from two passes (update-all-bands, then
process-all-bands) into one interleaved per-band pass, because each band's
detector must see the signal as it stands at that exact point in the series
chain (post every earlier band, pre this one) — still zero lookahead, just
correct ordering. GUI: new dynamic control row in `BandControlPanel`
(only enabled when the selected band's type has gain), live gain-delta
readout, and a curve indicator (orange ring + live modulation tick) on
engaged bands in `EQCurveComponent`. Window grew to 1150×900 to fit the
extra row.

Verified with a throwaway real-audio harness (same pattern as the earlier
silence-bug catch — build it, verify, delete it, never leave test
scaffolding in the shipped `CMakeLists.txt`): a loud/quiet/loud 1kHz tone
through band 3 (defaults to Bell @1000Hz) in both directions. Downward
ducked the loud segments by -8.26dB, matching the analytically-predicted
-8.23dB (threshold -20dB, ratio 4:1, ~-9dB RMS input) almost exactly, while
leaving the quiet segment untouched. Upward boosted the quiet segment,
correctly clamped at the configured 12dB range ceiling, leaving loud
segments untouched. Re-ran `pluginval` (VST3, strictness 5) after — still
clean. This confirms the "always verify DSP changes against real audio
through the actual shipped processor class" lesson from the earlier silence
bug is being followed, not just noted.

**GUI automation gotcha from this session**: mid-session, an AppleScript
click aimed at the Standalone app's window instead landed on a background
Chrome tab, which triggered a macOS system permission dialog ("claude.app
wants access to control Google Chrome.app"). That's an unrelated OS-level
automation artifact, not a plugin bug — don't mistake a slow/blocked
`osascript` call for an app hang without first checking for a blocking
system dialog. Declined the permission (not needed for this project's
testing) rather than granting it or leaving it hanging.

**Dynamic EQ visual feedback shipped same day (commit `240412f`)**: three
layered visual cues on the main curve for dynamic bands, GUI-only change
(`Source/GUI/EQCurveComponent.cpp`, no DSP changes) — a shaded "range
envelope" showing the full swing a dynamic band could reach (pure static
math, no live audio needed), a "live curve" drawn on top of the static one
showing actual instantaneous total gain that visibly moves with the signal
(reuses `EQBand::computeMagnitudeForFrequency`/`EQEngine::getCompositeMagnitude`,
the same math already proven correct for the static curve), and node-ring
glow intensity now scaling with how hard each dynamic band is currently
working. Design was explicitly inspired by two commercial references the
user named in conversation — do NOT write those product names into the
repo itself (README/code/commits); describe the behavior only, same rule
as the existing Pro-Q exclusion.

**Repeated GUI-automation hazard, escalated this session**: TWICE now an
AppleScript click aimed at the Zero EQ Standalone window has instead landed
on a completely different open window — once a background Chrome tab
(triggering a permission dialog), once **this user's own separate Claude
Code desktop-app session** ("Presentation Commander"), where a stray
keystroke sequence (`-18.00` + Return) was sent after a misplaced
double-click. No confirmed harm (that session's chat input showed empty
afterward) but it could not be fully verified from this session. Given two
strikes, **stopped all further coordinate-based `osascript`/`System Events`
clicking for this project's GUI verification** rather than risk a third.
Root cause suspected: focus timing races in this environment between
screenshot capture and the next `click at` call, worse when multiple app
windows overlap on screen. For future GUI-visual-feedback work here, rely
on: (a) code-level reasoning about correctness (does it reuse already-
audio-verified data/math, does it compile clean), (b) static/non-interactive
screenshots only (launch + screenshot with NO subsequent clicks), and
(c) ask the user to interactively confirm anything that needs a live click
or real audio playing, rather than attempting further automated clicks.

**Release tagging convention started this session**: after dynamic EQ
shipped, "ship it" was interpreted as creating an annotated git tag +
`gh release create` with notes (verified/known-limitations summary) —
`v0.2.0` for dynamic EQ, `v0.3.0` for harmonic EQ. Continue this pattern
(bump minor version, tag, `gh release create` with a verified/limitations
summary) for future shipped milestones on this repo unless told otherwise.

**Harmonic EQ shipped same day (commit `9dabc59`, tag `v0.3.0`).** Third
per-band character alongside Modern/Vintage: `FilterCharacter::Harmonic`
uses Modern-style linear filtering plus gain-driven even/odd harmonic
saturation via new `Source/DSP/HarmonicShaper.h/.cpp`. Two waveshapers -
asymmetric quadratic (`f(x)=x+k*x^2`, even harmonics, needs a DC blocker
since it's asymmetric) and tanh (odd harmonics) - crossfaded by a new
per-band `harmonic_blend` parameter (0=even,1=odd). Drive scales with
`|gainDb|/12` clamped to [0,1], so a band at 0dB stays transparent even in
Harmonic mode. Uses first-order ADAA (antiderivative anti-aliasing —
`(F(x)-F(xPrev))/(x-xPrev)`, trapezoidal rule on each shaper's closed-form
antiderivative) instead of oversampling to control nonlinearity-driven
aliasing without adding latency or lookahead. `EQBand::process()` runs the
shaper per-channel immediately after that band's own linear filter stages.

Verified with the same real-audio-harness discipline as the two earlier
features: a 200Hz tone at max drive through both blend extremes, FFT'd.
Results were mathematically exact for the chosen functions - pure even
produced ONLY the fundamental + 2nd harmonic (3rd/4th/5th at the noise
floor, correct since sin²(x) has no higher terms), pure odd (tanh, an odd
function) produced ONLY odd harmonics (2nd/4th at the noise floor, correct
since odd functions of a sine can't produce even harmonics). Zero latency
confirmed. Re-ran `pluginval` after — clean. GUI: blend slider added to
`BandControlPanel`, enabled only when the selected band's character is
Harmonic and its type has gain; window grew again to 1150×950 to fit it.

**Autonomous continuation session (2026-07-17): user explicitly asked to be
left running "for as long as possible" unattended.** Worked through the
remaining `README.md` "Known limitations" backlog without further check-ins:
(1) real GUI screenshot for the README, replacing a placeholder the user had
already wired up (`docs/screenshots/plugin.png`) — used the newer, safer
capture method (see **screenshot capture** (working-practice note, kept in Claude memory) update) of bare
full-screen capture + Python PIL crop, no `set frontmost`/click at all, so
none of this session's earlier focus-race risk applied; (2) full **preset
system**, commit `aec7cb4`, tag `v0.4.0`: new `Source/PresetManager.h/.cpp`
— factory presets are sparse parameter-override lists applied on top of a
full reset-to-defaults (`getAllParameterIDs()` added to
`PluginParameters.h` to enumerate every param for that reset), 7 presets
covering every feature area (Init/Vocal Presence/De-Esser (Dynamic)/Warm
Bus (Harmonic)/Podcast Voice/Broadcast Loudness/Telephone/Lo-Fi); user
presets save/load as APVTS-state XML to
`~/Library/Audio/Presets/Allan Sargeant/Zero EQ/` (standard macOS per-user
preset convention); factory presets now also back the real VST3/AU
`setCurrentProgram`/`getProgramName`/`getNumPrograms` (previously stubbed
to one unnamed program) — this incidentally resolved the benign AU
"Current program is -1" pluginval warning noted in in an earlier session,
both formats now pass with zero warnings. New GUI: `PresetBar` (combo with
Factory/User sections synced to `setCurrentProgram`, prev/next, Save via
`AlertWindow` name-entry) — loading a preset is just `setValueNotifyingHost`
per parameter through the same APVTS every control is already attached to,
so no manual GUI refresh code was needed anywhere. Verified with the same
real-audio-harness discipline as every prior feature: all 7 presets
apply via `setCurrentProgram` with every override landing exactly as
specified, audio stays finite, latency stays 0; a save/load round trip to
disk restores exact values. `pluginval` re-run clean on both formats
after. GitHub repo description field kept in sync again (same pattern as
before — update it whenever a shipped feature changes what the plugin
actually does, not just the README).

**Ballistics-accurate metering shipped same autonomous session (commit
`ed91382`, tag `v0.5.0`).** New `Source/DSP/LevelMeter.h/.cpp` replaces the
old raw-peak-only atomic reads on input/output with four independent
measures: Peak (instant attack, ~20dB/s exponential release), VU (one-pole
RMS integrator, ~300ms to settle on a step), True Peak (4x-oversampled
Catmull-Rom cubic interpolation between samples — explicitly documented as
a practical estimate, NOT a full ITU-R BS.1770-compliant filter), and a
held clip indicator (~1.5s hold). Read-only side path on a copy of the
buffer, so it can't add latency. `IOPanel` redrawn: two-layer meter (slow
VU fill + fast peak tick + red clip cap) plus a numeric peak dB readout.

Two more real bugs caught by the same real-audio-harness discipline used
every prior feature (again, neither `pluginval` nor GUI-only checks caught
either one):
- **Clip threshold `>= 1.0f` never triggered**, even feeding a sample at
  exactly 1.0f, because after the "0dB" input-gain stage
  (`decibelsToGain` round-trip through a normalized parameter) the value
  came out ~6e-8 short of 1.0f. Fixed with a -0.1dBFS (0.988553 linear)
  headroom threshold — also just how real clip indicators are normally
  built, not a hack.
- **Clip hold was measured in "blocks remaining"** (decremented once per
  `process()` call) but the hold duration was computed assuming the block
  size that triggered the clip; different block sizes on later calls threw
  off the real elapsed hold time (observed: indicator still lit 2s after
  silence when it should've cleared at 1.5s). Fixed by tracking samples
  remaining and decrementing by `numSamples` each call, so hold duration is
  correct regardless of how the host chunks its callbacks.

`pluginval` strictness 5 clean on both VST3 and AU after. This is now the
**fourth** distinct real bug this project's "build a throwaway test
harness, feed it real audio through the actual shipped processor class"
methodology has caught that pluginval/GUI-only inspection missed (after the
ProcessorDuplicator silence bug and two dynamic-EQ/harmonic-EQ math
checks) — treat that harness step as mandatory for any future DSP-affecting
change on this project, not optional polish.

**External sidechain input shipped same autonomous session (commit
`f4047e5`, tag `v0.6.0`) — the last remaining actionable README backlog
item, so all four original + this roadmap items are now shipped.** Added
an optional stereo "Sidechain" input bus to `BusesProperties` (disabled by
default, `isBusesLayoutSupported` allows it disabled/mono/stereo
independent of the main bus) plus a per-band `bandDynSidechain` bool
parameter. Key structural change: `PluginProcessor::processBlock` used to
operate directly on the raw callback `buffer`, but with a second input bus
that buffer can now carry extra read-only channels beyond the main in/out
pair — so every stage (gain, metering, EQ, compressor) was switched to
operate on a `getBusBuffer(buffer, true, 0)` main-bus view instead, or
sidechain audio would have silently leaked into the actual signal path
(gain staging, meters, everything). `EQEngine::updateAndProcess` gained a
`sidechainBuffer` parameter; per band it picks the sidechain view instead
of the main buffer when that band's toggle is on AND the bus actually has
channels (`isSidechainConnected()` on the processor), otherwise it falls
back to internal detection automatically rather than detecting against
silence. `DynamicEQDetector` itself needed zero changes — its `process()`
already took its analysis buffer as an explicit parameter, so this was
purely a routing change at the call site, a nice payoff from that
existing design. GUI: `BandControlPanel` gained a `Sidechain` toggle next
to the direction combo, whose label live-switches to "Sidechain (n/c)"
whenever the host hasn't connected the bus, so a misconfigured band
doesn't look silently broken.

Verified with the same real-audio-harness discipline as every prior
feature, but this time with three scenarios instead of one (needed to
actually prove ROUTING, not just DSP math): main = constant 1kHz tone
that never varies, sidechain = 1kHz tone alternating loud/quiet every
0.2s. (1) toggle on, bus connected: gain delta fluctuated strongly
(variance ~25, min -11dB) tracking the sidechain pulses despite the
constant main signal — proves detection genuinely came from sidechain,
not a mislabeled main-buffer read. (2) toggle off: gain delta was steady
(variance ~0.04) at the same duck depth, driven by the constant main
signal — the correct default-path baseline. (3) toggle on but sidechain
bus not connected (no `enableAllBuses()` call before `prepareToPlay`):
matched scenario 2 almost exactly, confirming the fallback engages
cleanly rather than crashing or detecting silence. `pluginval` strictness
5: VST3 fully clean (its own bus-enumeration checks — mono/stereo
sidechain layouts, enable-all, disable-non-main, restore-default — all
passed, corroborating the bus wiring independently of the custom
harness). AU produced ONE new warning: "Disabling non-main buses failed."
Investigated via `WebSearch` before accepting it — confirmed via JUCE
forum threads as a known, widely-reported AUv2/JUCE interaction (AU's
bus-disable semantics for aux/sidechain buses don't round-trip the way
pluginval expects) affecting AU plugins with a sidechain bus generally,
not a project-specific bug; `auval` itself (Apple's own validator) still
exits 0 and every other AU check passes. Documented honestly in the
README's Status section rather than hidden or worked around, consistent
with this project's existing pattern of documenting real caveats (true-
peak "estimate not compliant filter" language, the -0.1dBFS clip
threshold rationale, etc.) instead of overclaiming cleanliness.

Test-harness note for future sessions: the harness needed
`processor.enableAllBuses()` before `prepareToPlay()` to get the sidechain
bus's channels active (it's `isActiveByDefault=false` in
`BusesProperties`), and used `juce::AudioBuffer`'s non-owning
pointer-array constructors (`AudioBuffer(Type* const*, numChannels,
numSamples)` and the sample-offset overload) to build zero-copy views into
one big multi-channel buffer for main vs. sidechain vs. per-block slices —
worth reusing that pattern for any future host-buffer-shape testing on
this project. Also: `juce_generate_juce_header()` only works on targets
created via a `juce_add_*` function; a bare `add_executable` test-harness
target must instead point its include path at the already-generated
`${CMAKE_BINARY_DIR}/ZeroEQ_artefacts/JuceLibraryCode` from the real
plugin target (with an explicit `add_dependencies` to guarantee build
order) rather than trying to generate its own.

**Harmonic character was fundamentally wrong until v0.7.0 (commit
`21ab6c6`) — caught by the USER, not by any test.** The v0.3.0 "Harmonic
EQ" ran its waveshaper across `block.getChannelPointer(ch)`, i.e. the
whole broadband signal passing through the band. There was no
band-splitting anywhere, so a Harmonic bell at 5kHz saturated bass, mids
and everything else — a full-range saturator behind an EQ filter, NOT a
harmonic EQ (whose defining property is generating harmonics for a
targeted region and leaving the rest alone). Fixed by isolating the
band's region (shared `EQBand::designRegionIsolationFilter`), shaping only
that, and summing back `shaped - region` (the generated harmonics ONLY)
in parallel with the dry signal — summing the difference rather than the
shaped signal keeps the band's level unchanged and makes drive=0 exactly
transparent. Region-isolation design was previously duplicated inside
`DynamicEQDetector`; it's now one shared static on `EQBand` that the
detector calls, so they can't drift.

**The critical testing lesson — this is the important part.** The v0.3.0
verification FFT'd a **single tone at the band's own frequency** and
declared the harmonic content "mathematically exact." That test is
*structurally incapable* of detecting this bug: with only one tone
present, "saturate only this band's region" and "saturate everything"
produce byte-identical output. It validated the waveshaper maths (which
was always correct) and nothing at all about the routing. The replacement
test is **two-tone** — one tone inside the band, one far outside it —
which separates them instantly (measured 91dB selectivity; out-of-band
fundamental passes within 0.01dB). Generalise this: **a test whose input
cannot distinguish the correct behaviour from the incorrect one proves
nothing, no matter how precise its numbers look.** When verifying anything
frequency-selective on this project, the stimulus must contain content
both inside AND outside the region under test. The same trap would apply
to the dynamic EQ detector and to any future multiband work.

Drive characterisation captured while fixing this (blend=0, even, Bell
Q=1, 2nd-harmonic level relative to fundamental): +3dB gain → -28dB,
+6dB → -15dB, +9dB → -6dB, +12dB → **+1dB (2nd harmonic LOUDER than the
fundamental)**. The low end of that range is musical; the top is extreme
and arguably too hot for a "harmonic EQ" default. Left as-is for now
because it's a taste/tuning judgement rather than a correctness bug —
flagged to the user as an open question, not silently changed.

**README accuracy correction**: the AU `pluginval` run has TWO consistent
benign warnings, not one — "Disabling non-main buses failed" (the known
JUCE/AUv2 sidechain-bus quirk) AND "Current program is -1" (the AU wrapper
reports no factory preset explicitly selected until a host picks one).
An earlier note in this file claimed the preset system had *resolved* the
program warning; that was wrong — it reappears on every run and was
verified consistent across three consecutive runs. Both are unrelated to
the DSP and `auval` itself still exits 0. README now says "two known
benign warnings" and lists both.

**Web simulator shipped 2026-08-03 (commit `6b411f7`), LIVE at
https://zero-eq-sim.allan-sargeant.workers.dev** — the plugin's `Source/DSP`
files compiled UNMODIFIED to WebAssembly (Emscripten via Homebrew) and run in
an AudioWorklet, in `web/` in this repo. Key pieces: `web/wasm/shim/JuceHeader.h`
is a shim standing in for the real JUCE header, with the filter math transcribed
from the exact JUCE sources in `build/_deps/juce-src` (tan-form LP/HP not RBJ
alpha form; `jmax(freq, 2.0)` clamp in shelf/peak; DF2T with per-sample `check()`;
even-order Butterworth Q = 1/(2cos((2i+1)π/2N)); ProcessorDuplicator's
shared-coefficients aliasing reproduced via shared_ptr — the silence-bug
semantics preserved). The shim's dummy parameter classes RECORD what the real
`createParameterLayout()` registers, so the web app harvests every param
id/default/range/skew from plugin source — no hand-copied defaults.
`web/test/harness.mjs` re-runs the house verification against the wasm build
(all 30 checks pass: two-tone harmonic selectivity 79.9 dB, dyn duck −8.26 vs
−8.25 analytic, comp GR −11.27 vs −11.25, sidechain routing proof, slope checks,
type/character sweep) — run it after ANY rebuild. Two wasm instances run in the
page: worklet (audio) + main thread (curve math), params mirrored to both. A
web-only safety clip (−0.1 dBFS, default on) is documented as not-plugin.
Deploy: `cd web && cf-run npx wrangler deploy` (static-assets Worker
`zero-eq-sim`, manual deploys only, no repo connection). Curve-magnitude C
exports floor at −300 dB (not −100) because 36/48 dB/oct slopes legitimately
exceed −100 and the floor flattened the drawn/queried curve.

**Hard-won web lesson from that session: never drive audio-parameter delivery
from requestAnimationFrame** — rAF throttles to ZERO in hidden/background tabs,
which silently stopped param flush to the worklet (audio kept running on stale
settings; looked exactly like a wasm bug). Flush on queueMicrotask from the
setter instead; keep rAF for drawing only.

**Why:** Tracking this so future sessions don't need to re-derive the
project's scope, architecture, or verification status from scratch.

**How to apply:** Before recommending or building on specifics, re-read
`README.md` in the repo since it may have evolved past this snapshot.
**commit means push** (working-practice note, kept in Claude memory) applies here too. This repo also now carries
the standard AI-assistance disclaimer — see **disclaimer scope** (working-practice note, kept in Claude memory).
This machine had no git author identity configured; it was set with
`--local` scope only (not `--global`) to `allan sargeant
<allan.sargeant@gmail.com>` for this repo.
