// WebGL2 renderer.
//   pass 1  relight   photo + depth -> RGBA8 texture at the photo's resolution (expensive)
//           In the 3D view the inpainted background layer is relit the same way.
//   pass 2  display   flat (with before/after split) or the layered 3D relief
// Pass 1 is cached per layer: it re-runs only when a lighting uniform changes, so orbiting the
// 3D view or dragging the split line costs one cheap textured draw. While the user drags, pass 1
// can render into a part of its texture (`scale` < 1) for a 4x cheaper preview.
import { FULLSCREEN_VS, RELIGHT_FS, DISPLAY_FS, MESH_VS, MESH_FS } from './shaders.js';
import { core } from './core.js';

function compile(gl, type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(s);
    gl.deleteShader(s);
    throw new Error('Shader compile error: ' + log);
  }
  return s;
}

function program(gl, vs, fs) {
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, vs));
  gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error('Link error: ' + gl.getProgramInfoLog(p));
  const uniforms = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) {
    const info = gl.getActiveUniform(p, i);
    uniforms[info.name.replace(/\[0\]$/, '')] = gl.getUniformLocation(p, info.name);
  }
  return { p, u: uniforms };
}

// Rows of a top-down image -> bottom-up (GL texture row 0 is the bottom).
function flipRows(data, width, height, channels) {
  const out = new data.constructor(data.length);
  const row = width * channels;
  for (let y = 0; y < height; y++) out.set(data.subarray((height - 1 - y) * row, (height - y) * row), y * row);
  return out;
}

export class Renderer {
  constructor(canvas) {
    this.canvas = canvas;
    const gl = canvas.getContext('webgl2', {
      antialias: false, alpha: false, depth: true, preserveDrawingBuffer: false, powerPreference: 'default',
    });
    if (!gl) throw new Error('WebGL2 is not available in this browser.');
    this.gl = gl;
    this.relightProg = program(gl, FULLSCREEN_VS, RELIGHT_FS);
    this.displayProg = program(gl, FULLSCREEN_VS, DISPLAY_FS);
    this.meshProg = program(gl, MESH_VS, MESH_FS);
    this.emptyVao = gl.createVertexArray();
    this.timer = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    this.floatLinear = !!gl.getExtension('OES_texture_float_linear');
    this.gpuMs = null;
    this.frames = 0;            // frames drawn (the idle test checks this stays constant)
    this.relightPasses = 0;     // light passes run, all layers
    this.layers = { photo: null, background: null };
    this.mesh = null;
  }

  get ready() { return !!(this.layers.photo?.image && this.layers.photo?.depth); }

  // --- resources -------------------------------------------------------------------------------
  _texture(setup) {
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    setup(gl);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return tex;
  }

