# Tesseract: Sauerbraten

This project aims to fully port **Cube 2: Sauerbraten** to its fork, **Tesseract**, while maintaining full compatibility with legacy Sauerbraten servers.

---

## 🚧 Project Status and Roadmap

The game is **playable** but still in the early stages. Significant work remains to fix, complete, and polish various elements (e.g., the scoreboard, and menus). Some older maps, (such as ~~face-camper~~ face-capture), are currently not loadable, while others have lighting issues.

### Playable Maps

The following is the current status of maps that are playable in multiplayer and free of glitches:
- **DM Maps:**  🟩🟩🟩🟩🟩🟩🟩🟩🟩⬛ **93%** complete.
- **CTF Maps:** 🟩🟩🟩🟩🟩🟩🟩🟩⬛⬛ **81%** complete.


### Roadmap

   - Finish implementing all of Sauerbraten's UI features.
   - Resolve light glitches.
   - Ensure older maps load properly.
   - Address all known issues for a perfect Sauerbraten port.
   - Improve audio and graphics with optional new effects.
   - Upgrade maps with higher-resolution textures.

---

## 🤝 Contribute
We welcome all contributions!
- Submit a patch or feature via a [GitHub Pull Request](https://github.com/Big-Onche/Tesseract-Sauerbraten/pulls).
- Report bugs or suggest features on the [GitHub Issue Tracker](https://github.com/Big-Onche/Tesseract-Sauerbraten/issues).
- Fix lighting issues on maps (see 'maps status.xlsx') and this 'tutorial: [https://youtu.be/gTVatxg6p9s](https://youtu.be/gTVatxg6p9s)

---

## 🕹 Features Provided by Tesseract

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
