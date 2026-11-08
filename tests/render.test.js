// GPU tests: render a synthetic scene (a hemisphere on a flat floor, seen from above) with the
// real shaders and check physical behaviour on the read-back pixels.
import { test, assert, near } from './harness.js';
import { Renderer } from '../js/renderer.js';
import { core } from '../js/core.js';

const W = 256, H = 192;
const cam = { aspect: W / H, tanHalfFov: Math.tan((55 * Math.PI) / 360), near: 1, far: 5 };

const photo = document.createElement('canvas');
photo.width = W; photo.height = H;
const pctx = photo.getContext('2d');
pctx.fillStyle = 'rgb(128,128,128)';
pctx.fillRect(0, 0, W, H);

// Floor at d = 0.3, hemisphere of radius R (in u units) in the centre. Row 0 = top.
const R = 0.15;
const depth = new Float32Array(W * H);
for (let y = 0; y < H; y++)
  for (let x = 0; x < W; x++) {
    const u = (x + 0.5) / W, v = 1 - (y + 0.5) / H;
    const q = Math.hypot(u - 0.5, (v - 0.5) / cam.aspect) / R;
    depth[y * W + x] = 0.3 + (q < 1 ? 0.5 * Math.sqrt(1 - q * q) : 0);
  }

const scene = new core.Scene();
scene.setDepth(depth, W, H, 0, 1e-4, 0, 0.04);
const canvas = document.createElement('canvas');
canvas.width = W; canvas.height = H;
document.body.appendChild(canvas);
const renderer = new Renderer(canvas);
const uniformShading = { data: new Float32Array(W * H).fill(0.2), mean: 0.2 };
renderer.setPhoto(photo, depth, W, H, uniformShading);

function lamp(u, v, s, intensity = 2) {
  return { pos: scene.lightPosition(u, v, s, cam.aspect, cam.tanHalfFov, cam.near, cam.far), col: [intensity, intensity, intensity] };
}

function params(over = {}) {
  return {
    cam, mode: 'relit', split: 0.5, scale: 1, ambient: 0, flatten: 0, exposure: 1, normalStrength: 1,
    wrap: 0, spec: 0, gloss: 32, haze: 0, shadow: 1, soft: 0.01, steps: 64,
    lights: [lamp(0.12, 0.5, 0.4)], view: core.orbitView(0, 0, 2, 1), ...over,
  };
}

function shot(over) {
  renderer.render(params(over));
  return renderer.readRelit();
}

// Mean luminance (0..255) of a 5x5 block around (u,v); v = 1 is the top.
function lum(img, u, v) {
  const cx = Math.round(u * img.width), cy = Math.round((1 - v) * img.height);
  let s = 0, n = 0;
  for (let y = cy - 2; y <= cy + 2; y++)
    for (let x = cx - 2; x <= cx + 2; x++) {
      const i = (y * img.width + x) * 4;
      s += 0.2126 * img.data[i] + 0.7152 * img.data[i + 1] + 0.0722 * img.data[i + 2];
      n++;
    }
  return s / n;
}

test('GPU: shaders compile and the relit image is not empty', () => {
  assert(lum(shot(), 0.3, 0.5) > 10, 'lit floor is black');
});

test('GPU: the bump casts a shadow on the side away from the lamp, not towards it', () => {
  const on = shot({ shadow: 1 }), off = shot({ shadow: 0 });
  const behind = [lum(on, 0.72, 0.5), lum(off, 0.72, 0.5)];
  assert(behind[0] < 0.7 * behind[1], `no shadow behind the bump: ${behind}`);
  near(lum(on, 0.3, 0.5), lum(off, 0.3, 0.5), 2, 'floor between lamp and bump must not be shadowed');
});

test('GPU: irradiance falls off with distance from the lamp', () => {
  const img = shot({ shadow: 0 });
  assert(lum(img, 0.2, 0.2) > lum(img, 0.9, 0.2) + 10, 'near floor not brighter than far floor');
});

test('GPU: with no lamps, uniform room light gives a uniform image', () => {
  const img = shot({ lights: [], ambient: 1 });
  const vals = [[0.1, 0.1], [0.5, 0.5], [0.9, 0.8], [0.3, 0.7], [0.55, 0.45]].map(([u, v]) => lum(img, u, v));
  assert(Math.max(...vals) - Math.min(...vals) < 2, `ambient image not uniform: ${vals.map((x) => x.toFixed(1))}`);
});

test('GPU: normals have no halo around the silhouette (edge-aware differences)', () => {
  // Flat floor just outside the bump must still face the camera: blue channel high, red ~ 128.
  const img = shot({ mode: 'normals' });
  for (const u of [0.5 + R + 0.01, 0.5 - R - 0.01]) {
    const i = (Math.round(0.5 * img.height) * img.width + Math.round(u * img.width)) * 4;
    near(img.data[i], 128, 12, `normal x tilted at u=${u.toFixed(2)}`);
  }
});

test('GPU: haze adds a glow around the lamp', () => {
  assert(lum(shot({ haze: 1 }), 0.12, 0.6) > lum(shot({ haze: 0 }), 0.12, 0.6) + 5, 'haze made no glow');
});

test('GPU: light pass is cached when nothing changed and re-runs when a lamp moves', () => {
  const p = params();
  renderer.render(p);
  const n0 = renderer.relightPasses;
  renderer.render(p);
  renderer.render({ ...p, split: 0.3, mode: 'split' });
  assert(renderer.relightPasses === n0, `view-only change re-ran the light pass (${renderer.relightPasses - n0}x)`);
  renderer.render({ ...p, lights: [lamp(0.2, 0.5, 0.4)] });
  assert(renderer.relightPasses === n0 + 1, 'moving a lamp did not re-run the light pass');
});

test('GPU: half-resolution preview fills the display without stale borders', () => {
  const full = renderer.readScreen && (renderer.render(params()), renderer.readScreen());
  renderer.render(params({ scale: 0.5 }));
  const half = renderer.readScreen();
  const i = (Math.round(H * 0.25) * W + Math.round(W * 0.85)) * 4;   // a corner region
  near(half.data[i], full.data[i], 12, 'preview differs from full quality');
});

test('GPU: the 3D view at rest shows the same picture as the flat view', () => {
  const bg = scene.buildBackground(new Uint8Array(W * H * 4).fill(128), 0.04, 8);
  renderer.setBackground(bg.color, bg.depth, W, H, uniformShading);
  const mesh = scene.buildMesh(200, 0.04), full = scene.buildMesh(200, 0);
  renderer.setMesh(mesh.uv, mesh.indices, full.indices);
  renderer.render(params());
  const flat = renderer.readScreen();
  renderer.render(params({ mode: '3d' }));
  const relief = renderer.readScreen();
  let diff = 0, n = 0;
  for (let y = 10; y < H - 10; y += 7)
    for (let x = 10; x < W - 10; x += 7) {
      const i = (y * W + x) * 4;
      diff += Math.abs(flat.data[i] - relief.data[i]);
      n++;
    }
  assert(diff / n < 4, `mean difference ${(diff / n).toFixed(1)} / 255`);
});

test('GPU: before/after split shows the original photo left of the line', () => {
  renderer.render(params({ mode: 'split', split: 0.5 }));
  const gl = renderer.gl, px = new Uint8Array(4);
  const r = renderer.fitRect();
  gl.readPixels(r.x + Math.round(r.w * 0.2), r.y + Math.round(r.h * 0.5), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
  near(px[0], 128, 2, 'original pixel');
});
