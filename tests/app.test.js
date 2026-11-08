// App-level checks on the real page, loaded in an iframe (same origin, so the test can reach
// window.relight inside it).
import { test, assert } from './harness.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let frame, app, errors = [];

async function waitFor(cond, ms = 60000) {
  const end = performance.now() + ms;
  while (!cond()) {
    if (performance.now() > end) throw new Error('timed out');
    await sleep(50);
  }
}

test('app: loads an example', async () => {
  frame = document.createElement('iframe');
  frame.style.cssText = 'width:1280px;height:800px;border:0';
  frame.src = '../index.html?sample=statue';
  document.body.appendChild(frame);
  await waitFor(() => frame.contentDocument?.documentElement.dataset.ready === '1');
  app = frame.contentWindow.relight;
  const w = frame.contentWindow;
  w.addEventListener('error', (e) => errors.push(String(e.message)));
  const orig = w.console.error.bind(w.console);
  w.console.error = (...a) => { errors.push(a.join(' ')); orig(...a); };
  assert(app.renderer.ready, 'renderer not ready');
});

test('app: an idle page draws no frames (GPU stays idle)', async () => {
  await sleep(500);
  const f0 = app.renderer.frames;
  await sleep(1500);
  assert(app.renderer.frames === f0, `${app.renderer.frames - f0} frames while idle`);
});

test('app: orbiting the 3D view does not re-run the light pass', async () => {
  app.setMode('3d');
  await sleep(200);                                   // first 3D frame relights the background layer
  const p0 = app.renderer.relightPasses;
  app.state.orbit.yaw = 0.2;
  app.requestRender();
  await sleep(200);
  assert(app.renderer.relightPasses === p0, `${app.renderer.relightPasses - p0} extra light passes`);
});

test('app: moving a lamp re-runs the light pass once (flat view)', async () => {
  app.setMode('relit');
  await sleep(150);
  const p0 = app.renderer.relightPasses;
  app.state.lights[0].u += 0.05;
  app.requestRender();
  await sleep(150);
  assert(app.renderer.relightPasses === p0 + 1, `${app.renderer.relightPasses - p0} passes`);
});

test('app: every example opens from its cached depth', async () => {
  for (const s of app.samples) {
    await app.openSample(s);
    assert(app.renderer.ready && app.renderer.layers.background, `${s.id} did not open`);
  }
});

test('app: "Save animation" produces a GIF (C++ encoder)', async () => {
  const blob = await app.recordGif({ frames: 3, maxSide: 120 });
  const head = new Uint8Array(await blob.slice(0, 6).arrayBuffer());
  assert(String.fromCharCode(...head) === 'GIF89a', 'not a GIF');
});

test('app: no errors in the console', async () => {
  assert(errors.length === 0, errors.join('\n'));
});
