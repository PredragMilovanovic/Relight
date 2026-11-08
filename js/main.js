// Relight: application glue. UI state -> renderer parameters, photo loading, AI depth, lights.
//
// GPU budget rules (see docs/performance.md):
//   * draw only when something changed (no idle render loop: 0 frames per second while idle)
//   * the expensive relight pass is cached and skipped when only the view changes
//   * the photo is capped at a working resolution; the canvas is capped at 1.5x device pixels
//   * animations are opt-in, capped at 30 fps, and stop when the tab is hidden
//
// Depth processing, the background layer, 3D meshes, lamp placement and GIF encoding run in
// the C++ core (core/, compiled to WebAssembly: js/relight_core.js).
import { Renderer } from './renderer.js';
import { core } from './core.js';
import { clamp, hexToRGB, rgbToHex } from './math.js';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);

const QUALITY = {
  low:    { steps: 12, maxSide: 1024, dpr: 1.0 },
  medium: { steps: 28, maxSide: 1600, dpr: 1.25 },
  high:   { steps: 56, maxSide: 2048, dpr: 1.5 },
};
const AI_MAX_SIDE = 1024;       // the network works at 518 px anyway; keep its output modest
const MAX_LIGHTS = 4;
const KELVIN_PRESETS = [[1900, 'Candle'], [2700, 'Warm'], [4000, 'Neutral'], [6500, 'Daylight'], [12000, 'Sky']];

const DEFAULTS = {
  ambient: 0.22, flatten: 0.35, exposure: 1.2, haze: 0.25,
  normalStrength: 0.85, wrap: 0.25, spec: 0.15, gloss: 32,
  shadow: 0.85, soft: 0.02,
  depthRange: 5, fov: 55, cut: 0.04,
};

// Depth post-processing in the C++ core (see docs/math.md).
const FILTER_EPS = 1e-4;                       // guided filter: noise variance to smooth away
const filterRadius = (w, h) => Math.max(2, Math.round(Math.max(w, h) / 200));
const SHARPEN = { radius: 4, threshold: 0.12 };  // edge snapping: ramps steeper than 0.013/px
const bandRadius = (w, h) => Math.round(Math.max(w, h) * 0.03);   // background layer band
const shadingRadius = (w, h) => Math.max(4, Math.round(Math.max(w, h) / 40));
const SHADING_EPS = 0.5;                       // guided filter on log-luminance (see shading.hpp)
const MESH_SIDE = 400;
const ORBIT = { yaw: 0.22, pitch: 0.13 };      // ±13°: a single photo only supports modest parallax

const state = {
  ...DEFAULTS,
  mode: params.get('mode') || 'relit',
  split: 0.5,
  quality: params.get('quality') || 'medium',
  aiDevice: params.get('device') === 'wasm' ? 'wasm' : 'auto',
  animate: false,
  wiggle: false,
  lights: [],
  selected: 0,
  orbit: { yaw: 0, pitch: 0, zoom: 1 },
};

let renderer;
const scene = new core.Scene(); // processed depth lives in WebAssembly memory
let photo = null;               // { canvas, name }
let depth = null;               // { normalized (Float32Array, row 0 = top), width, height }
let worker = null, workerReady = null, jobId = 0;
const pendingJobs = new Map();
let aiInfo = { device: null, ms: null };
let lastFrameT = 0, rafPending = false, animStart = performance.now();
let interacting = false, settleTimer = 0;

// Progressive refinement: while the user drags something the light pass runs at half resolution
// with a third of the shadow steps; 180 ms after the last change one full-quality pass is drawn.
function interact() {
  interacting = true;
  clearTimeout(settleTimer);
  settleTimer = setTimeout(() => { interacting = false; requestRender(); }, 180);
  requestRender();
}
let lightIdCounter = 0;
let samples = [];

// ---------------------------------------------------------------------------------------------
// Camera and lights
// ---------------------------------------------------------------------------------------------
function camera() {
  const a = photo ? photo.canvas.width / photo.canvas.height : 1.5;
  return { aspect: a, tanHalfFov: Math.tan((state.fov * Math.PI) / 360), near: 1, far: state.depthRange };
}

const kelvinHex = (k) => rgbToHex(core.kelvinToRGB(k));

function makeLight(u, v, kelvin = 2700, intensity = 1.6, depth = 0.25) {
  return { id: ++lightIdCounter, u, v, depth, intensity, kelvin, color: kelvinHex(kelvin) };
}

function placeLight(L, t, cam) {
  let u = L.u, v = L.v;
  if (state.animate) {
    const a = t * 0.0006 + L.id * 2.1;
    u = clamp(u + Math.cos(a) * 0.08, 0.02, 0.98);
    v = clamp(v + Math.sin(a * 1.3) * 0.06, 0.02, 0.98);
  }
  return { pos: scene.lightPosition(u, v, L.depth, cam.aspect, cam.tanHalfFov, cam.near, cam.far), u, v };
}

