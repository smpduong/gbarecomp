# Audio bridge review, 2026-09-19

The MMBN3 White replay exposed host audio shortages around 128–138 seconds.
The fixes address measured playback defects; they do not establish complete
GBA audio accuracy or guarantee every device and game is free of artifacts.

## Confirmed causes and fixes

- Live audio and concealment switched with an abrupt waveform discontinuity.
  A 48-case stereo, multi-tone regression across 44.1/48/65.536/96 kHz measured
  a worst boundary jump of 21,079 S16 units before the fix, zero afterward.
  A 3ms convex crossfade now smooths entry and recovery.
- Starved playback resumed before the buffer had recovered. It now rebuilds
  the steady cushion; long stalls still stop concealment and fade to silence.
- Overflow could overwrite the samples behind the read cursor that the
  interpolation filter still needed. The ring now reserves this history.
- A 1.5ms late-wake rule accumulated permanent producer slowdown. Short late
  wakes now retain the absolute frame schedule; a whole missed frame still
  resynchronizes. A 6000-frame schedule with a 2ms delay every tenth frame
  accumulated 1200ms under the old rule and zero under the new one.
- Game audio arrives in uneven batches, sometimes approximately 5ms followed
  by 28ms. The SDL bridge target is now 40ms, up from 25ms. This adds 15ms of
  target buffering, not a measured 15ms change in button-to-speaker latency.
  `GBARECOMP_AUDIO_TARGET_MS=25..100` permits controlled comparisons.

## Exact recorded-schedule comparison

The baseline contains 9,321,346 source samples and 9,327,104 callback samples.
Replaying its push/pull order with the original bridge reproduced every output
sample exactly. All comparisons below use those same inputs and callbacks.

| Bridge / target | Concealment episodes, full run | Synthesized audio at 128–138s |
|---|---:|---:|
| Original / 25ms | 76 | 18.417ms |
| Corrected / 25ms | 51 | 25.970ms |
| Corrected / 30ms | 6 | 8.728ms |
| Corrected / 35ms | 2 | 0ms |
| Corrected / 40ms | 1 | 0ms |

Crossfading alone is insufficient: rebuilding the queue can synthesize more
audio at a too-small target even while reducing episode count. At 40ms there
were no shortages after 20 seconds in this recording. The one startup stall
remains in the evidence; it is not silently excluded from the full-run total.

## Reproduction tools

Set `GBARECOMP_AUDIO_CAPTURE=/absolute/path/to/new-prefix` before a windowed
bridge run. Its parent directory must exist. This opt-in recorder preallocates
bounded memory for up to 240 seconds, copies PCM/events under the existing audio
mutex, and writes only after the audio device stops. It records no microphone
or desktop sound. It refuses an existing output prefix and reports write
failures or truncation. Keep captures local; they contain game audio.

Outputs are `-source.wav` (mono bridge input after volume), `-output.wav` (mono
SDL callback output before downstream SDL/device conversion), and `-events.csv`
(ordered source pushes `P`, host pulls `C`, and action markers `M`, with sample
offsets, counters and a trailing `label` column). Markers are written by
`HostWindow::audio_capture_marker` at save/load boundaries
(`presave`/`postsave`, `preload`/`postload`, `premem-save`/`postmem-save`,
`premem-load`/`postmem-load`) and at host-action points (`fast-on`/`fast-off`
on fast-forward level edges, `pause`/`resume` on applied pause transitions,
`rewind-trigger` when the rewind request reaches the dispatcher) under the
same audio mutex as `P`/`C`, so they carry a steady-clock timestamp in the
same timeline and let a harness read the ring fill at an action instead of
guessing it from a push index. They are read-only: nothing is recorded unless
a capture is active, and the bridge and device are only read.

