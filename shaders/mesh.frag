#version 300 es
// Pass 2b: textured with the layer's own relit image.
precision highp float;
in vec2 vUv;
out vec4 outColor;
uniform sampler2D uRelit;
uniform vec2 uScale;
uniform float uShade;              // background layer is drawn slightly darker (it is a guess)
void main() {
  outColor = vec4(texture(uRelit, vUv * uScale).rgb * uShade, 1.0);
}
