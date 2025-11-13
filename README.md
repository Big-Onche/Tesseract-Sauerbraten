# Tesseract: Sauerbraten

This project aims to fully port **Cube 2: Sauerbraten** to its fork, **Tesseract**, while maintaining full compatibility with legacy Sauerbraten servers.

---

## 🚧 Project Status and Roadmap

The game is almost fully playable, but some maps have poorly optimized lighting or even light flickering issues. The single-player port is also in early stages, with major issues on some maps.


### Playable Maps

The following is the current status of maps that are playable in multiplayer and free of glitches:
- **DM Maps:**  🟩🟩🟩🟩🟩🟩🟩🟩🟩⬛ **96%** complete.
- **CTF Maps:** 🟩🟩🟩🟩🟩🟩🟩🟩🟩⬛ **90%** complete.

### Roadmap
   - Finish implementing all of Sauerbraten's UI features.
   - Resolve light glitches.
   - Ensure older maps load properly.
   - Address all known issues for a perfect Sauerbraten port.
   - Improve audio and graphics with new, optional effects.
   - Upgrade maps with higher-resolution textures.

---

## 🤝 Contribute
We welcome all contributions!
- Submit a patch or feature via a [GitHub Pull Request](https://github.com/Big-Onche/Tesseract-Sauerbraten/pulls).
- Report bugs or suggest features on the [GitHub Issue Tracker](https://github.com/Big-Onche/Tesseract-Sauerbraten/issues).
- Fix lighting issues on maps (see 'maps status.xlsx') and this 'tutorial: [https://youtu.be/gTVatxg6p9s](https://youtu.be/gTVatxg6p9s)

---

## ❓ Q&A
**Can my potato computer run it?**
Yes! As long as your GPU or integrated graphics supports OpenGL 3.0+, the game should run at a decent framerate, even on older or low-end hardware.
Just note that vanilla Sauerbraten will always run faster, since Tesseract adds more advanced rendering features.

**How do I install the game on Linux?**
Install or update these libraries: `build-essential lib32z1-dev freeglut3-dev libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev`.
Then open a terminal in the project’s root directory and run: `make -C src install` to build the binaries.
After that, you can launch the game using: `./sauerract_unix.sh`.

**Will I get banned if I play on vanilla Cube 2: Sauerbraten servers?**
No. The gameplay and netcode are identical to Sauerbraten; only the rendering engine differs. This client gives no competitive advantage, so you can safely play on any Sauerbraten server.

**Can I edit maps with Tesseract: Sauerbraten and load them in Cube 2: Sauerbraten?**
Not yet. Maps edited in Tesseract currently do not load in vanilla Sauerbraten.
However, all existing Sauerbraten maps load correctly in this port, and full editing compatibility is planned for the future.

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
