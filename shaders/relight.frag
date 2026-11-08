#version 300 es
// Pass 1: relight the photo (or the inpainted background layer) into an RGBA8 target.
precision highp float;
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uImage;          // SRGB8_ALPHA8 -> linear on fetch
uniform sampler2D uShading;        // estimated illumination (linear luminance), from C++
uniform float uShadingMean;
#include "common.glsl"
uniform vec2 uTexel;               // 1 / depth resolution
uniform int uMode;                 // 0 relit, 1 depth, 2 normals
uniform float uAmbient, uFlatten, uExposure, uNormalStrength, uWrap;
uniform float uSpec, uGloss, uHaze, uShadow, uSoft;
uniform int uSteps, uNumLights;
uniform vec3 uLightPos[4];
uniform vec3 uLightCol[4];         // colour * intensity

float ign(vec2 p) {                // interleaved gradient noise, for jittering shadow rays
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Screen-space shadow ray: march from the surface towards the light through the depth map.
// A sample is occluded when the reconstructed surface at its pixel lies in front of the ray
// (closer to the camera), but not by more than a thickness proportional to depth.
float shadowRay(vec3 P, vec3 n, vec3 Lp, float jitter) {
  if (uSteps == 0 || uShadow <= 0.0) return 1.0;
  vec3 start = P + n * (0.006 * -P.z);
  float occ = 0.0;
  for (int k = 0; k < 96; k++) {
    if (k >= uSteps) break;
    float t = (float(k) + jitter) / float(uSteps);
    t *= t;                                     // denser near the receiver: contact shadows
    vec3 S = mix(start, Lp, t);
    vec2 uvS = toUv(S);
    if (any(lessThan(uvS, vec2(0.0))) || any(greaterThan(uvS, vec2(1.0)))) break;
    float sceneZ = viewZ(uvS);
    float diff = -S.z - sceneZ;                 // > 0: the surface is in front of the ray
    float bias = 0.004 * sceneZ;
    float thick = 0.3 * sceneZ;
    float o = smoothstep(bias, bias + uSoft * sceneZ, diff)
            * (1.0 - smoothstep(thick, 2.0 * thick, diff));
    occ = max(occ, o);
    if (occ > 0.99) break;
  }
  return 1.0 - occ * uShadow;
}

vec3 turbo(float x) {              // Turbo colormap, polynomial fit (Google AI, Apache-2.0)
  const vec4 kR4 = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
  const vec4 kG4 = vec4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
  const vec4 kB4 = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
  const vec2 kR2 = vec2(-152.94239396, 59.28637943);
  const vec2 kG2 = vec2(4.27729857, 2.82956604);
  const vec2 kB2 = vec2(-89.90310912, 27.34824973);
  x = clamp(x, 0.0, 1.0);
  vec4 v4 = vec4(1.0, x, x * x, x * x * x);
  vec2 v2 = v4.zw * v4.z;
  return vec3(dot(v4, kR4) + dot(v2, kR2), dot(v4, kG4) + dot(v2, kG2), dot(v4, kB4) + dot(v2, kB2));
}

// Edge-aware normal: on each axis take the one-sided difference with the smaller depth change.
// A central difference across a silhouette mixes foreground and background and produces a halo
// of tilted normals around every object; the one-sided choice stays on the pixel's own surface.
vec3 normalAt(vec2 uv, vec3 c, vec2 e, vec3 V) {
  vec3 r = posAt(uv + vec2(e.x, 0.0)) - c, l = c - posAt(uv - vec2(e.x, 0.0));
  vec3 t = posAt(uv + vec2(0.0, e.y)) - c, b = c - posAt(uv - vec2(0.0, e.y));
  vec3 dx = abs(r.z) < abs(l.z) ? r : l;
  vec3 dy = abs(t.z) < abs(b.z) ? t : b;
  vec3 n = normalize(cross(dx, dy));
  return dot(n, V) < 0.0 ? -n : n;
}

vec3 aces(vec3 x) {                // Narkowicz ACES filmic fit
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 toSRGB(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
  vec3 P = posAt(vUv);
  vec3 V = normalize(-P);
  // Depth arrives denoised by the C++ guided filter; two stencil sizes (2 and 6 texels) keep
  // fine shape and suppress what noise is left.
  vec3 n = normalize(normalAt(vUv, P, uTexel * 2.0, V) + normalAt(vUv, P, uTexel * 6.0, V));
  n = normalize(mix(V, n, uNormalStrength));

  if (uMode == 1) { outColor = vec4(turbo(texture(uDepth, vUv).r), 1.0); return; }
  if (uMode == 2) { outColor = vec4(n * 0.5 + 0.5, 1.0); return; }

  // Albedo estimate: divide out the photo's own lighting. The illumination map comes from the
  // C++ core (edge-preserving guided filter on log-luminance, core/src/shading.cpp), so there are
  // no halos around objects as a plain blur would leave.
  vec3 img = texture(uImage, vUv).rgb;
  float S = texture(uShading, vUv).r;
  vec3 albedo = mix(img, min(img * uShadingMean / max(S, 0.01), vec3(1.0)), uFlatten);

  float jitter = ign(gl_FragCoord.xy);
  vec3 lit = albedo * uAmbient;
  vec3 haze = vec3(0.0);
  vec3 D = normalize(P);
  float tP = length(P);
  for (int i = 0; i < 4; i++) {
    if (i >= uNumLights) break;
    vec3 Lp = uLightPos[i];
    vec3 L = Lp - P;
    float d2 = dot(L, L);
    vec3 l = L * inversesqrt(d2);
    float ndl = (dot(n, l) + uWrap) / (1.0 + uWrap);   // wrap hides small normal errors
    if (ndl > 0.0) {
      // Inverse square with a small source radius (6 % of the lamp's distance) so the
      // irradiance stays finite right next to the lamp. Monocular depth has no absolute scale,
      // so intensity is normalized to the lamp's own distance: intensity 1 gives irradiance 1 at
      // a quarter of that distance, whatever the photo.
      float lz2 = dot(Lp, Lp);
      float att = 0.0625 * lz2 / (d2 + 0.0036 * lz2);
      // Skip the (expensive) shadow ray where this lamp adds less than ~1/1000 of white:
      // a shadow there could not change the 8-bit result.
      float contrib = att * ndl * max(uLightCol[i].r, max(uLightCol[i].g, uLightCol[i].b)) * uExposure;
      float sh = contrib < 0.001 ? 1.0 : shadowRay(P, n, Lp, jitter);
      vec3 h = normalize(l + V);
      float spec = pow(max(dot(n, h), 0.0), uGloss) * uSpec * (uGloss + 8.0) / 25.13;
      lit += uLightCol[i] * att * sh * ndl * (albedo + spec);
    }
    // Closed-form airlight (see docs/math.md): glow of the lamp in a hazy room.
    float b = dot(Lp, D);
    float hh = max(length(Lp - b * D), 1e-3);
    // A has units 1/length; scaling by the lamp distance keeps it scale-invariant too.
    haze += uLightCol[i] * (atan((tP - b) / hh) - atan(-b / hh)) / hh * 0.0125 * length(Lp);
  }
  vec3 c = (lit + haze * uHaze) * uExposure;
  // Per-channel ACES pushes saturated warm light towards yellow-green. Tone-map the brightest
  // channel instead (keeps the hue) and blend in a little per-channel ACES so very bright
  // lamps still burn towards white, like a real camera.
  float m = max(max(c.r, c.g), c.b);
  vec3 hue = m > 1e-5 ? c * (aces(vec3(m)).r / m) : c;
  c = toSRGB(mix(hue, aces(c), 0.25));
  c += (jitter - 0.5) / 255.0;                         // dither: no banding in the glow
  outColor = vec4(c, 1.0);
}
