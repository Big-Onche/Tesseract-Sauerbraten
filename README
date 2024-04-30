Tesseract is a fork of the Cube 2: Sauerbraten engine. The goal of Tesseract
is to make mapping more fun by using modern dynamic rendering techniques, so
that you can get instant feedback on lighting changes, not just geometry.

No more long calclight pauses... just plop down the light, move it, change its
color, or do whatever else with it. It all happens in real-time now.

Tesseract removes the static lightmapping system of Sauerbraten and replaces
it with completely dynamic lighting system based on deferred shading and
shadowmapping.

It provides a bunch of new rendering features such as:

* deferred shading
* omnidirectional point lights using cubemap shadowmaps
* perspective projection spotlight shadowmaps
* orthographic projection sunlight using cascaded shadowmaps
* HDR rendering with tonemapping and bloom
* real-time diffuse global illumination for sunlight (radiance hints)
* volumetric lighting
* transparent shadows
* screen-space ambient occlusion
* screen-space reflections and refractions for water and glass (use as many water planes as you want now!)
* screen-space refractive alpha cubes
* deferred MSAA, subpixel morphological anti-aliasing (SMAA 1x, T2x, S2x, and 4x), FXAA, and temporal AA
* runs on both OpenGL Core (3.0+) and legacy (2.0+) contexts

Editing:

To make shadowmapped point lights, just make light entities as normal. Optionally you can control the shadowing properties of the light using attribute 5: 0 = shadows, 1 = no shadows, 2 = static shadows - world and mapmodels only (no playermodels or pickups). Shadowmaps are cached from frame to frame if no dynamic entities such as playermodels are rendered into them, so static lights (2) will generally be much faster than normal lights (0) during gameplay and should be used where possible. Unshadowed lights (1) are even faster as they further reduce shading costs, so they should also be used where possible.

To make cascaded shadowmapped sunlight, just use the normal sunlight commands:
sunlight
sunlightyaw
sunlightpitch
You can also set sunlightyaw and sunlightpitch from your current direction with the following command:
getsundir

You can control the diffuse global illumination of sunlight using the following parameters:
giscale - scales the contribution of indirect light
gidist - limits the distance which indirect light travels (smaller values are more intense, but higher values allow farther travel)
giaoscale - controls the contribution of atmospheric light
skylight - controls the color of atmospheric light

Screen-space ambient occlusion affects mostly ambient light and to a lesser degree sunlight,
but it does not affect point lights. If you want to make use of SSAO in your map, just use 
the ambient command to set an appropriate ambient light color/intensity.

To make refractive alpha cubes:
First mark the cubes with the alpha material. Use valpha or texalpha to set the transparency level as normal.
Then use either of the following commands:
vrefract K [R G B]
K is the strength of the refraction, on a scale of 0 to 1, 1 meaning distortion spanning the entire screen, 0.5 spanning half the screen, etc.
The direction of the distortion is taken from the normal-map of the texture.
R G B are optional and specify a color for the refraction, with each component being on a scale of 0 to 1 as well.

After editing operations your map may be full of small cubes and triangles, and you will notice that your map looks faceted.
To fix this you still need to use the calclight command, however, it no longer generates lightmaps anymore.
It just does a remip optimization pass as well as calculates smoothed normals to make things look better.
Eventually this may be done differently using some sort of background optimization while editing.

To make decals:

newent decal SLOT YAW PITCH ROLL SIZE

If you don't specify the yaw, pitch, and roll, or leave them all 0, then newent will fill them with current direction you are looking at.
The slot refers to a decal slot in the map configuration, starting at offset 0.
The size is specified in cube units.

To make decal slots:

decelreset // functions like texture reset, clears all decal texture slots

setshader bumpspecmapdecal  // shaders work like for world shaders, but with the -decal suffix instead of -world
texture decal "diffuse.png" // all decal slots start with the "decal" sub-slot instead of "0", and this texture must have an alpha-channel
texture n "normals.png"
texture s "spec.png"

To make volumetric lights:

Set attribute 5 of a light entity to 4.

The intensity of the volumetric lighting medium can be controlled by volscale and the color can by controlled by volcolour.

To make transparent shadows:

This is toggled on a per-map basis by setting "alphashadow" to 0 (off), 1 (sunlight), or 2 (sunlight and point lights). 
If set to 1, you must set attribute 5 of a light entity to 16 to enable transparent shadows for that point light.
If set to 2, all point lights automatically cast transparent shadows.
If a light is volumetric, regardless of "alphashadow" setting, the light entity's attribute 5 must be set to 20 (4 | 16) for it
to cast a volumetric transparent shadow.

To make a cube cast transparent shadows, use the "alpha" material. The texalpha/valpha commands can be then used to control the alpha
of the cube, which then is used to determine the intensity of the transparent shadow. This intensity can be further scaled by setting
the global map variable "alphashadowscale" (0..2).

If a texture slot has an alpha texture (slot "a"), this can be used to mask the alpha of the cube. This masked alpha will then be used
to further mask the transparent shadow.

