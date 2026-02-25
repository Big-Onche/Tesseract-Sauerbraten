# Volumetric Clouds (Technique Overview)

## What It Is

The clouds are rendered as a **screen-space volumetric effect**:

- A fullscreen shader raymarches through a configurable cloud layer in world space.
- Cloud density is generated procedurally with layered noise (fBM-style).
- Lighting uses Beer-Lambert style transmittance plus a Henyey-Greenstein phase approximation.
- The result is rendered to a low-resolution buffer, then upscaled/composited over the scene.

## Cloud Rendering Pipeline

1. Build a cloud layer volume from:
- layer center height (percent of map/world height)
- layer thickness (percent of map/world height)

2. Reconstruct world position from scene depth for each pixel.

3. Raymarch through the cloud layer:
- sample procedural density
- apply vertical shaping (soft bottom/top falloff)
- use per-pixel temporal jitter (quantized time seed) to reduce banding / support temporal averaging
- accumulate color + alpha along the view ray

4. Estimate sunlight inside clouds:
- short sun-direction sampling from each cloud sample
- convert density to transmittance for soft self-shadowing
- apply phase-weighted directional lighting (HG forward-scattering style)

5. Composite the cloud result back into the HDR scene using premultiplied alpha.

## Performance Strategy

- Clouds are rendered at reduced resolution if needed (`vcscale`) for speed.
- Optional bilateral blur/upscale smooths noise while preserving depth edges.
- Current blur path is a separable 2-pass bilateral (horizontal + vertical, 9 taps each pass).
- Blur radius is alpha-adaptive (more blur in lower-alpha / noisier cloud regions).
- Cloud quality and cost are controlled by density, scale, blur, and shadow settings.

## Cloud Shadows (Ground / Scene Shadowing)

Cloud shadows use a **2D world-space shadow map** (camera-centered):

- A 2D texture is generated in XY world coordinates around the camera.
- Each texel samples cloud density at 1 or more heights in the cloud layer.
- Density is converted to sunlight transmittance (`0..1` shadow factor).

This shadow map is then applied in a fullscreen pass:

- Reconstruct world position from depth
- Project the point toward the cloud plane along sun direction
- Sample the cloud shadow map
- Multiply scene lighting by the sampled factor

### Stability / Quality Notes

- The 2D shadow map center is snapped to texel-sized world steps to avoid crawling.
- Horizon fade/clamping reduces artifacts when the sun is near the horizon.
- Optional PCF smoothing reduces pixelation in low-resolution shadow maps.
- Cloud layer entry/exit raymarch start is kept continuous (tiny epsilon only) to avoid a disappearance band when crossing layer boundaries.
