# Tesseract: Sauerbraten

This project attempts to port Sauerbraten to its fork Tesseract.
While the game is fully playable both online and offline (except SP, not tested) the project is still in an early phase and a lot of stuff must be fixed, finished and polished (scoreboard, menus, etc.), please note that older maps (such as ~~face-camper~~ face-capture) is currently not playable.


## What Tesseract provides?

Tesseract is a fork of the Cube 2: Sauerbraten engine. The goal of Tesseract is to make mapping more fun by using modern dynamic rendering techniques, so that you can get instant feedback on lighting changes, not just geometry.

No more long calclight pauses... just plop down the light, move it, change its color, or do whatever else with it. It all happens in real-time now.

Tesseract removes the static lightmapping system of Sauerbraten and replaces it with completely dynamic lighting system based on deferred shading and shadowmapping.

**It provides a bunch of new rendering features such as:**

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