function defaultLights() {
  return [makeLight(0.28, 0.62, 2700, 1.8, 0.2), makeLight(0.76, 0.42, 6500, 1.1, 0.3)];
}

// ---------------------------------------------------------------------------------------------
// Rendering (on demand)
// ---------------------------------------------------------------------------------------------
function requestRender() {
  if (rafPending || !renderer) return;
  rafPending = true;
  requestAnimationFrame(frame);
}

function animating() {
  return (state.animate || (state.wiggle && state.mode === '3d')) && !document.hidden;
}

function frame(t, force = false) {
  if (!force) rafPending = false;
  if (!renderer?.ready) return;
  if (!force && animating() && t - lastFrameT < 32) { requestRender(); return; }   // 30 fps cap
  lastFrameT = t;
  const cam = camera();
  const placed = state.lights.map((L) => ({ L, ...placeLight(L, t - animStart, cam) }));
  const lights = placed.map(({ L, pos }) => ({
    pos: pos.map((x) => +x.toFixed(5)),
    col: hexToRGB(L.color).map((c) => +(c * L.intensity).toFixed(4)),
  }));
  const view = viewMatrix(t);
  const steps = QUALITY[state.quality].steps;
  renderer.render({
    cam, mode: state.mode, split: state.split, scale: interacting ? 0.5 : 1,
    ambient: state.ambient, flatten: state.flatten, exposure: state.exposure,
    normalStrength: state.normalStrength, wrap: state.wrap, spec: state.spec, gloss: state.gloss,
    haze: state.haze, shadow: state.shadow, soft: state.soft,
    steps: interacting ? Math.max(6, Math.round(steps / 3)) : steps,
    lights, view,
  });
  placeGizmos(placed, cam, view);
  updateHud();
  if (!force && animating()) requestRender();
}

// Synchronous render, for read-backs that must happen in the same task (GIF, tests, docs).
function renderNow() { frame(performance.now(), true); }

function viewMatrix(t) {
  let { yaw, pitch } = state.orbit;
  if (state.wiggle && state.mode === '3d') {
    const a = (t - animStart) * 0.0012;
    yaw += Math.sin(a) * 0.12;
    pitch += Math.cos(a) * 0.05;
  }
  const c = camera();
  const pivot = depth ? -scene.unproject(0.5, 0.5, c.aspect, c.tanHalfFov, c.near, c.far)[2] : 3;
  return core.orbitView(yaw, pitch, pivot, state.orbit.zoom);
}

function resize() {
  const stage = $('stage');
  const dpr = Math.min(window.devicePixelRatio || 1, QUALITY[state.quality].dpr);
  const w = Math.max(1, Math.round(stage.clientWidth * dpr));
  const h = Math.max(1, Math.round(stage.clientHeight * dpr));
  const c = $('view');
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  requestRender();
}

// ---------------------------------------------------------------------------------------------
// Light gizmos (HTML elements over the canvas)
// ---------------------------------------------------------------------------------------------
function rectCss() {
  const c = $('view');
  const s = c.clientWidth / c.width;
  const r = renderer.fitRect();
  return { x: r.x * s, y: (c.height - r.y - r.h) * s, w: r.w * s, h: r.h * s };
}

function placeGizmos(placed, cam, view) {
  const box = $('gizmos');
  box.classList.toggle('passive', state.mode === '3d');
  const r = rectCss();
  const mvp = state.mode === '3d' ? core.multiply(renderer.projection(cam), view) : null;
  const seen = new Set();
  placed.forEach(({ L, pos, u, v }, i) => {
    let el = box.querySelector(`[data-id="${L.id}"]`);
    if (!el) {
      el = document.createElement('div');
      el.className = 'gizmo';
      el.dataset.id = L.id;
      el.innerHTML = `<span class="n"></span><span class="h"></span>`;
      el.addEventListener('pointerdown', onGizmoDown);
      el.addEventListener('wheel', onGizmoWheel, { passive: false });
      box.appendChild(el);
    }
    seen.add(String(L.id));
    let x, y;
    if (mvp) {
      const p = core.transformPoint(mvp, pos[0], pos[1], pos[2]);      // 3D view uses the whole canvas
      const c = $('view');
      x = (p[0] * 0.5 + 0.5) * c.clientWidth;
      y = (1 - (p[1] * 0.5 + 0.5)) * c.clientHeight;
    } else {
      x = r.x + u * r.w;
      y = r.y + (1 - v) * r.h;
    }
    el.style.left = `${x}px`;
    el.style.top = `${y}px`;
    el.style.setProperty('--c', L.color);
    el.classList.toggle('on', i === state.selected);
    el.querySelector('.n').textContent = i + 1;
    el.querySelector('.h').textContent = `depth ${Math.round(L.depth * 100)}% · scroll`;
  });
  box.querySelectorAll('.gizmo').forEach((el) => { if (!seen.has(el.dataset.id)) el.remove(); });

  const sh = $('splitHandle');
  sh.hidden = state.mode !== 'split';
  if (!sh.hidden) sh.style.left = `${r.x + state.split * r.w}px`;
}

