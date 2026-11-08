# Architecture

## Modules

```
index.html ─► js/main.js ─────────────► js/renderer.js ──► shaders/*.glsl  (WebGL2, via js/shaders.js)
                 │ UI, lamps, scheduling       │ textures, passes, layered mesh, GPU timing
                 │                             │
                 ├──► js/core.js ──► js/relight_core.js  (C++ core compiled to WebAssembly)
                 │                     core/src: camera · depth · shading · layers · mesh · mat4 · gif
                 │
                 └─ Web Worker ─► js/depth-worker.js ─► transformers.js ─► ONNX Runtime Web
                                                          (WebGPU fp16 | WASM int8)
```

| Module | Responsibility |
|---|---|
| `core/src/camera.cpp` | pinhole camera, disparity → depth, lamp placement, colour temperature, airlight, bilinear sampling |
| `core/src/depth.cpp` | robust normalization, box mean (summed-area table), guided filter, min/max filters, toggle-mapping edge sharpening, depth file format |
| `core/src/shading.cpp` | illumination estimate for "Remove original lighting" |
| `core/src/layers.cpp` | silhouette mask, background layer (propagation inpainting with push-pull fallback) |
| `core/src/mesh.cpp` | grid meshes, triangles across silhouettes removed |
| `core/src/mat4.cpp` | column-major 4×4 matrices: perspective, rotations, orbit camera |
| `core/src/gif.cpp` | GIF89a encoder: median-cut palette, ordered dithering, LZW |
| `core/wasm/relight_wasm.cpp` | Embind bindings; a `Scene` object keeps the processed depth in WebAssembly memory |
| `shaders/` | GLSL; `common.glsl` mirrors `camera.cpp` line for line |
| `js/renderer.js` | owns every GL object: `setPhoto`, `setBackground`, `setMesh`, `render(params)`, read-backs |
| `js/main.js` | everything the user touches; turns UI state into `render()` parameters |

The C++ core has no dependency on the browser: the same sources build as a static library with
MSVC for the GoogleTest suite and with Emscripten for the page. `renderer.render()` takes a plain
parameter object, which is what lets the GPU tests drive the real renderer with a synthetic scene.

## Data flow for a new photo

```
file ─► createImageBitmap (EXIF rotation) ─► working canvas (≤ 1024 / 1600 / 2048 px)
     ─► ≤ 1024 px RGBA ─► worker: Depth Anything ─► raw disparity
     ─► C++ Scene.setRawDepth   robust normalization                      (cacheable: .depth.bin)
     ─► C++ Scene.setDepth      guided filter → toggle-mapping sharpening
     ─► C++ estimateShading     illumination of photo and of background layer
     ─► C++ buildBackground     background depth + colour
     ─► C++ buildMesh ×2        photo mesh (cut at silhouettes) + full mesh (background)
     ─► GPU textures: photo (sRGB), depth (R32F), shading (R32F), background layer, meshes
```

Examples skip the worker: their normalized depth is stored in `samples/<id>.depth.bin` and goes
straight to `setDepth`, so filter settings can change without re-running the network.

## Rendering

**Pass 1, relight** (one full-screen triangle → RGBA8 texture per layer): reconstruct position,
compute edge-aware normals at two scales, divide out the estimated illumination, then for each
lamp add irradiance, specular, a shadow ray and the airlight. Finally tone mapping and sRGB
encoding.

- The output is display-ready, so pass 2 is a plain texture fetch.
- Each layer caches the pass by a key built from every uniform it uses.
- The background layer is only relit when the 3D view is shown.
- While the user drags, the pass renders into the lower-left quarter of its texture (half
  resolution) and pass 2 samples that quarter.

**Pass 2, display:**
- *Relit / Depth / Normals / Before-after*: a full-screen triangle into the letterboxed rectangle.
- *3D*: the background layer is drawn first, with `polygonOffset` so it never z-fights where it
  coincides with the photo, then the photo mesh, cut at silhouettes. The projection over the whole
  canvas is chosen so that the start pose matches the flat view pixel for pixel (a GPU test checks
  this).

**Lamps** are HTML elements over the canvas: hit testing, pointer capture and crisp labels for
free. In 3D they are projected with the C++ model-view-projection matrix.

## Decisions

| Decision | Alternatives | Why |
|---|---|---|
| Image processing in C++ → WebAssembly | JavaScript; GPU shaders | Filters with summed-area tables, breadth-first propagation and LZW are natural in C++, run once per photo, and are unit-tested natively. Per-frame work stays on the GPU. |
| Toggle-mapping sharpening | bilateral filter; guided filter with the photo as guide | Ramps must become *steps*. Edge-preserving smoothers keep a ramp as it is, and a photo-guided filter copies texture into the depth. Toggle mapping snaps only where the local range is large. |
| Background layer by propagation from the far side | push-pull everywhere; no layer (discard triangles) | Discarding leaves black holes. Push-pull lets the foreground's own interior leak into the hole (a test caught it: 0.31 instead of 0.2). Growing inwards from pixels known to be behind the foreground cannot leak. |
| Illumination by guided filter on log-luminance | mip-level blur in the shader | A blur crosses object edges and leaves halos; the edge-preserving filter does not. It runs once per photo. |
| Orbit limited to ±13° | free orbit | Beyond that, a single view shows what it cannot know. A small range looks solid. |
| WebGL2 by hand | three.js; WebGPU | Two passes and a mesh do not need a scene graph, and WebGL2 runs everywhere. |
| Depth Anything V2 **Small** | Base/Large | 27–50 MB and about 1 s on a laptop GPU, which suits a demo opened from a link. |
| Static site, committed WebAssembly | build on deploy | GitHub Pages serves the repository directly; no toolchain is needed to run it. |

## Testing

| Layer | What | Where |
|---|---|---|
| C++ | 30 GoogleTest tests: closed forms vs numeric integration, brute-force references (box mean, min/max), synthetic scenes for the filters, inpainting and meshes, a GIF round trip through an independent LZW decoder | `core/tests` |
| Bindings | matrices arrive column-major, depth files round-trip, sizes and ranges of every returned array | `tests/math.test.js` |
| GPU | real shaders on a synthetic hemisphere: shadow side, falloff, uniform ambient, no normal halo, haze, pass caching, half-resolution preview, 3D at rest = flat view, split view | `tests/render.test.js` |
| App | the real page in an iframe: idle = 0 frames, orbiting doesn't relight, a lamp move relights once, every example opens, GIF export, clean console | `tests/app.test.js` |

`scripts/test.ps1` runs all of it. The browser part runs headless Chrome against
`scripts/serve.ps1`; the page posts its report back to the server. No Node.js, Python or
browser-automation library is needed.
