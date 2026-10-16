# Motion and audio timing investigation

**Player success first:** native 1x, original 4:3 and FrameSkip=0 must provide
continuous car/world motion and audio through heavy carnage, including other
cars on screen, at the original mode's intended cadence. Prompt wheel movement
does not establish faithful steering/traction response. Do not change physics or
hide stalls by skipping frames/audio. Required proof includes a matched race,
race-clock versus wall-clock progression, actual update cadence and a subsequent
hands-on/reference comparison; host vblank rate alone is insufficient.

**Situation:** the player reports periodic car/world and audio jumps, worsened
when other cars are visible. Read-only tracing found the EE can block for GS
backpressure while vblanks, hardware timers and audio production derive from
wall time. The existing kernel delivers missed vblanks back-to-back, reanchoring
after more than roughly 12 periods of debt. Existing SPU2 production discards
debt beyond 100 ms and withholds mixed buffers when the audio queue already holds
at least 200 ms. These facts suggest a shared stall/catch-up hypothesis; they do
not establish the reported symptom's root cause.

**Task/action:** `EOD_TIMING_TRACE=1` adds default-off observations of these
existing boundaries, following the bounded aggregate approach already used by
`EOD_INPUT_TIMING`. No deadline, queue bound, drop policy, rendering or physics
rule changes. It prints at most 120 lines, at most once per second:

- `wall_us`, `vblank`: monotonic host timestamp and delivered vblank count.
- `service_gap_max_us`: largest gap between event-service calls this interval.
- `vblank_debt_max_us`, `catchups`, `resyncs`: lateness at a delivered vblank,
  deliveries with another period already due, and existing deadline reanchors.
  A reanchor can also count as a catch-up delivery.
- `gs_block_max_us`: maximum duration inside the existing `gs_vblank` call,
  including its queue wait and other bookkeeping, not a pure hardware GPU time.
- `audio_gap_max_us`, `audio_debt_max_samples`: service-call gap and largest
  pending production debt, measured in stereo sample frames at 48 kHz.
- `audio_mixed_samples`, `audio_debt_dropped_samples`,
  `audio_queue_suppressed_samples`, `audio_no_device_samples`: interval totals
  distinguishing actual mixing, discarded production debt, buffers withheld by
  the existing queue policy, and mixed frames with no output device.
- `frameskip`, `skipall`: effective configured/environment frame-skip policy and
  presence of the diagnostic all-skip override. These are policy indicators,
  not observed counts of skipped draws or displayed pictures.

Counts reset after each report; prior service timestamps persist across reports.
No per-draw logging or retail data is saved. Timing instrumentation adds host work
when enabled, so compare instrumented and ordinary runs before attributing a
small performance difference. An enabled trace stops printing after its cap.

**Result/proof limits:** the offline audio-volume test exercises actual
`spu2_update` with an unbound SDL stream: 250 ms demand produces 12,000 due
sample frames, mixes 4,800 and accounts for 7,200 discarded; an already-full
offline stream accounts for a subsequently withheld 96-frame buffer. A synthetic
clock also checks the one-second report gate and 120-line cap. These tests need
to pass in the current build before claiming verified accounting.

All automated validation remains `EOD_HEADLESS=1 EOD_MUTE=1` with `--headless` for
game runs. Such runs have no physical audio output device, so zero queue
suppression cannot prove no audible underruns. The trace has no authoritative
physics-update/race-clock hook and does not observe actual display scanout.
Preserve those as explicit acceptance gaps. Next test: one exclusive bounded
native heavy race with both timing flags and correlate GS blocking, event debt,
catch-up and mixer drops. Actual audible/visible continuity and original cadence
still need the subsequent human/reference comparison. Continue the paraLLEl-GS
real-race experiment; title/boot success alone does not qualify a replacement.