function uvFromEvent(e) {
  const stage = $('stage').getBoundingClientRect();
  const r = rectCss();
  return [(e.clientX - stage.left - r.x) / r.w, 1 - (e.clientY - stage.top - r.y) / r.h];
}

function onGizmoDown(e) {
  if (state.mode === '3d') return;
  e.stopPropagation();
  e.preventDefault();
  const el = e.currentTarget;
  const idx = state.lights.findIndex((L) => String(L.id) === el.dataset.id);
  if (idx < 0) return;
  selectLight(idx);
  el.setPointerCapture(e.pointerId);
  el.classList.add('dragging');
  const move = (ev) => {
    const [u, v] = uvFromEvent(ev);
    state.lights[idx].u = clamp(u, 0.005, 0.995);
    state.lights[idx].v = clamp(v, 0.005, 0.995);
    interact();
  };
  const up = () => {
    el.classList.remove('dragging');
    el.removeEventListener('pointermove', move);
    el.removeEventListener('pointerup', up);
    el.removeEventListener('pointercancel', up);
  };
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
}

function onGizmoWheel(e) {
  e.preventDefault();
  const idx = state.lights.findIndex((L) => String(L.id) === e.currentTarget.dataset.id);
  if (idx < 0) return;
  const L = state.lights[idx];
  L.depth = clamp(L.depth + e.deltaY * 0.0006, 0, 1);
  interact();
  selectLight(idx);
}

// ---------------------------------------------------------------------------------------------
// Stage interaction: add lights, split slider, 3D orbit
// ---------------------------------------------------------------------------------------------
function setupStage() {
  const stage = $('stage');
  stage.addEventListener('dblclick', (e) => {
    if (!renderer?.ready || state.mode === '3d') return;
    const [u, v] = uvFromEvent(e);
    if (u < 0 || u > 1 || v < 0 || v > 1) return;
    addLight(u, v);
  });

  let orbitDrag = null;
  stage.addEventListener('pointerdown', (e) => {
    if (!renderer?.ready || state.mode !== '3d' || e.button !== 0) return;
    // Only a press on the 3D view itself starts an orbit. Capturing the pointer for a press on the
    // mode bar (or any other overlay) would swallow that button's click.
    if (e.target !== $('view')) return;
    orbitDrag = { x: e.clientX, y: e.clientY, yaw: state.orbit.yaw, pitch: state.orbit.pitch };
    stage.setPointerCapture(e.pointerId);
  });
  stage.addEventListener('pointermove', (e) => {
    if (!orbitDrag) return;
    state.orbit.yaw = clamp(orbitDrag.yaw + (e.clientX - orbitDrag.x) * 0.003, -ORBIT.yaw, ORBIT.yaw);
    state.orbit.pitch = clamp(orbitDrag.pitch + (e.clientY - orbitDrag.y) * 0.003, -ORBIT.pitch, ORBIT.pitch);
    requestRender();
  });
  const endOrbit = () => { orbitDrag = null; };
  stage.addEventListener('pointerup', endOrbit);
  stage.addEventListener('pointercancel', endOrbit);
  stage.addEventListener('wheel', (e) => {
    if (state.mode !== '3d' || !renderer?.ready) return;
    e.preventDefault();
    state.orbit.zoom = clamp(state.orbit.zoom * Math.exp(e.deltaY * 0.001), 0.6, 1.6);
    requestRender();
  }, { passive: false });

  const sh = $('splitHandle');
  sh.addEventListener('pointerdown', (e) => {
    e.stopPropagation();
    sh.setPointerCapture(e.pointerId);
    const move = (ev) => { state.split = clamp(uvFromEvent(ev)[0], 0, 1); requestRender(); };
    const up = () => { sh.removeEventListener('pointermove', move); sh.removeEventListener('pointerup', up); };
    sh.addEventListener('pointermove', move);
    sh.addEventListener('pointerup', up);
  });

  document.querySelectorAll('#modes button').forEach((b) =>
    b.addEventListener('click', () => setMode(b.dataset.mode)));

  window.addEventListener('keydown', (e) => {
    if (e.target.matches('input, textarea')) return;
    if ((e.key === 'Delete' || e.key === 'Backspace') && state.lights.length) removeLight(state.selected);
    const modes = ['relit', 'split', '3d', 'depth', 'normals'];
    if (e.key >= '1' && e.key <= '5') setMode(modes[+e.key - 1]);
  });
}

function setMode(mode) {
  state.mode = mode;
  document.querySelectorAll('#modes button').forEach((b) => b.classList.toggle('on', b.dataset.mode === mode));
  const hints = {
    relit: 'Drag a lamp · scroll on it to move it deeper or closer · double-click to add one',
    split: 'Drag the handle to compare the original with the relit photo',
    '3d': 'Drag to orbit · scroll to zoom: this is the 3D surface the AI reconstructed',
    depth: 'AI depth estimate: warm = near, cool = far',
    normals: 'Surface orientation computed from the depth (RGB = XYZ)',
  };
  $('hint').textContent = hints[mode] || '';
  requestRender();
}

