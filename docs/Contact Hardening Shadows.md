# Contact-hardening sun shadows

Enable with `csmpcss 1`; restore the original CSM shader with `csmpcss 0` (the default).
PCSS requires OpenGL 3.3 or `GL_ARB_sampler_objects`. Unsupported drivers retain the original filtering path.
The minimap and volumetric shadow passes retain their existing filters.

PCSS searches the existing cascade depth map for blockers, converts their average separation from the receiver into world units,
and filters a disk with radius `separation * tan(0.5 * atmosundisksize * pi / 180) * csmpcsssoftness`.
`atmosundisksize` is the sky renderer's existing sun angular **diameter**, in degrees. It is the only sun-size setting;
the former independent `csmpcssangle` control has been removed. Remove that obsolete command from custom configurations.
PCSS reads the current disk size each frame, so changes require no shader rebuild and the angular scale varies continuously.
Small disks sharpen shadows; larger disks widen penumbrae until the existing world-space or quality limits are reached.
The sky's default diameter is 6 degrees; use 0.54 degrees for an approximately real-sun size, or tune `csmpcsssoftness`
to adjust the shadows independently without changing the visible disk. A zero disk size skips PCSS search and filtering.
Camera distance controls cost and fade, not the physical penumbra size.

| Control | Default | Meaning |
| --- | --- | --- |
| `csmpcss` | 0 | Enable the optional shader variant |
| `csmpcssquality` | 1 | 0: low, 1: medium, 2: high |
| `csmpcssblockers` | 12 | Requested blocker samples, 1–32 |
| `csmpcsssamples` | 16 | Requested filtering samples, 1–64 |
| `csmpcssdist` | 512 | Maximum receiver distance in world units; 0 skips PCSS |
| `csmpcssfade` | 0.25 | Fraction of that distance occupied by the smooth fallback fade |
| `csmpcssminradius` | 0 | Minimum penumbra radius in world units, limited by the maximum |
| `csmpcssmaxradius` | 8 | Maximum penumbra and search radius in world units |
| `csmpcsssoftness` | 1 | Multiplier on the angular spread; 0 skips PCSS |
| `csmpcsscascadescale` | 0.5 | 0 keeps equal cascade budgets; 1 scales them by `1/(cascade + 1)` |

All these controls persist in the saved configuration. Quality limits the requested counts:

| Quality | Blocker ceiling | Filter ceiling | Radius ceiling in atlas texels |
| --- | --- | --- | --- |
| 0 | 8 | 12 | 6 |
| 1 | 16 | 24 | 12 |
| 2 | 32 | 64 | 20 |

For example, use `csmpcssquality 2; csmpcssblockers 24; csmpcsssamples 48` for a higher sample budget.
Counts are also reduced by cascade index and through the distance fade. Sub-texel penumbrae use the original filter.
Radii normally convert through each cascade's world-to-texel scale; extreme settings can hit the quality ceiling and truncate softness.

The disk has a fixed golden-angle pattern, with no frame jitter, random-number generation, or additional noise texture.
During a solar eclipse, the existing Sun/Moon directions and angular sizes define a Moon mask in this disk's coordinates.
Covered blocker-search and filter samples are skipped before texture reads. Partial eclipses leave a crescent source;
annular eclipses leave a ring. Accepted samples are normalized by their weight, while the existing analytic solar visibility
attenuates direct sunlight exactly once. The mask follows atmosphere opacity when `atmoalpha` is below 1.
Totality skips PCSS shadow work; direct solar intensity reaches zero with a fully opaque Moon. No-overlap frames bypass
the per-sample eclipse math. There is no additional texture, shadow map, raymarch, or render pass.
Very thin crescents or rings can miss all samples at low budgets; these use the existing shadow filter with the correct
solar intensity rather than dividing by zero or inventing illumination. Raise the existing sample counts for finer eclipse silhouettes.
Receiver-plane correction uses geometric position derivatives, so normal-map detail does not tilt the blocker-search plane.
Existing raster and comparison bias controls remain active. Samples stay within the selected cascade's rendered footprint.
PCSS variants blend overlapping cascades near their edges; the original path retains its existing cascade selection unchanged.

The renderer binds a non-comparison sampler to the same depth texture on texture unit 13, while the existing comparison/gather
sampler stays on unit 4. No depth texture copy or extra shadow-caster pass is needed. The sampler is unbound after lighting
and released with the atlas. Alpha-tested casters, colored shadow modulation, existing `smfilter`/`smgather` modes, and atlas
debugging remain available. Colored modulation is sampled with each PCSS filter tap; translucent-only shadows without an opaque
depth blocker retain their existing filter.

## Validation

Run `exec tests/pcss.cfg` in a development build to compile the quality/filter/cascade variants. Check the console or log for
shader compilation/link errors and the final `PCSS_SHADER_CHECK_DONE` marker. This does not enable PCSS for gameplay.

For visual tuning, compare `csmpcss 0` and `csmpcss 1` on contact geometry, a raised platform, foliage, and a tall caster.
Move slowly through cascade boundaries and through the final quarter of `csmpcssdist`; check sloping receivers and normal-mapped
surfaces for acne, and verify colored and alpha-tested casters. Test `debugshadowatlas` and MSAA as usual. Performance depends
on visible shadowed area and cascade overlap; measure GPU frame time in the intended maps before raising sample counts or radii.
With PCSS enabled, vary `atmosundisksize` gradually from 0 through 0.54, 1.08, and 6 degrees. At a fixed blocker separation,
doubling a small diameter should approximately double the penumbra radius before clamping. Verify that disk-size changes
do not affect shadow filtering with `csmpcss 0`.
For eclipses, align the Moon with the Sun, compare smaller/equal/larger Moon diameters (annular/total), then offset the Moon
to either side for partial eclipses. Check that the asymmetric shadow edge reverses with the Moon offset and that normal
PCSS returns when the disks separate. Test low sample counts and atmosphere opacity below 1 for finite, stable output.