  _colorTexture(width, height, pixels /* canvas | Uint8Array RGBA top-down */) {
    const gl = this.gl;
    return this._texture(() => {
      if (pixels instanceof Uint8Array) {
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.SRGB8_ALPHA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE,
          flipRows(pixels, width, height, 4));
      } else {
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.SRGB8_ALPHA8, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      }
      gl.generateMipmap(gl.TEXTURE_2D);   // the mip chain feeds the albedo estimate
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    });
  }

  // Half floats have an 11-bit mantissa: near d = 1 the step is 1/2048, which shows up as
  // terraces in the normals. Use 32-bit floats whenever the GPU can filter them.
  _depthTexture(data, width, height) {
    const gl = this.gl;
    return this._texture(() => {
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texImage2D(gl.TEXTURE_2D, 0, this.floatLinear ? gl.R32F : gl.R16F, width, height, 0, gl.RED, gl.FLOAT,
        flipRows(data, width, height, 1));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    });
  }

  _target(width, height) {
    const gl = this.gl;
    const tex = this._texture(() => {
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    });
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { tex, fbo, width, height };
  }

  _dropLayer(name) {
    const gl = this.gl, L = this.layers[name];
    if (!L) return;
    for (const t of [L.image, L.depth, L.shading, L.target?.tex]) if (t) gl.deleteTexture(t);
    if (L.target) gl.deleteFramebuffer(L.target.fbo);
    this.layers[name] = null;
  }

  // The photo (canvas at the working resolution), its processed depth and its illumination
  // estimate ({ data, mean }, both at depth resolution, row 0 = top), all from the C++ core.
  setPhoto(canvas, depth, dw, dh, shading) {
    this._dropLayer('photo');
    this._dropLayer('background');
    this.layers.photo = {
      image: this._colorTexture(canvas.width, canvas.height, canvas),
      depth: this._depthTexture(depth, dw, dh), dw, dh,
      shading: this._depthTexture(shading.data, dw, dh), shadingMean: shading.mean,
      target: this._target(canvas.width, canvas.height), key: '',
    };
  }

  // Inpainted background layer from the C++ core, at depth resolution.
  setBackground(rgba, depth, width, height, shading) {
    this._dropLayer('background');
    this.layers.background = {
      image: this._colorTexture(width, height, rgba),
      depth: this._depthTexture(depth, width, height), dw: width, dh: height,
      shading: this._depthTexture(shading.data, width, height), shadingMean: shading.mean,
      target: this._target(width, height), key: '',
    };
  }

  // Grid mesh from the C++ core: shared (u,v) vertices, two index lists.
  setMesh(uv, photoIndices, backgroundIndices) {
    const gl = this.gl;
    if (this.mesh) {
      gl.deleteVertexArray(this.mesh.vao);
      for (const b of [this.mesh.vbo, this.mesh.photo, this.mesh.background]) gl.deleteBuffer(b);
    }
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, uv, gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(this.meshProg.p, 'aUv');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);
    const ibo = (data) => {
      const b = gl.createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, b);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, data, gl.STATIC_DRAW);
      return b;
    };
    this.mesh = {
      vao, vbo, photo: ibo(photoIndices), photoCount: photoIndices.length,
      background: ibo(backgroundIndices), backgroundCount: backgroundIndices.length,
    };
  }

  // --- geometry helpers ---------------------------------------------------------------------
  // Letterboxed rectangle (device pixels, GL origin bottom-left) where the photo is shown.
  fitRect() {
    const W = this.canvas.width, H = this.canvas.height;
    const t = this.layers.photo?.target;
    if (!t) return { x: 0, y: 0, w: W, h: H };
    const a = t.width / t.height;
    let w = W, h = W / a;
    if (h > H) { h = H; w = H * a; }
    return { x: Math.round((W - w) / 2), y: Math.round((H - h) / 2), w: Math.round(w), h: Math.round(h) };
  }

  // Projection for the 3D view over the WHOLE canvas, chosen so that at the start pose the photo
  // covers exactly its letterboxed rectangle: widen the vertical field by canvas / photo height.
  projection(cam) {
    const r = this.fitRect();
    const tanY = cam.tanHalfFov * (this.canvas.height / r.h);
    return core.perspective(tanY, this.canvas.width / this.canvas.height, cam.near * 0.05, cam.far * 4);
  }

  // --- rendering ----------------------------------------------------------------------------
  _setCamera(u, cam) {
    const gl = this.gl;
    gl.uniform1f(u.uAspect, cam.aspect);
    gl.uniform1f(u.uTanF, cam.tanHalfFov);
    gl.uniform1f(u.uNear, cam.near);
    gl.uniform1f(u.uFar, cam.far);
  }

  _relight(L, p, modeId) {
    const key = JSON.stringify([p.cam, modeId, p.scale, p.ambient, p.flatten, p.exposure, p.normalStrength,
      p.wrap, p.spec, p.gloss, p.haze, p.shadow, p.soft, p.steps, p.lights]);
    if (key === L.key) return;
    L.key = key;
    this.relightPasses++;
    const gl = this.gl, { p: prog, u } = this.relightProg;
    gl.bindFramebuffer(gl.FRAMEBUFFER, L.target.fbo);
    gl.viewport(0, 0, Math.max(1, Math.round(L.target.width * p.scale)), Math.max(1, Math.round(L.target.height * p.scale)));
    gl.disable(gl.DEPTH_TEST);
    gl.useProgram(prog);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, L.image);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, L.depth);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, L.shading);
    gl.uniform1i(u.uImage, 0);
    gl.uniform1i(u.uDepth, 1);
    gl.uniform1i(u.uShading, 3);
    gl.uniform1f(u.uShadingMean, L.shadingMean);
    this._setCamera(u, p.cam);
    gl.uniform2f(u.uTexel, 1 / L.dw, 1 / L.dh);
    gl.uniform1i(u.uMode, modeId);
    for (const k of ['ambient', 'flatten', 'exposure', 'normalStrength', 'wrap', 'spec', 'gloss', 'haze', 'shadow', 'soft'])
      gl.uniform1f(u['u' + k[0].toUpperCase() + k.slice(1)], p[k]);
    gl.uniform1i(u.uSteps, p.steps);
    const n = Math.min(4, p.lights.length);
    const pos = new Float32Array(12), col = new Float32Array(12);
    for (let i = 0; i < n; i++) { pos.set(p.lights[i].pos, i * 3); col.set(p.lights[i].col, i * 3); }
    gl.uniform1i(u.uNumLights, n);
    gl.uniform3fv(u.uLightPos, pos);
    gl.uniform3fv(u.uLightCol, col);
    gl.bindVertexArray(this.emptyVao);
    this._timed(() => gl.drawArrays(gl.TRIANGLES, 0, 3));
  }

  // p: { cam, mode, split, scale, ambient, flatten, exposure, normalStrength, wrap, spec, gloss,
  //      haze, shadow, soft, steps, lights:[{pos, col}], view (3D) }
  render(p) {
    if (!this.ready) return;
    const gl = this.gl;
    const modeId = { depth: 1, normals: 2 }[p.mode] ?? 0;
    const is3d = p.mode === '3d' && this.mesh && this.layers.background;
    this._relight(this.layers.photo, p, modeId);
    if (is3d) this._relight(this.layers.background, p, modeId);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.clearColor(0.043, 0.047, 0.059, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const photo = this.layers.photo;

    if (is3d) {
      const { p: prog, u } = this.meshProg;
      gl.useProgram(prog);
      gl.enable(gl.DEPTH_TEST);
      this._setCamera(u, p.cam);
      gl.uniformMatrix4fv(u.uProj, false, this.projection(p.cam));
      gl.uniformMatrix4fv(u.uView, false, p.view);
      gl.uniform2f(u.uScale, p.scale, p.scale);
      gl.uniform1i(u.uRelit, 0);
      gl.uniform1i(u.uDepth, 1);
      gl.bindVertexArray(this.mesh.vao);
      const draw = (layer, ibo, count, shade) => {
        gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, layer.target.tex);
        gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, layer.depth);
        gl.uniform1f(u.uShade, shade);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
        gl.drawElements(gl.TRIANGLES, count, gl.UNSIGNED_INT, 0);
      };
      // Background first, pushed back slightly so it never z-fights the photo where they coincide.
      gl.enable(gl.POLYGON_OFFSET_FILL);
      gl.polygonOffset(2, 64);
      draw(this.layers.background, this.mesh.background, this.mesh.backgroundCount, 1.0);
      gl.disable(gl.POLYGON_OFFSET_FILL);
      draw(photo, this.mesh.photo, this.mesh.photoCount, 1.0);
      gl.disable(gl.DEPTH_TEST);
    } else {
      const r = this.fitRect();
      gl.viewport(r.x, r.y, r.w, r.h);
      const { p: prog, u } = this.displayProg;
      gl.useProgram(prog);
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, photo.target.tex);
      gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, photo.image);
      gl.uniform1i(u.uRelit, 0);
      gl.uniform1i(u.uImage, 2);
      gl.uniform2f(u.uScale, p.scale, p.scale);
      gl.uniform1f(u.uSplit, p.mode === 'split' ? p.split : -1);
      gl.uniform1f(u.uLineWidth, 1.5 / r.w);
      gl.bindVertexArray(this.emptyVao);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
    }
    gl.bindVertexArray(null);
    this.frames++;
  }

  // GPU time of a light pass, read back asynchronously (never stalls the pipeline).
  _timed(draw) {
    const gl = this.gl, ext = this.timer;
    if (!ext || this._pendingQuery) { draw(); return; }
    const q = gl.createQuery();
    gl.beginQuery(ext.TIME_ELAPSED_EXT, q);
    draw();
    gl.endQuery(ext.TIME_ELAPSED_EXT);
    this._pendingQuery = q;
    const poll = () => {
      if (!gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) { setTimeout(poll, 16); return; }
      if (!gl.getParameter(ext.GPU_DISJOINT_EXT)) this.gpuMs = gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6;
      gl.deleteQuery(q);
      this._pendingQuery = null;
      this.onTiming?.(this.gpuMs);
    };
    setTimeout(poll, 16);
  }

  // Relit photo at full working resolution, top-down RGBA (for "Save image"). Call after a
  // full-quality render (scale 1).
  readRelit() {
    const gl = this.gl, { width: w, height: h, fbo } = this.layers.photo.target;
    const px = new Uint8Array(w * h * 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return new ImageData(new Uint8ClampedArray(flipRows(px, w, h, 4).buffer), w, h);
  }

  // What is on screen in a rectangle (device pixels, GL origin), top-down RGBA. Must be called in
  // the same task as render(): the drawing buffer is not preserved after compositing.
  readScreen(r = { x: 0, y: 0, w: this.canvas.width, h: this.canvas.height }) {
    const gl = this.gl;
    const px = new Uint8Array(r.w * r.h * 4);
    gl.readPixels(r.x, r.y, r.w, r.h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    return new ImageData(new Uint8ClampedArray(flipRows(px, r.w, r.h, 4).buffer), r.w, r.h);
  }
}