// ---------------------------------------------------------------------------------------------
// Lights panel
// ---------------------------------------------------------------------------------------------
function addLight(u = 0.5, v = 0.6) {
  if (state.lights.length >= MAX_LIGHTS) { toast(`Up to ${MAX_LIGHTS} lights.`); return; }
  const k = KELVIN_PRESETS[state.lights.length % KELVIN_PRESETS.length][0];
  state.lights.push(makeLight(u, v, k));
  selectLight(state.lights.length - 1);
}

function removeLight(i) {
  state.lights.splice(i, 1);
  selectLight(Math.max(0, Math.min(state.selected, state.lights.length - 1)));
}

function selectLight(i) {
  state.selected = i;
  renderLightPanel();
  requestRender();
}

function renderLightPanel() {
  const list = $('lightList');
  list.innerHTML = '';
  state.lights.forEach((L, i) => {
    const b = document.createElement('button');
    b.className = 'light-chip' + (i === state.selected ? ' on' : '');
    b.innerHTML = `<span class="dot" style="background:${L.color};color:${L.color}"></span>Light ${i + 1}`;
    b.addEventListener('click', () => selectLight(i));
    list.appendChild(b);
  });
  $('addLight').disabled = state.lights.length >= MAX_LIGHTS || !renderer?.ready;

  const ed = $('lightEditor');
  ed.innerHTML = '';
  const L = state.lights[state.selected];
  if (!L) {
    ed.innerHTML = '<p class="empty-note">No lights: only the room light is on. Double-click the photo to add a lamp.</p>';
    return;
  }
  const row = document.createElement('div');
  row.className = 'row';
  const color = document.createElement('input');
  color.type = 'color';
  color.value = L.color;
  color.title = 'Light colour';
  color.addEventListener('input', () => { L.color = color.value; L.kelvin = null; renderLightChipsOnly(); requestRender(); });
  const kel = document.createElement('div');
  kel.className = 'kelvin';
  for (const [k, name] of KELVIN_PRESETS) {
    const b = document.createElement('button');
    const hex = kelvinHex(k);
    b.innerHTML = `<i style="background:${hex}"></i>${name}`;
    b.title = `${k} K`;
    b.addEventListener('click', () => { L.kelvin = k; L.color = hex; color.value = hex; renderLightChipsOnly(); requestRender(); });
    kel.appendChild(b);
  }
  row.append(color, kel);
  ed.appendChild(row);
  ed.appendChild(slider({ label: 'Intensity', min: 0, max: 6, step: 0.05, get: () => L.intensity, set: (x) => (L.intensity = x) }));
  ed.appendChild(slider({ label: 'Depth in scene (front → back)', min: 0, max: 1, step: 0.01, fmt: (x) => `${Math.round(x * 100)}%`, get: () => L.depth, set: (x) => (L.depth = x) }));
  const rm = document.createElement('button');
  rm.className = 'mini remove';
  rm.textContent = 'Remove light';
  rm.addEventListener('click', () => removeLight(state.selected));
  const r2 = document.createElement('div');
  r2.className = 'row';
  r2.appendChild(rm);
  ed.appendChild(r2);
}

function renderLightChipsOnly() {
  $('lightList').querySelectorAll('.dot').forEach((d, i) => {
    const c = state.lights[i]?.color;
    if (c) { d.style.background = c; d.style.color = c; }
  });
}

// ---------------------------------------------------------------------------------------------
// Generic controls
// ---------------------------------------------------------------------------------------------
const sliderRefreshers = [];

function slider({ label, min, max, step, get, set, fmt, key }) {
  const wrap = document.createElement('div');
  wrap.className = 'ctl';
  const id = 'c' + Math.random().toString(36).slice(2, 8);
  const f = fmt || ((x) => (Math.abs(x) >= 10 ? x.toFixed(0) : x.toFixed(2)));
  wrap.innerHTML = `<label for="${id}">${label}</label><output></output>
    <input id="${id}" type="range" min="${min}" max="${max}" step="${step}">`;
  const input = wrap.querySelector('input'), out = wrap.querySelector('output');
  const refresh = () => { input.value = get(); out.textContent = f(get()); };
  input.addEventListener('input', () => { set(+input.value); out.textContent = f(+input.value); interact(); });
  input.addEventListener('dblclick', () => { if (key && key in DEFAULTS) { set(DEFAULTS[key]); refresh(); requestRender(); } });
  if (key) input.title = 'Double-click to reset';
  refresh();
  if (key) sliderRefreshers.push(refresh);
  return wrap;
}

function stateSlider(key, label, min, max, step, fmt) {
  return slider({ label, min, max, step, fmt, key, get: () => state[key], set: (x) => (state[key] = x) });
}

