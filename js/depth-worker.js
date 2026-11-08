// Runs the depth network off the main thread so the page never freezes.
// Model: Depth Anything V2 Small (Apache-2.0), ONNX export, via transformers.js / ONNX Runtime Web.
//   WebGPU + fp16 weights (~50 MB) when the GPU supports shader-f16, otherwise WASM on the CPU
//   with 8-bit weights (~27 MB). Weights are cached by the browser after the first download.
import { pipeline, env, RawImage } from 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1';

env.allowLocalModels = false;
const MODEL = 'onnx-community/depth-anything-v2-small';
let estimator = null;
let device = null;

async function pickDevice(preference) {
  if (preference === 'wasm') return 'wasm';
  try {
    const adapter = await self.navigator.gpu?.requestAdapter();
    if (adapter && adapter.features.has('shader-f16')) return 'webgpu';
  } catch { /* no WebGPU */ }
  return 'wasm';
}

async function load(preference) {
  device = await pickDevice(preference);
  const progress = (p) => {
    if (p.status === 'progress' && p.total)
      postMessage({ type: 'progress', file: p.file, loaded: p.loaded, total: p.total });
  };
  try {
    estimator = await pipeline('depth-estimation', MODEL, {
      device, dtype: device === 'webgpu' ? 'fp16' : 'q8', progress_callback: progress,
    });
  } catch (err) {
    if (device !== 'webgpu') throw err;
    device = 'wasm';            // WebGPU failed at runtime: fall back to the CPU
    estimator = await pipeline('depth-estimation', MODEL, { device, dtype: 'q8', progress_callback: progress });
  }
  postMessage({ type: 'ready', device });
}

self.onmessage = async (e) => {
  const msg = e.data;
  try {
    if (msg.type === 'load') {
      if (!estimator) await load(msg.preference);
      else postMessage({ type: 'ready', device });
    } else if (msg.type === 'estimate') {
      if (!estimator) await load(msg.preference);
      const { width, height, rgba } = msg;
      const rgb = new Uint8ClampedArray(width * height * 3);
      for (let i = 0, j = 0; i < rgba.length; i += 4, j += 3) {
        rgb[j] = rgba[i]; rgb[j + 1] = rgba[i + 1]; rgb[j + 2] = rgba[i + 2];
      }
      const t0 = performance.now();
      const out = await estimator(new RawImage(rgb, width, height, 3));
      const ms = performance.now() - t0;
      const t = out.predicted_depth;
      const [h, w] = t.dims;
      const data = Float32Array.from(t.data);
      postMessage({ type: 'depth', id: msg.id, width: w, height: h, data, ms, device }, [data.buffer]);
    }
  } catch (err) {
    postMessage({ type: 'error', id: msg.id, message: String(err?.message || err) });
  }
};
