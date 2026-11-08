# The math

The C++ core ([core/src](../core/src), tested with GoogleTest in [core/tests](../core/tests)) and
the GLSL in [shaders/](../shaders) implement these formulas with the same names;
[shaders/common.glsl](../shaders/common.glsl) mirrors [camera.cpp](../core/src/camera.cpp).

## 1. From network output to 3D

Monocular depth networks predict **relative inverse depth** (disparity), with an unknown scale and
offset. The raw output `r` is normalized robustly. The 0.5th and 99.5th percentiles map to 0 and 1,
so single hot pixels (a specular highlight, a patch of sky) cannot squash the useful range:

$$d = \mathrm{clamp}\left(\frac{r - r_{0.5\%}}{r_{99.5\%} - r_{0.5\%}},\,0,\,1\right)$$

`d = 1` is the nearest point and `d = 0` the farthest. We interpret `d` linearly **in 1/z** between
a near plane (1) and a far plane (the *Depth range* slider, `f`):

$$\frac{1}{z} = \frac{1}{f} + d\left(1 - \frac{1}{f}\right)$$

Linear in disparity is what the network actually estimates, and it keeps near objects detailed.
Linear in `z` would flatten them.

A pinhole camera with vertical half-angle θ (the *field of view* slider) and image aspect `a`
unprojects texture coordinate (u, v) to view space (camera at the origin, looking down −z):

$$P = \big((2u-1)\,a\tan\theta\,z,\;\; (2v-1)\tan\theta\,z,\;\; -z\big)$$

`project()` is the exact inverse; a test round-trips 1000 random points to 10⁻¹².

## 2. Cleaning the depth (C++)

**Denoising: guided filter.** The prediction carries low-amplitude noise that normals turn into an
"orange peel" texture. The self-guided filter (He et al. 2010) fits, in every window, `q = a·d + b`
with

$$a = \frac{\operatorname{cov}(d, d)}{\operatorname{var}(d) + \varepsilon}, \qquad b = \bar d - a\,\bar d$$

and averages the coefficients of all windows covering a pixel. Where the window is flat
(`var ≪ ε`), `a → 0` and the output is the local mean: noise is removed. Across a silhouette
(`var ≫ ε`), `a → 1` and the step is kept. All means are box filters from a summed-area table in
double precision, O(1) per pixel. ε = 10⁻⁴ matches noise of about 0.01 in disparity.

**Sharpening: toggle mapping.** The network blurs every silhouette into a ramp 5–20 px wide. In 3D
a ramp becomes a stretched sheet; in the normals, a halo. No smoothing filter can fix that, because
the ramp *is* the signal. Toggle mapping replaces each pixel by the nearer of its local extremes,
but only where the local range is large:

$$d' = \begin{cases} \min_W d & \text{if } \max_W d - \min_W d > \tau \text{ and } d - \min_W d < \max_W d - d \\ \max_W d & \text{if } \max_W d - \min_W d > \tau \text{ otherwise} \\ d & \text{else} \end{cases}$$

With a 9 × 9 window and τ = 0.12, anything steeper than 0.013 per pixel is treated as a
silhouette. Real surfaces in a normalized depth map stay well below that (a floor is about 0.002
per pixel). A unit test checks both sides: an 8-px ramp becomes a step, and a smooth surface at
0.005–0.009 per pixel is returned bit for bit.

## 3. Normals

Normals come from differences of the reconstructed positions, oriented towards the camera:

$$n = \frac{\Delta_u P \times \Delta_v P}{\lVert\cdot\rVert}$$

On each axis the shader takes the **one-sided** difference with the smaller depth change instead of
the central difference. A central difference straddling a silhouette mixes the foreground and the
background and tilts the normal; the one-sided choice stays on the pixel's own surface (a GPU
test checks the floor next to an object still faces the camera). Two stencils (2 and 6 texels) are
averaged, then blended towards the view vector by the *Surface detail* slider.

## 4. Direct light

For a lamp at `L` with colour × intensity `Φ`, at surface point `P` with normal `n`, view vector
`V`, `l = (L−P)/|L−P|`, `h = (l+V)/|l+V|`:

$$E = \Phi\;\frac{k\,|L|^2}{|L-P|^2 + (0.06\,|L|)^2}\;\frac{n\cdot l + w}{1 + w}\;S$$