function segmented(label, options, get, set) {
  const frag = document.createDocumentFragment();
  const l = document.createElement('div');
  l.className = 'seg-label';
  l.textContent = label;
  const seg = document.createElement('div');
  seg.className = 'seg';
  for (const [value, text] of options) {
    const b = document.createElement('button');
    b.textContent = text;
    b.classList.toggle('on', get() === value);
    b.addEventListener('click', () => {
      set(value);
      seg.querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
    });
    seg.appendChild(b);
  }
  frag.append(l, seg);
  return frag;
}

function toggle(label, get, set) {
  const l = document.createElement('label');
  l.className = 'toggle';
  l.innerHTML = `<span>${label}</span><input type="checkbox">`;
  const input = l.querySelector('input');
  input.checked = get();
  input.addEventListener('change', () => { set(input.checked); requestRender(); });
  return l;
}

function buildControls() {
  const pct = (x) => `${Math.round(x * 100)}%`;
  $('roomControls').append(
    stateSlider('ambient', 'Room light', 0, 1.2, 0.01, pct),
    stateSlider('flatten', 'Remove original lighting', 0, 1, 0.01, pct),
    stateSlider('haze', 'Haze / glow', 0, 1, 0.01, pct),
    stateSlider('exposure', 'Exposure', 0.2, 3, 0.01),
  );
  $('surfaceControls').append(
    stateSlider('shadow', 'Shadow strength', 0, 1, 0.01, pct),
    stateSlider('soft', 'Shadow softness', 0.003, 0.08, 0.001),
    stateSlider('normalStrength', 'Surface detail', 0, 1, 0.01, pct),
    stateSlider('spec', 'Shine', 0, 1, 0.01, pct),
    stateSlider('gloss', 'Shine sharpness', 4, 128, 1),
    stateSlider('wrap', 'Light wrap', 0, 1, 0.01, pct),
  );
  $('depthControls').append(
    stateSlider('depthRange', 'Depth range (far / near)', 1.5, 20, 0.1, (x) => `${x.toFixed(1)}×`),
    stateSlider('fov', 'Camera field of view', 25, 100, 1, (x) => `${x}°`),
  );
  const cut = stateSlider('cut', '3D edge cut (disparity jump)', 0.01, 0.2, 0.005);
  cut.querySelector('input').addEventListener('change', rebuildMesh);   // C++ rebuild on release
  $('depthControls').append(cut);
  $('perfControls').append(
    segmented('Quality', [['low', 'Low'], ['medium', 'Medium'], ['high', 'High']], () => state.quality, (q) => {
      state.quality = q; resize();
    }),
    segmented('AI runs on', [['auto', 'GPU if possible'], ['wasm', 'CPU only']], () => state.aiDevice, (d) => {
      if (d === state.aiDevice) return;
      state.aiDevice = d;
      worker?.terminate(); worker = null; workerReady = null;    // reload the model on next photo
      toast(d === 'wasm' ? 'The next photo will be analysed on the CPU.' : 'The next photo will use WebGPU when available.', 'info');
    }),
    toggle('Animate lights (30 fps)', () => state.animate, (x) => { state.animate = x; animStart = performance.now(); }),
    toggle('Wiggle in 3D view (30 fps)', () => state.wiggle, (x) => { state.wiggle = x; }),
  );
  $('addLight').addEventListener('click', () => addLight());
}

// ---------------------------------------------------------------------------------------------
// HUD, toasts, busy overlay
// ---------------------------------------------------------------------------------------------
function updateHud() {
  const hud = $('hud');
  if (!renderer?.ready) { hud.hidden = true; return; }
  hud.hidden = false;
  const t = renderer.layers.photo.target;
  const gpu = renderer.gpuMs != null ? `${renderer.gpuMs.toFixed(1)} ms` : 'n/a';
  const ai = aiInfo.device ? `${aiInfo.device === 'webgpu' ? 'WebGPU' : 'CPU'}${aiInfo.ms ? ` · ${Math.round(aiInfo.ms)} ms` : ''}` : 'cached';
  hud.innerHTML =
    `light pass <b>${gpu}</b> · ${t.width}×${t.height}<br>` +
    `AI depth <b>${ai}</b><br>` +
    `${animating() ? '<b>animating</b> 30 fps cap' : 'idle · <b>0 fps</b>'} · frames ${renderer.frames}`;
}

let toastTimer;
function toast(msg, kind = 'error') {
  const el = $('toast');
  el.textContent = msg;
  el.style.background = kind === 'info' ? '#15202a' : '';
  el.style.color = kind === 'info' ? '#d6ecff' : '';
  el.style.borderColor = kind === 'info' ? '#2a465a' : '';
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (el.hidden = true), kind === 'info' ? 3000 : 7000);
}

function busy(on, text = '', sub = '', progress = null) {
  $('busy').hidden = !on;
  $('busyText').textContent = text;
  $('busySub').textContent = sub;
  $('busyBar').parentElement.style.visibility = progress == null ? 'hidden' : 'visible';
  if (progress != null) $('busyBar').style.width = `${Math.round(progress * 100)}%`;
}

