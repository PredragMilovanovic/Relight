// Loads the GLSL ES 3.00 sources from shaders/ and resolves `#include "file"` lines (WebGL has
// no include mechanism). The camera functions in shaders/common.glsl mirror core/src/camera.cpp.
const base = new URL('../shaders/', import.meta.url);
const cache = new Map();

async function source(name) {
  if (!cache.has(name)) {
    cache.set(name, fetch(new URL(name, base)).then((r) => {
      if (!r.ok) throw new Error(`shader ${name}: HTTP ${r.status}`);
      return r.text();
    }));
  }
  const text = await cache.get(name);
  const parts = await Promise.all(text.split('\n').map(async (line) => {
    const m = line.match(/^\s*#include\s+"([\w.-]+)"\s*$/);
    return m ? source(m[1]) : line;
  }));
  return parts.join('\n');
}

const [FULLSCREEN_VS, RELIGHT_FS, DISPLAY_FS, MESH_VS, MESH_FS] = await Promise.all(
  ['fullscreen.vert', 'relight.frag', 'display.frag', 'mesh.vert', 'mesh.frag'].map(source));

export { FULLSCREEN_VS, RELIGHT_FS, DISPLAY_FS, MESH_VS, MESH_FS };
