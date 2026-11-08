// The WebAssembly bindings of the C++ core. The algorithms themselves are tested in C++
// (core/tests); here we check that they arrive in JavaScript intact and with the right layout.
import { test, assert, near } from './harness.js';
import { core } from '../js/core.js';

test('wasm: matrices are column-major Float32Array(16) for WebGL', () => {
  const P = core.perspective(0.5, 1.5, 0.1, 50);
  assert(P instanceof Float32Array && P.length === 16, 'type');
  near(P[11], -1, 0, 'perspective w row is in column 2 (column-major)');
  near(core.transformPoint(P, 0, 1, -2)[1], 1, 1e-6, 'top edge');
  const q = core.transformPoint(core.orbitView(0, 0, 3, 1), 0.3, -0.2, -2.5);
  [0.3, -0.2, -2.5].forEach((v, k) => near(q[k], v, 1e-6, 'orbit identity at rest'));
  const M = core.multiply(P, core.orbitView(0.2, 0.1, 3, 1));
  assert(M.length === 16 && M.every(Number.isFinite), 'multiply');
});

test('wasm: depth file round trip within 1/65535, corrupt files rejected', () => {
  const w = 37, h = 23, data = new Float32Array(w * h).map((_, i) => (i * 7919 % 1000) / 999);
  const bytes = core.encodeDepth16(data, w, h);
  const back = core.decodeDepth16(bytes);
  assert(back.width === w && back.height === h, 'size');
  for (let i = 0; i < data.length; i++) near(back.normalized[i], data[i], 0.5 / 65535 + 1e-7, `px ${i}`);
  let threw = false;
  try { core.decodeDepth16(bytes.slice(0, 100)); } catch { threw = true; }
  assert(threw, 'truncated file accepted');
});

test('wasm: illumination estimate is positive with a sensible mean', () => {
  const w = 64, h = 32, rgba = new Uint8Array(w * h * 4).map((_, i) => (i % 4 === 3 ? 255 : (i >> 2) % w * 3));
  const s = core.estimateShading(rgba, w, h, 6, 0.5);
  assert(s.shading.length === w * h && s.shading.every((v) => v > 0), 'shading');
  assert(s.mean > 0 && s.mean < 1, `mean ${s.mean}`);
});

test('wasm: normalization and guided filter keep depth in [0,1]', () => {
  const scene = new core.Scene();
  const raw = new Float32Array(64 * 48).map((_, i) => 5 + Math.sin(i) + (i % 64 > 32 ? 10 : 0));
  const n = scene.setRawDepth(raw, 64, 48, 0, 1e-4);
  const f = scene.setDepth(n, 64, 48, 3, 1e-4, 3, 0.04);
  assert(n.length === 64 * 48 && f.length === 64 * 48, 'size');
  for (const v of f) assert(v >= 0 && v <= 1, `out of range: ${v}`);
  scene.delete();
});

test('wasm: lamp lies on the ray through its icon (C++ camera model)', () => {
  const scene = new core.Scene();
  scene.setDepth(new Float32Array(32 * 32).fill(0.3), 32, 32, 0, 1e-4, 0, 0.04);
  const t = Math.tan((55 * Math.PI) / 360), a = 1.5;
  const [x, y, z] = scene.lightPosition(0.2, 0.7, 0.3, a, t, 1, 5);
  near(x / (-z * a * t) * 0.5 + 0.5, 0.2, 1e-9, 'u');
  near(y / (-z * t) * 0.5 + 0.5, 0.7, 1e-9, 'v');
  scene.delete();
});

test('wasm: background layer and meshes come back with the right sizes', () => {
  const W = 80, H = 60, scene = new core.Scene();
  const d = new Float32Array(W * H).map((_, i) => ((i % W) > 30 && (i % W) < 50 ? 0.8 : 0.2));
  scene.setDepth(d, W, H, 0, 1e-4, 0, 0.04);
  const bg = scene.buildBackground(new Uint8Array(W * H * 4).fill(200), 0.04, 6);
  assert(bg.depth.length === W * H && bg.color.length === W * H * 4, 'background size');
  assert(bg.silhouettePixels > 0, 'no silhouettes found');
  const full = scene.buildMesh(40, 0), cut = scene.buildMesh(40, 0.04);
  assert(full.uv.length === full.cols * full.rows * 2, 'uv size');
  assert(cut.indices.length < full.indices.length, 'nothing was cut at the silhouettes');
  scene.delete();
});

test('wasm: GIF encoder writes a valid GIF89a', () => {
  const enc = new core.GifEncoder(16, 8, 5);
  for (let f = 0; f < 2; f++) enc.addFrame(new Uint8Array(16 * 8 * 4).map((_, i) => (i * 37 + f * 90) & 255));
  const bytes = enc.finish();
  enc.delete();
  assert(String.fromCharCode(...bytes.slice(0, 6)) === 'GIF89a', 'header');
  assert(bytes[bytes.length - 1] === 0x3b, 'trailer');
});

test('wasm: colour temperature', () => {
  const [r, g, b] = core.kelvinToRGB(1900);
  assert(r === 1 && g < r && b < g, `1900 K not warm: ${[r, g, b]}`);
});