Phase brackets use the same marker record and sink. Emitted per presented
frame and per input pump: `fh-enter`/`fh-exit` (whole frame-present hook),
`render-enter`/`-exit`, `present-enter`/`-exit`, `audiopush-enter`/`-exit`,
`pumpfn-enter`/`-exit` (input pump), `rewindcall-enter`/`-exit` around the
rewind-history capture call, `premem-save`/`postmem-save` (in-memory snapshot
serialization), `rewind-fire`/`rewind-store`/`rewind-fail` (cadence gate,
history push, failure), `hostpump-enter`/`-exit` around `HostWindow::pump`,
`pumppoll-enter`/`-exit` around each `SDL_PollEvent` call and
`pumphandle-enter`/`-exit` around the handling of one dequeued event (both
balanced on the loop's `continue` paths by a scope guard),
`svcev-enter`/`-exit` around `SDL_PumpEvents` in `service_events`, and
`pace-enter`/`-exit` around the frame pacer. Labels fit the 24-byte field;
the pairs nest, and the consumer (`tools/host_stall_attribution.py` in the
game repo) refuses a malformed stream rather than guessing. They exist so a
producer stall can be attributed to one code path; the consuming game's
AUDIO_REVIEW §5h holds the measurements and limits.

Two opt-in probes ride the same sink, both default off and both recording no
markers unless a capture is active (their callbacks/thread still run when on):

* `GBARECOMP_EVENT_WATCH=1` (read once, first use) installs an SDL event
  watch and records one `evwatch:<type hex>` marker when its callback fires
  on the event-posting thread. On this Mac's
  [SDL2-compat implementation](https://github.com/libsdl-org/sdl2-compat/blob/release-2.32.70/src/sdl2_compat.c)
  the callback may precede final queue admission. Its position in the stream
  shows watch activity relative to `svcev`/`pumppoll`, not proof of enqueue.
* `GBARECOMP_EVENT_CANARY=<interval ms>` (read once, first use; honoured only
  together with the watch; 2-1000 ms, `1` means the 8 ms default) starts a
  helper thread that pushes one private user event per interval and brackets
  the `SDL_PushEvent` call with `canary-push-enter`/`canary-push-exit`. The
  revised probe also writes `canary-push-queued` or `canary-push-rejected`
  from [SDL_PushEvent's return value](https://wiki.libsdl.org/SDL2/SDL_PushEvent)
  (1 means queued); historical captures did not. The records are
  written by that thread and can interleave with main-thread
  brackets, so a consumer must treat both probe families as known but never
  as phase intervals; the game's tool does exactly that (current rule v6;
  the historical probe reports used v5). It tests whether a long pump call
  also holds up another thread's event push; the new result marker can verify
  whether SDL accepted that push. It does not explain why the main thread
  returns late. The thread is
  joined in `HostWindow::close()` before any SDL teardown; the watch is also
  removed there before its `HostWindow*` userdata can be destroyed. The pushes
  are directed to the normal queue, where the event loop drains and ignores
  accepted ones (unknown user type).

`GBARECOMP_PHASE_MARKERS=0` (read once, first use) suppresses exactly the
phase set above (including the canary brackets and result labels) while
keeping the capture and the action/save-boundary
markers. This removes phase-marker *records* in the current binary, but the
classification call and branch remain; it is not an exact reconstruction of
the earlier, uninstrumented executable. Marker calls now return before taking
the audio mutex when no capture is active; this prevents the default
phase hooks from imposing a mutex/read cost on ordinary uncaptured gameplay.
The default records everything only when a capture is active.
The game's AUDIO_REVIEW §5i compares both modes and routes, and §5k estimates
the *mean* added wall time of recording the stream at about 8 ms per 4.5 s
route-2 run (nominal 95% interval [-37, +53] ms at ~19 records/frame). That
interval does not limit the duration or probability of rare stalls. §5l adds
a pre-registered paired route-1 batch (ON 1/60 vs OFF 2/60), but its
repeatedly checked interval assumes independent runs and cannot establish a
95%-confidence exclusion of marker effects. §5m uses the
two probes: 17 startup `svcev` calls were seen before b4, and b4 added two
startup calls plus one post-settle instance of 157.955 ms. In those calls
with a canary, the helper thread's pushes completed in 2.2-24.2 µs at
points throughout (worst of 24,264 pushes: 0.31 ms), so the other thread's push calls and watch
callbacks continued during those calls even though the main thread returned
late, in both phases. Those historical captures did not record push return
values, so they do **not** prove queue admission or when the game thread
consumed events; they also do not identify the work inside SDL.

The revised result-marker build has since supplied direct admission evidence
(game AUDIO_REVIEW §5m): an 18-run batch recorded 7,743 queued canary pushes
with none rejected, including 9 inside an 81.673 ms post-settle
`SDL_PollEvent` call in a no-action control. A subsequent one-repeat smoke on
the final binary recorded 884 queued, none rejected, including 10 inside an
87.1845 ms post-settle `SDL_PumpEvents` call in its no-action control.
These captures establish cross-thread queue admission during those specific
stalls, not the reason SDL returned late, the timing of game-thread input
consumption, or an uninstrumented stall-rate estimate.

Build `audio_bridge_replay`, then run:

```sh
build/audio_bridge_replay /path/to/capture /path/to/new-replay --target-ms 40 --require-exact
```

The target must match the captured run for an exact comparison. The tool
defaults to 25ms for historical captures. Remove `--require-exact` when comparing
changed bridge behavior. `RAB_REPLAY_HEADER` can select a historical bridge
header at compile time without altering the checkout. Source/host rates come
from the WAV headers. Other bridge parameters use runtime defaults.

`underrun_events` is a legacy API name counting unfilled output frames, not
episodes. `starvation_events` counts shortages including concealed ones;
`stretch_events` and `stretch_frames` describe synthesized audio. Zero hard
underruns alone is not a continuity pass.

## Verification and limits

The 31-test engine suite passes. The new continuity test also passes AddressSanitizer
and UndefinedBehaviorSanitizer. Existing tests cover multiple host sample rates,
callback sizes, correction direction, bounded concealment, fade to silence, and
recovery. The new uneven-batch test requires zero shortages at the 40ms target.
Live game replay results are recorded in the consuming game's September 19 review.

Remaining audio concerns include device-side conversion and buffering, audible
confirmation on physical speakers, host audio queued across a save/rewind, and
the existing snapshot format's omitted wave/noise channel state. These need
separate compatibility-aware tests; they are not claimed fixed by this patch.

## SDL event-call cost probes (host stall audit)

Five opt-in, default-off diagnostics were added while auditing why a producer
stall coincides with a long SDL event call — three probes and two heal-load
contention arms. None changes any default behaviour and none is required for
normal play.

* `GBARECOMP_SDL_COST=<ms>` (read once, at first use; threshold 1–1000 ms, a
  non-numeric value means 8 ms) times every `SDL_PumpEvents` and
  `SDL_PollEvent` call in wall time and **thread CPU time**
  (`CLOCK_THREAD_CPUTIME_ID`), with whole-process CPU (`getrusage`) as well.
  It writes one `[sdl-cost]` stderr line per call at or above the threshold
  and one per-run summary at `close()`, each with a run-relative `t_ms`. It is
  pure stderr: it never touches the bridge, the capture, the phase markers,
  the event watch or the canary, so it can measure an otherwise uninstrumented
  run. The thread-CPU figure is the point — a long call with almost no CPU
  means the calling thread was parked inside SDL rather than executing SDL
  code. On this Mac it showed exactly that: 66–188 ms calls consuming
  0.5–10 ms of thread CPU, with `sample` stacks placing the park in AppKit's
  `NSAutoFillHeuristicController` / `WritingToolsUILibraryCore` soft-links and
  dyld loaders-lock waits reached from  `Cocoa_PumpEventsUntilDate ->
  -[NSApplication nextEventMatchingMask:]`. Each line also carries `mono_us=`,
  the end of that call's window on the process-wide monotonic clock, so the
  window `[mono_us - wall_us, mono_us]` can be intersected with the engine's
  own load windows from `GBARECOMP_LOAD_TRACE` (below).
* `GBARECOMP_NO_GAMEPAD=1` (read once, at open) skips the game-controller and
  sensor subsystem for the window, as the diagnostic arm that separates SDL's
  HIDAPI gamepad/gyro polling from SDL's core platform pump. The stall did not
  go away with it.
* `GBARECOMP_HEAL_PREWARM_MAP=1` (read once, at first load; default off) is the
  heal-load contention arm, and the recommended one. Before `dlopen`, the heal
  worker maps the freshly compiled shard `PROT_READ|PROT_EXEC`, touches one
  byte per page, unmaps it, and only then loads it. On this Mac the per-path
  first-load cost is charged at the first executable mapping (37–71 ms), not by
  page-in, write-back or the shard's contents, and dyld holds its loaders write
  lock for the whole of it; paying that mapping on a thread that holds no dyld
  lock turns the subsequent `dlopen` into ~0.2–2 ms. Measured on route 1: lock
  duty 64.1 % -> 0.6 %, worst single lock window 265 ms -> 2.0 ms, and the one
  reproducible 78–108 ms pump park per run (12/12) disappears (0/12). It moves
  cost rather than deleting it — the same kernel work now idle-waits on the
  worker — and is not the shipping default.
* `GBARECOMP_HEAL_OUT_OF_PROCESS_LOAD=1` (read once, at first load; default
  off) is the same idea via a helper process: the executable re-invokes itself
  with `GBARECOMP_LOAD_VALIDATE=<path>[;<path>...]`, loads the paths and exits
  before `main()`. It works (114.6 ms in-process load -> 0.198 ms afterwards)
  but costs ~0.24 s of process startup per shard, so the mapping pre-warm above
  is preferred; it is kept as the fallback if executable mapping is ever
  restricted.
* `GBARECOMP_LOAD_TRACE=1` (read once, at first load; default off) wraps this
  engine's only runtime `dlopen` — the heal/overlay shard load in
  `overlay_compile.cpp::load_and_resolve` — and writes one `[load-trace]`
  stderr line per load: `begin_us= end_us= wall_us= tid= path=` on the
  process-wide monotonic clock. `[sdl-cost]` lines carry the matching
  `mono_us=` stamp, so an SDL call window and a load window can be
  **intersected arithmetically** instead of inferring authorship from log
  order. Header-only (`src/runtime/load_trace.h`), no allocation, no sampling,
  and inert (one cached bool test) when unset. It covers only this engine's
  loads: AppKit/CoreAudio soft-links need dyld's own `DYLD_PRINT_APIS` tracing.

The consuming game's `docs/AUDIO_REVIEW_2026-09-19.md` §5n holds the
measurements, the captured stacks, the `sample` confound and the remaining
limits; §5o resolves the open question with the load-window probe and reports
that freshly compiled shards cost 44–766 ms on their first load (ad-hoc
signature validation, ~0.2 ms on a repeat path), so a cold heal cache makes
this engine hold dyld's loaders write lock ~66 % of a run and is what parks
the main thread's event pump. §5p then traces the cost to the first executable
mapping of a path and A/Bs the pre-warm arm (64.1 % -> 0.6 % lock duty, worst
window 265 ms -> 2.0 ms, 12/12 -> 0/12 runs with a >= 20 ms park). All five
toggles are also admitted by the game's
sweep `--env` allowlist.