// ---------------------------------------------------------------------------------------------
// AI depth (web worker)
// ---------------------------------------------------------------------------------------------
function ensureWorker() {
  if (worker) return workerReady;
  worker = new Worker(new URL('./depth-worker.js', import.meta.url), { type: 'module' });
  const files = new Map();
  workerReady = new Promise((resolve, reject) => {
    worker.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'progress') {
        files.set(m.file, m);
        let loaded = 0, total = 0;
        for (const f of files.values()) { loaded += f.loaded; total += f.total; }
        busy(true, 'Downloading the AI model (once)',
          `${(loaded / 1e6).toFixed(1)} / ${(total / 1e6).toFixed(1)} MB · cached by your browser afterwards`, loaded / total);
      } else if (m.type === 'ready') {
        aiInfo.device = m.device;
        resolve(m.device);
      } else if (m.type === 'depth' || m.type === 'error') {
        const job = pendingJobs.get(m.id);
        pendingJobs.delete(m.id);
        if (!job) return;
        if (m.type === 'error') job.reject(new Error(m.message));
        else job.resolve(m);
      }
    };
    worker.onerror = (e) => reject(new Error(e.message || 'AI worker failed to start'));
  });
  workerReady.catch(() => {});
  worker.postMessage({ type: 'load', preference: state.aiDevice });
  return workerReady;
}

async function estimateDepth(canvas) {
  const s = Math.min(1, AI_MAX_SIDE / Math.max(canvas.width, canvas.height));
  const w = Math.round(canvas.width * s), h = Math.round(canvas.height * s);
  const small = document.createElement('canvas');
  small.width = w; small.height = h;
  const ctx = small.getContext('2d');
  ctx.drawImage(canvas, 0, 0, w, h);
  const rgba = ctx.getImageData(0, 0, w, h).data;
  busy(true, 'Loading the AI model…', '', null);
  await ensureWorker();
  busy(true, 'Reconstructing 3D from your photo…',
    aiInfo.device === 'webgpu' ? 'Depth Anything V2 on your GPU (WebGPU)' : 'Depth Anything V2 on your CPU (WebAssembly)', null);
  const id = ++jobId;
  const result = await new Promise((resolve, reject) => {
    pendingJobs.set(id, { resolve, reject });
    worker.postMessage({ type: 'estimate', id, width: w, height: h, rgba, preference: state.aiDevice }, [rgba.buffer]);
  });
  aiInfo = { device: result.device, ms: result.ms };
  // Robust normalization in C++ (radius 0: no filtering yet; that happens in loadDepth).
  const normalized = scene.setRawDepth(result.data, result.width, result.height, 0, FILTER_EPS);
  return { normalized, width: result.width, height: result.height };
}

// Normalized depth -> C++ processing -> GPU: guided filter, background layer, meshes.
function loadDepth(canvas, d) {
  const { width: w, height: h } = d;
  const filtered = scene.setDepth(d.normalized, w, h, filterRadius(w, h), FILTER_EPS, SHARPEN.radius, SHARPEN.threshold);
  // Colour at depth resolution: input for the illumination estimate and the background layer.
  const small = document.createElement('canvas');
  small.width = w; small.height = h;
  const ctx = small.getContext('2d', { willReadFrequently: true });
  ctx.drawImage(canvas, 0, 0, w, h);
  const rgba = new Uint8Array(ctx.getImageData(0, 0, w, h).data.buffer);
  const sr = shadingRadius(w, h);
  const shading = core.estimateShading(rgba, w, h, sr, SHADING_EPS);
  renderer.setPhoto(canvas, filtered, w, h, { data: shading.shading, mean: shading.mean });
  const bg = scene.buildBackground(rgba, state.cut, bandRadius(w, h));
  const bgShading = core.estimateShading(bg.color, w, h, sr, SHADING_EPS);
  renderer.setBackground(bg.color, bg.depth, w, h, { data: bgShading.shading, mean: bgShading.mean });
  rebuildMesh();
}

function rebuildMesh() {
  if (!depth) return;
  const photoMesh = scene.buildMesh(MESH_SIDE, state.cut);
  const full = scene.buildMesh(MESH_SIDE, 0);
  renderer.setMesh(photoMesh.uv, photoMesh.indices, full.indices);
  requestRender();
}

// ---------------------------------------------------------------------------------------------
// Loading photos
// ---------------------------------------------------------------------------------------------
function toWorkingCanvas(source, sw, sh) {
  const maxSide = QUALITY[state.quality].maxSide;
  const s = Math.min(1, maxSide / Math.max(sw, sh));
  const c = document.createElement('canvas');
  c.width = Math.round(sw * s);
  c.height = Math.round(sh * s);
  c.getContext('2d').drawImage(source, 0, 0, c.width, c.height);
  return c;
}

