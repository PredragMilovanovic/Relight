// Developer tools, loaded only with ?bake or ?docs. Driven headless by scripts/bake.ps1 and
// scripts/docs_images.ps1; results go to scripts/serve.ps1 (POST /save), which only accepts
// sample depth files, docs/images and .cache reports.

const R = () => window.relight;

async function save(path, body) {
  const r = await fetch(`/save?path=${encodeURIComponent(path)}`, { method: 'POST', body });
  if (!r.ok) throw new Error(`saving ${path} failed: HTTP ${r.status}`);
}

export async function run(params) {
  const name = params.has('bake') ? 'bake-report' : 'docs-report';
  let report;
  try {
    report = { ok: true, lines: params.has('bake') ? await bake() : await docs() };
  } catch (err) {
    console.error(err);
    report = { ok: false, lines: [String(err?.stack || err)] };
  }
  await save(`.cache/${name}.json`, JSON.stringify(report));
}

// AI depth for every example -> samples/<id>.depth.bin (normalized, before filtering).
async function bake() {
  const { samples, loadImage, toWorkingCanvas, estimateDepth, core } = R();
  const lines = [];
  for (const s of samples) {
    const img = await loadImage(`samples/${s.image}`);
    const canvas = toWorkingCanvas(img, img.naturalWidth, img.naturalHeight);
    const t0 = performance.now();
    const d = await estimateDepth(canvas);
    await save(`samples/${s.id}.depth.bin`, core.encodeDepth16(d.normalized, d.width, d.height));
    lines.push(`${s.id}: ${d.width}x${d.height}, ${Math.round(performance.now() - t0)} ms`);
  }
  return lines;
}

// --- documentation images ----------------------------------------------------------------------
function toCanvas(img) {
  const c = document.createElement('canvas');
  c.width = img.width; c.height = img.height;
  c.getContext('2d').putImageData(img, 0, 0);
  return c;
}

const toBlob = (canvas, type = 'image/jpeg') => new Promise((res) => canvas.toBlob(res, type, 0.9));

async function open(id, mode, setup = () => {}) {
  const r = R();
  await r.openSample(r.samples.find((s) => s.id === id));
  r.setMode(mode);
  setup(r.state);
}

// Render now and read back what is on screen: the photo rectangle, or the whole canvas in 3D.
function grab() {
  const r = R();
  r.renderNow();
  const whole = r.state.mode === '3d';
  return toCanvas(r.renderer.readScreen(whole ? undefined : r.renderer.fitRect()));
}

async function docs() {
  const out = [];
  const shot = async (file, canvas) => {
    await save(`docs/images/${file}`, await toBlob(canvas));
    out.push(`${file}: ${canvas.width}x${canvas.height}`);
  };

  await open('bedroom', 'split', (s) => { s.split = 0.42; });
  await shot('bedroom_split.jpg', grab());

  await open('livingroom', 'relit');
  await shot('livingroom_relit.jpg', grab());

  await open('livingroom', 'depth');
  const depth = grab();
  R().setMode('normals');
  const normals = grab();
  const pair = document.createElement('canvas');
  pair.width = depth.width * 2 + 12; pair.height = depth.height;
  const ctx = pair.getContext('2d');
  ctx.fillStyle = '#0b0c0f';
  ctx.fillRect(0, 0, pair.width, pair.height);
  ctx.drawImage(depth, 0, 0);
  ctx.drawImage(normals, depth.width + 12, 0);
  await shot('livingroom_depth_normals.jpg', pair);

  await open('statue', 'normals');
  await shot('statue_normals.jpg', grab());

  await open('statue', '3d', (s) => { s.orbit = { yaw: 0.2, pitch: 0.08, zoom: 1 }; });
  await shot('statue_3d.jpg', grab());

  await open('livingroom', '3d', (s) => { s.orbit = { yaw: -0.2, pitch: 0.1, zoom: 1 }; });
  await shot('livingroom_3d.jpg', grab());

  await open('stilllife', 'relit');
  await shot('stilllife_relit.jpg', grab());

  // README animation: the warm lamp circles the statue. Encoded by the C++ GIF encoder.
  await open('statue', 'relit', (s) => { s.selected = 0; });
  const gif = await R().recordGif({ frames: 36, maxSide: 520, delayCs: 7, radius: [0.33, 0.3], center: [0.5, 0.55] });
  await save('docs/images/statue_orbit.gif', gif);
  out.push(`statue_orbit.gif: ${Math.round(gif.size / 1024)} KB`);
  return out;
}
