# Performance and GPU budget

Relight is meant to be opened by anyone, on any laptop, and left open in a tab. The goal is not
"as many fps as possible" but **as little GPU work as the picture needs**.

## Rules

| Rule | How | Checked by |
|---|---|---|
| **No idle render loop** | `requestRender()` schedules one `requestAnimationFrame` only when something changed; with nothing happening the page draws **0 frames per second**. | app test *idle page draws no frames* |
| **Cache the expensive pass** | Each layer's light pass writes to a texture and re-runs only when a lighting uniform changes. Orbiting, the split slider or a resize cost one textured draw. | GPU test + app test *orbiting does not re-run the light pass* |
| **Progressive refinement** | While a lamp or slider is dragged, the light pass renders at **half resolution** (¼ of the pixels) with ⅓ of the shadow steps; 180 ms after the last change one full-quality pass is drawn. | GPU test *half-resolution preview* |
| **Work once per photo on the CPU** | Depth filtering, sharpening, illumination estimate, background layer and meshes run once in C++ (WebAssembly) when a photo opens, not per frame. | |
| **Bounded resolution** | Photos are scaled to at most 1024 / 1600 / 2048 px (Low / Medium / High); the canvas to at most 1.0 / 1.25 / 1.5 × device pixels; the AI input to 1024 px. | |
| **Opt-in animation, capped** | *Animate lights* and *Wiggle* are off by default, run at most 30 fps and stop in background tabs. | |
| **No stalls** | GPU time comes from `EXT_disjoint_timer_query_webgl2`, read back asynchronously. | |
| **AI off the main thread** | The model runs in a Web Worker and loads on first use only; the examples never download it. | |
| **Gentle GPU choice** | The context asks for `powerPreference: 'default'`, so laptops may stay on the integrated GPU. | |

## Measurements

Windows 11, Chrome (ANGLE / Direct3D 11) on the **integrated AMD Radeon** of a Ryzen desktop CPU:
the GPU Chrome picked for the page (the discrete GPU was not used). Median GPU time of the light
pass over 8 lamp positions, read with the timer query (the HUD shows the same number):

| Example | Working resolution | Quality | Full pass | While dragging |
|---|---|---|---|---|
| Living room | 1280 × 1066 | Low / Medium / High | 12.9 / 21.7 / 37.7 ms | 2.7 / 3.3 / 5.0 ms |
| Statue | 1006 × 1600 | Low / Medium / High | 14.7 / 26.2 / 45.8 ms | 3.2 / 3.5 / 5.7 ms |
| Castle bedroom (3 lamps) | 1600 × 1064 | Low / Medium / High | 20.0 / 34.9 / 61.0 ms | 4.4 / 4.7 / 7.0 ms |

The cost scales with pixels × lamps × shadow steps, as expected for a screen-space ray march.
Dragging at Medium is 6–7× cheaper than a full pass, well inside a 60 Hz frame even on integrated
graphics. The display pass (flat, or the layered 3D mesh with up to about 640k triangles) is well
under 1 ms.

C++ processing when a photo opens (WebAssembly, single thread, same machine):

| Example | Depth size | Filter + sharpen | Illumination ×2 | Background layer | Meshes | Total |
|---|---|---|---|---|---|---|
| Statue | 644 × 1024 | 51 ms | 57 ms | 104 ms | 6 ms | 217 ms |
| Living room | 1024 × 853 | 38 ms | 69 ms | 72 ms | 5 ms | 185 ms |
| Castle bedroom | 1024 × 681 | 30 ms | 54 ms | 45 ms | 4 ms | 134 ms |

AI depth (1024 × 853 input, steady state after the first run): **1.35 s** on WebGPU (fp16,
50 MB of weights), **3.0 s** on the CPU (WebAssembly, int8, 27 MB). The first photo adds about
3 s of initialization and shader compilation, plus the one-time download.

## What was tried

- **Skipping shadow rays for negligible light.** Kept because it is free, but it made no
  measurable difference: with distance-normalized lamps almost every pixel gets meaningful light.
- **Half-float depth (R16F).** Rejected: its 11-bit mantissa gives steps of 1/2048 near d = 1,
  visible as terraces in the normals. 32-bit float is used when the GPU can filter it
  (`OES_texture_float_linear`), with R16F as fallback.
- **Fewer shadow steps only, while dragging.** The first version of progressive refinement.
  Cutting the resolution as well gave another 4× for no visible cost during motion.

## Next steps

- Hierarchical depth (a min-max mip chain) so shadow rays skip empty space instead of taking
  fixed steps.
- Moving the light pass to a WebGPU compute shader that shares the device the AI already uses.
- Running the C++ preprocessing in a worker too: today its 130–220 ms run on the main thread when
  a photo opens.
