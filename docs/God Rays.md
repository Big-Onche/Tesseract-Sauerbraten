# God Rays (Cloud-Driven, World-Space)

## Overview

Cloud god rays are rendered in a fullscreen world-space pass (`volumetriccloudgodrays`) using:

- scene depth (world-position reconstruction),
- cloud shadow map (`volumetriccloudshadowmap`),
- sun direction (`sunlightdir`).

## Pipeline Order

In `vclouds::render()`:

1. Render volumetric clouds.
2. Build cloud shadow map.
3. Apply cloud shadows (optional).
4. Add god rays (additive).
5. Composite cloud color/alpha.

## Current Behavior

Per pixel, the pass raymarches from camera to scene hit/max distance and accumulates in-scattering.

- `sun above horizon`: shaft column is anchored at cloud top and trails below cloud layer.
- `sun near/below horizon`: direction switches to anti-solar mode so shafts project opposite the sun and rise upward.
- In below-horizon mode, shaft column is anchored in the upper cloud body and trails above the cloud layer.
- Cloud anchor test is direction-aware (`tshadow` sign depends on sun mode).
- Cloud visibility is multiplied by CSM geometry visibility (world + mapmodels) when enabled.
- Final result uses a soft luminance clamp to avoid low-sun oversaturation.

## Tunables

- `vcgodrays`: enable/disable pass.
- `vcgodraysteps`: raymarch steps.
- `vcgodraysscale`: pass resolution scale (`0` => full-res fallback).
- `vcgodraystrength`: base intensity.
- `vcgodraydensity`: extinction density.
- `vcgodraydist`: max trace distance (scaled by world size).
- `vcgodrayhorizonboost`: extra low-sun visibility boost.
- `vcgodrayclamp`: soft highlight rolloff.
- `vcgodraygeomshadow`: geometry shadow influence from CSM (`0..1`).
- `vcamount`: scales god-ray strength from subtle to full (`godray *= 0.20 + 0.80 * clamp(vcamount / 100, 0, 1)`).

Related cloud context: `vcalpha`, `vcheight`, `vcthickness`, `vcdome`, `vcshadowmapsize`, `vcshadowpcf`, `vcskyinherit`, `vccolour`.

## Notes

- Fidelity is limited by cloud shadow map resolution/filtering.
- Geometry occlusion uses CSM (world + shadow-casting mapmodels), sampled with step reuse.
- Current implementation applies geometry influence when CSM runs with rectangle shadow atlas target.
- Very deep below-horizon sun is faded out by twilight gating.

## Files

- `config/glsl/volumetric.cfg` (`volumetriccloudgodrays`)
- `src/engine/volumetric.cpp` (setup, params, integration)
