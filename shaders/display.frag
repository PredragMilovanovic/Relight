#version 300 es
// Pass 2a: show the relit image, with an optional before/after split.
precision highp float;
in vec2 vUv;
out vec4 outColor;
uniform sampler2D uRelit;
uniform sampler2D uImage;
uniform vec2 uScale;               // part of the relit texture in use (< 1 while dragging)
uniform float uSplit;              // < 0: no split
uniform float uLineWidth;          // in uv units
vec3 toSRGB(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
void main() {
  vec3 c = texture(uRelit, vUv * uScale).rgb;
  if (uSplit >= 0.0) {
    if (vUv.x < uSplit) c = toSRGB(texture(uImage, vUv).rgb);
    float line = 1.0 - smoothstep(0.0, uLineWidth, abs(vUv.x - uSplit));
    c = mix(c, vec3(1.0), line * 0.9);
  }
  outColor = vec4(c, 1.0);
}