async function openPhoto(canvas, name, cachedDepth = null, preset = null) {
  $('empty').hidden = true;
  try {
    const d = cachedDepth || (await estimateDepth(canvas));
    if (cachedDepth) aiInfo = { device: null, ms: null };
    Object.assign(state, DEFAULTS, preset?.settings || {});
    photo = { canvas, name };
    depth = d;
    loadDepth(canvas, d);
    sliderRefreshers.forEach((f) => f());
    state.lights = preset?.lights ? preset.lights.map((l) => Object.assign(makeLight(l.u, l.v, l.kelvin ?? 2700, l.intensity, l.depth), l.color ? { color: l.color } : {})) : defaultLights();
    state.selected = 0;
    state.orbit = { yaw: 0, pitch: 0, zoom: 1 };
    busy(false);
    $('modes').hidden = false;
    $('hint').hidden = false;
    $('saveBtn').disabled = false;
    $('gifBtn').disabled = false;
    setMode(state.mode);
    renderLightPanel();
    resize();
    return d;
  } catch (err) {
    console.error(err);
    busy(false);
    if (!photo) $('empty').hidden = false;
    toast(`Could not analyse the photo: ${err.message}`);
    throw err;
  }
}

async function openFile(file) {
  if (!file || !file.type.startsWith('image/')) { toast('Please choose an image file.'); return; }
  busy(true, 'Opening photo…');
  let bmp;
  try {
    bmp = await createImageBitmap(file, { imageOrientation: 'from-image' });
  } catch {
    busy(false);
    toast('This image format cannot be decoded by the browser.');
    return;
  }
  const canvas = toWorkingCanvas(bmp, bmp.width, bmp.height);
  bmp.close();
  await openPhoto(canvas, file.name).catch(() => {});
}

async function loadImage(url) {
  const img = new Image();
  img.decoding = 'async';
  img.src = url;
  await img.decode();
  return img;
}

async function openSample(s) {
  busy(true, `Opening “${s.title}”…`);
  try {
    const img = await loadImage(`samples/${s.image}`);
    const canvas = toWorkingCanvas(img, img.naturalWidth, img.naturalHeight);
    let cached = null;
    if (s.depth) {
      const r = await fetch(`samples/${s.depth}`);
      if (r.ok) cached = core.decodeDepth16(new Uint8Array(await r.arrayBuffer()));
    }
    await openPhoto(canvas, s.id, cached, s);
  } catch (err) {
    busy(false);
    toast(`Could not open the example: ${err.message}`);
  }
}

async function loadSamples() {
  try {
    const r = await fetch('samples/manifest.json', { cache: 'no-cache' });
    if (!r.ok) return;
    samples = (await r.json()).samples || [];
  } catch { samples = []; }
  const box = $('samples');
  box.innerHTML = '';
  for (const s of samples) {
    const b = document.createElement('button');
    b.className = 'sample';
    b.innerHTML = `<img alt="" loading="lazy" src="samples/${s.thumb || s.image}"><span>${s.title}</span>`;
    b.addEventListener('click', () => openSample(s));
    box.appendChild(b);
  }
  $('samplesBtn').hidden = samples.length === 0;
}

// ---------------------------------------------------------------------------------------------
// Camera capture
// ---------------------------------------------------------------------------------------------
let camStream = null;
async function startCamera() {
  try {
    camStream = await navigator.mediaDevices.getUserMedia({ video: { width: { ideal: 1280 }, height: { ideal: 720 } } });
  } catch (err) {
    toast(`Camera not available: ${err.message}`);
    return;
  }
  const v = $('camVideo');
  v.srcObject = camStream;
  await v.play();
  $('camera').hidden = false;
}
function stopCamera() {
  camStream?.getTracks().forEach((t) => t.stop());
  camStream = null;
  $('camera').hidden = true;
}
async function shootCamera() {
  const v = $('camVideo');
  const canvas = toWorkingCanvas(v, v.videoWidth, v.videoHeight);
  stopCamera();
  await openPhoto(canvas, 'camera').catch(() => {});
}

// ---------------------------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------------------------
function download(blob, filename) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}

const baseName = () => (photo?.name || 'photo').replace(/\.[^.]+$/, '');

function saveImage() {
  if (!renderer?.ready) return;
  settle();
  const img = renderer.readRelit();
  const c = document.createElement('canvas');
  c.width = img.width; c.height = img.height;
  c.getContext('2d').putImageData(img, 0, 0);
  c.toBlob((blob) => download(blob, `relight-${baseName()}.png`), 'image/png');
}

// Leave preview quality and draw one full-quality frame right now.
function settle() {
  interacting = false;
  clearTimeout(settleTimer);
  renderNow();
}

