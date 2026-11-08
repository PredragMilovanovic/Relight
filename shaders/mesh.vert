#version 300 es
// Pass 2b: the layered 3D relief. Vertices carry only (u, v); depth comes from the texture.
precision highp float;
in vec2 aUv;
out vec2 vUv;
uniform mat4 uProj, uView;
#include "common.glsl"
void main() {
  vUv = aUv;
  gl_Position = uProj * uView * vec4(posAt(aUv), 1.0);
}
