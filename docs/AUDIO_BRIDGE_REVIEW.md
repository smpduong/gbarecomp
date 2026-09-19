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
(ordered source pushes and host pulls with sample offsets and counters).

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