// "Save animation": the selected lamp circles its position (or, in the 3D view, the camera
// sways), each frame is read back from the GPU, and the C++ core encodes the GIF.
let recording = false;
async function recordGif({ frames = 36, maxSide = 560, delayCs = 7, radius = [0.22, 0.2], center = null } = {}) {
  if (!renderer?.ready || recording) return null;
  recording = true;
  const L = state.lights[state.selected] ?? state.lights[0];
  const saved = { orbit: { ...state.orbit }, u: L?.u, v: L?.v };
  const c0 = center || [L?.u ?? 0.5, L?.v ?? 0.5];
  const tmp = document.createElement('canvas');
  const src = document.createElement('canvas');
  let enc = null;
  try {
    for (let i = 0; i < frames; i++) {
      const a = (2 * Math.PI * i) / frames;
      if (state.mode === '3d') {
        state.orbit.yaw = Math.sin(a) * ORBIT.yaw * 0.8;
        state.orbit.pitch = Math.cos(a) * ORBIT.pitch * 0.4;
      } else if (L) {
        L.u = clamp(c0[0] + radius[0] * Math.cos(a), 0.03, 0.97);
        L.v = clamp(c0[1] + radius[1] * Math.sin(a), 0.03, 0.97);
      }
      settle();                                   // render and read back in the same task
      const r = state.mode === '3d' ? { x: 0, y: 0, w: $('view').width, h: $('view').height } : renderer.fitRect();
      const img = renderer.readScreen(r);
      if (!enc) {
        const s = Math.min(1, maxSide / Math.max(r.w, r.h));
        tmp.width = Math.round(r.w * s);
        tmp.height = Math.round(r.h * s);
        enc = new core.GifEncoder(tmp.width, tmp.height, delayCs);
      }
      src.width = r.w; src.height = r.h;
      src.getContext('2d').putImageData(img, 0, 0);
      const tctx = tmp.getContext('2d', { willReadFrequently: true });
      tctx.drawImage(src, 0, 0, tmp.width, tmp.height);
      enc.addFrame(new Uint8Array(tctx.getImageData(0, 0, tmp.width, tmp.height).data.buffer));
      busy(true, 'Recording animation…', `frame ${i + 1} / ${frames} · encoded in C++ (WebAssembly)`, (i + 1) / frames);
      await new Promise((res) => setTimeout(res, 0));   // keep the page responsive
    }
    return new Blob([enc.finish()], { type: 'image/gif' });
  } finally {
    enc?.delete();
    state.orbit = saved.orbit;
    if (L) { L.u = saved.u; L.v = saved.v; }
    recording = false;
    busy(false);
    requestRender();
  }
}

async function saveAnimation() {
  try {
    const blob = await recordGif();
    if (blob) download(blob, `relight-${baseName()}.gif`);
  } catch (err) {
    toast(`Could not record the animation: ${err.message}`);
  }
}

// ---------------------------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------------------------
function setupFileInputs() {
  for (const id of ['fileInput', 'fileInput2'])
    $(id).addEventListener('change', (e) => { openFile(e.target.files[0]); e.target.value = ''; });
  $('camBtn').addEventListener('click', startCamera);
  $('camBtn2').addEventListener('click', startCamera);
  $('camShoot').addEventListener('click', shootCamera);
  $('camCancel').addEventListener('click', stopCamera);
  $('saveBtn').addEventListener('click', saveImage);
  $('gifBtn').addEventListener('click', saveAnimation);
  $('samplesBtn').addEventListener('click', () => { if (photo) $('empty').hidden = !$('empty').hidden; });
  $('panelBtn').addEventListener('click', () => document.body.classList.toggle('panel-open'));

  const empty = $('empty');
  window.addEventListener('dragover', (e) => { e.preventDefault(); empty.classList.add('dragover'); });
  window.addEventListener('dragleave', () => empty.classList.remove('dragover'));
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    empty.classList.remove('dragover');
    openFile(e.dataTransfer.files[0]);
  });
}

async function boot() {
  try {
    renderer = new Renderer($('view'));
  } catch (err) {
    $('empty').querySelector('p').textContent = `This browser cannot run the demo: ${err.message}`;
    return;
  }
  renderer.onTiming = () => updateHud();
  $('view').addEventListener('webglcontextlost', (e) => {
    e.preventDefault();
    toast('The graphics driver reset the GPU context. Please reload the page.');
  });
  buildControls();
  renderLightPanel();
  setupStage();
  setupFileInputs();
  new ResizeObserver(resize).observe($('stage'));
  document.addEventListener('visibilitychange', () => { if (!document.hidden) requestRender(); });
  await loadSamples();

  // Hooks for the tests and the developer tools (js/dev.js).
  window.relight = {
    state, renderer, scene, core, openSample, requestRender, renderNow, interact, recordGif, estimateDepth,
    loadImage, toWorkingCanvas, setMode, get samples() { return samples; },
  };
  if (params.has('bake') || params.has('docs')) {
    const dev = await import('./dev.js');
    await dev.run(params);
    return;
  }
  const want = params.get('sample');
  const s = samples.find((x) => x.id === want);
  if (s) await openSample(s);
  if (params.has('light')) {     // e.g. ?light=0.3,0.6 for reproducible screenshots
    const [u, v] = params.get('light').split(',').map(Number);
    if (state.lights[0]) { state.lights[0].u = u; state.lights[0].v = v; requestRender(); }
  }
  document.documentElement.dataset.ready = '1';
}

boot();