$$C = \rho\,(a + \textstyle\sum E) + \sum E\, s\,\frac{g+8}{8\pi}\,(n\cdot h)^{g}$$

- **Inverse square** with a source radius of 6 % of the lamp's distance, so the irradiance stays
  finite next to the lamp.
- **Scale normalization** `k|L|² (k = 1/16)`: monocular depth has no absolute scale, so a lamp's
  power is expressed relative to its own distance from the camera. Intensity 1 gives irradiance 1
  at a quarter of that distance in every photo, and the sliders behave the same for a close-up
  and for a whole room.
- **Wrap lighting** `w` (*Light wrap*) softens the terminator to hide small normal errors.
- **Blinn-Phong** specular with the energy-normalizing factor (g+8)/8π.
- `ρ` is the albedo estimate (§7), `a` the *Room light*, `S` the shadow factor (§5).

### Lamp placement

A lamp is defined by where it appears (u, v) and by a depth slider s ∈ [0, 1]. Its distance is
interpolated in disparity like the scene (`d_lamp = 1.15 (1 − s)`, so s = 0 is slightly in front
of the nearest object), then clamped to at most 97 % of the distance of the surface under it:

$$z_L = \min\big(z(d_{lamp}),\; 0.97\,z_{surface}(u,v)\big),\qquad L = z_L\,\text{ray}(u,v)$$

The lamp therefore always sits on the camera ray through its icon: the icon marks exactly where the
lamp projects into the photo, and the lamp can never end up behind a wall. Both properties are
unit-tested.

## 5. Screen-space shadow rays

From `P` (pushed 0.6 % along `n` to avoid self-shadowing) the shader marches N steps towards `L`.
Each sample `S` is projected back into the depth map:

```
for k in 0..N-1:
    t = ((k + jitter) / N)²          # squared: dense near the receiver (contact shadows)
    S = mix(P, L, t)
    uv = project(S);  if uv outside the photo: stop (no information, assume lit)
    Δ = z_ray(S) − z_scene(uv)       # > 0: the photo's surface is in front of the ray
    occlusion = max(occlusion, smoothstep(bias, bias + soft·z, Δ) · (1 − smoothstep(T, 2T, Δ)))
```

- The **thickness** limit `T = 0.3 z` handles the fact that a depth map only stores the *front*
  surface: a ray passing far behind an object is not blocked by it.
- **Jitter** with interleaved gradient noise per pixel turns step banding into fine noise.
- The `smoothstep` on Δ gives penumbrae whose width is the *Shadow softness* slider.

## 6. Volumetric glow: the airlight integral

In a thin homogeneous medium, the light a point source scatters towards the camera along the view
ray `x(t) = tD` (|D| = 1) up to the surface at distance `t_P` is proportional to

$$A = \int_0^{t_P} \frac{dt}{\lVert tD - L\rVert^2}$$

With `b = D·L` (the ray parameter of closest approach) and `h = ‖L − bD‖` (the closest distance),
`‖tD − L‖² = (t − b)² + h²`, so

$$A = \int_0^{t_P}\frac{dt}{(t-b)^2 + h^2} = \frac{1}{h}\left[\arctan\frac{t_P - b}{h} - \arctan\frac{-b}{h}\right]$$

One `atan` pair per lamp per pixel gives an exact glow, including the right behaviour when an
object is in front of the lamp (the integral stops at `t_P`), with no ray marching. A is scaled by
`|L|` to stay scale-invariant (A has units of 1/length). The unit test compares this closed form
with Simpson's rule at 20,000 steps on 50 random configurations: the relative error is below 10⁻⁶.
Phase function and medium shadowing are ignored. This is a glow model, not participating-media
transport.

## 7. Removing the photo's own lighting (C++)

A photo is roughly albedo × illumination. Illumination varies smoothly except where geometry
changes, so an **edge-preserving** smoothing of log-luminance estimates it (a Retinex-style
decomposition). [shading.cpp](../core/src/shading.cpp) linearizes the sRGB pixels, takes
`log Y`, applies the guided filter twice (radius = 1/40 of the image, ε = 0.5 in log² units, so
texture is flattened but strong lighting edges survive) and exponentiates:

