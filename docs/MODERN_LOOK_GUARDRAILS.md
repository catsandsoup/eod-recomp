# Modern look guardrails (R6)

Every failure of the old `ModernRenderer=1` pass (`docs/MODERN_RENDERER_RCA.md`) becomes a
rule. Each R6 phase checks every rule against captures before it commits. A rule that
fails blocks the commit.

## What bad looks like (from the old pass)

| # | Old failure | Cause | R6 rule and check |
|---|---|---|---|
| 1 | Torn triangle shards, sliced pit walls | GS-drawn and modern-drawn terrain mixed with unrelated depth | The Modern look draws only native Plume batches. It never mixes the GS 3D path in and never classifies draws by guessing from GS traffic. Check: `EOD_NATIVE_ONLY=1` frames show no holes and no shards. |
| 2 | Sky or ground misclassified (free roam lost its ground) | A per-program rule tested on one track | Classify from the decompiled C++ (world, sky, water and so on), never by program or ITOP mode. Check: Career free roam plus 2 race tracks. |
| 3 | Blocky, washed-out or missing shadows | Cascades and AO radius sized by the median object distance | Shadow and AO sizes are camera-relative constants, not scene statistics. Check: Dundas with max draw distance and Woodly give the same near-shadow texel size. |
| 4 | Night tracks lit like day | The same light direction and ambient are used at night | Daylight comes from the game's fog and sky state. Check: a night track with flood lights stays dark between the lights, with average luminance within 15% of Classic. |
| 5 | Washed-out ground | Baked vertex light × 1.35, then ACES lifting the lows | Never relight on top of baked light. Baked vertex light multiplies albedo once, and tone mapping keeps the mid-tones. Check: average luminance per screen region within ±15% of Classic, in day and at night. |
| 6 | White streaks | Over-exposure made the dust decals read white | Decals and FX keep the game's blend. Check: no pixel at luminance 1.0 outside sky, sun, lights and emissive surfaces. |
| 7 | Solid walls from FX cards | Draws with no GS state were drawn with default state | Blend and alpha come only from the game's render state (the material table has no blend field). A batch with no state is skipped and logged, never drawn opaque. Check: `fx` and `emissive_light` cards show no opaque rectangles. |
| 8 | Lag (11→18.6 ms/frame, 14–23k draws) | Per-draw CPU cost in the GS shim, alpha-test pass/fail splits | No GS shim in the Modern path. Draws are batched by material, with no per-draw descriptor churn. Check: Modern costs at most 2 ms/frame more than Classic at 1x on Dundas with max cars (`EOD_GPU_PROF`), and the draw count is about the same as Classic. |
| 9 | 4x MSAA on top of supersampling | MSAA was left on with RenderScale > 1 | MSAA is off when RenderScale ≥ 2. |

## General rules

- **The Classic look stays byte-identical.** Every Modern change sits behind `Look=Modern`.
- **Test across scenes, not one track.** Use the Dundas day plus night flood-light rotation in `docs/TEST_ROTATION.md`, cycle every camera view, and use odd vehicles.
- **Compare side by side.** Save Classic and Modern captures of the same frame. Any of these is a fail, even if the numbers pass: obvious shards, a blown-out sky or ground, night that looks like day, opaque FX cards, or missing ground shadows.