$$S = \exp\big(\mathrm{GF}(\mathrm{GF}(\log Y))\big), \qquad \rho = \mathrm{mix}\Big(I,\; \min\big(I\,\tfrac{\bar S}{\max(S, 0.01)}, 1\big),\; f\Big)$$

`f` is the *Remove original lighting* slider. A plain blur (the first version used a mip level)
crosses object edges, so dark objects brighten their surroundings and vice versa: halos. The
edge-preserving estimate does not. The C++ test lights a checkerboard with a 3.3× gradient; after
division the same albedo differs by less than 25 % across the image, while the checker contrast
survives.

## 8. Colour

- Photos are uploaded as `SRGB8_ALPHA8`, so the GPU linearizes them on fetch; all lighting is in
  linear light.
- **Colour temperature** presets use a curve fit of the Planckian locus (Tanner Helland),
  normalized to max channel = 1. Tests check that 6500 K is near white and that blue/red rises
  monotonically with temperature.
- **Tone mapping.** Per-channel ACES (Narkowicz fit) pushes saturated warm light towards
  yellow-green. The shader tone-maps the largest channel and scales the colour by the same factor
  (hue-preserving), then blends 25 % of per-channel ACES back in so very bright lamps still burn
  towards white, like a real camera. The result is encoded with the exact sRGB transfer function
  and dithered by ±½ LSB to remove banding in the glow.

## 9. Layered 3D (C++)

**Silhouettes.** A pixel is on the near side of a silhouette when its disparity exceeds a
4-neighbour's by more than τ (the *3D edge cut*, 0.04).

**Background layer.** Behind a foreground object the photo has no pixels, yet a turning camera
uncovers them. [layers.cpp](../core/src/layers.cpp) builds a second layer:

1. Starting from the silhouette pixels, a breadth-first search marks a band of the foreground
   (3 % of the image size) to remove. Each band pixel remembers the disparity `d_far` of the
   background behind its seed, and the band only grows through pixels still clearly in front of
   it (`d > d_far + τ/2`).
2. The band is filled by **propagation from the background side**. In repeated sweeps, every band
   pixel that touches *usable* pixels takes their mean depth and colour. Usable means already
   filled, or original and lying behind the band (`d ≤ d_far + τ/2`). The fill therefore grows
   inwards from the background and can never pick up the foreground's own interior. A first
   version used push-pull (pyramid) interpolation with far-biased weights, and the unit test caught
   foreground leaking in: 0.31 instead of 0.2.
3. Twelve 3 × 3 averaging passes restricted to the band remove the streaks propagation leaves.
4. The result is clamped never to be nearer than the original.

**Meshes.** [mesh.cpp](../core/src/mesh.cpp) builds a regular grid (at most 400 × 400 vertices)
carrying only (u, v); the vertex shader reads depth from a texture, so one mesh serves any depth
range or field of view. The photo mesh leaves out every triangle whose vertex disparities differ by
more than τ. The test checks that exactly the one column of quads across a step disappears. The
background layer uses the full grid and is drawn first with a polygon offset.

**Camera.** The view orbits a pivot at the depth of the image centre:

$$V = T(0,0,-z_p\,\text{zoom})\;R_x(\text{pitch})\;R_y(\text{yaw})\;T(0,0,z_p)$$

`V` is the identity at rest ([mat4.cpp](../core/src/mat4.cpp), tested), and the projection over the
whole canvas widens the field of view by canvas/photo height, so the 3D view starts exactly on the
photo. A GPU test compares the two pictures. The orbit is limited to ±13°: a single photo supports
a modest parallax convincingly, and beyond that it would have to invent too much.

## 10. GIF encoder (C++)

"Save animation" and the README animation use [gif.cpp](../core/src/gif.cpp):

- **Median-cut quantization** to at most 256 colours per frame: sample ≤ 64k pixels, repeatedly
  split the box with the widest channel range at its median (`nth_element`), and take box means.
  Images with few colours are reproduced exactly.
- **Mapping** through a lazily filled 32³ lookup table, with **4 × 4 Bayer dithering** so the
  smooth glow does not band.
- **LZW** with variable code size (9–12 bits), clear code first and a dictionary reset at 4096
  codes. A generation counter makes each reset O(1) instead of wiping 4096 × 256 entries.
- The test decodes the stream with an independent decoder written from the GIF89a specification
  and compares indices exactly (random data, long runs, empty input).
