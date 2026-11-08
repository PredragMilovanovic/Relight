// Camera model shared by all passes; mirrors core/src/camera.cpp (depth_to_view_z, unproject,
// project). Inserted where a shader says #include "common.glsl".
uniform sampler2D uDepth;
uniform float uAspect, uTanF, uNear, uFar;

float viewZ(vec2 uv) {
  float d = texture(uDepth, uv).r;
  return 1.0 / mix(1.0 / uFar, 1.0 / uNear, d);
}
vec3 posAt(vec2 uv) {
  float z = viewZ(uv);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * uAspect * uTanF * z, ndc.y * uTanF * z, -z);
}
vec2 toUv(vec3 p) {
  float z = -p.z;
  return vec2(p.x / (z * uAspect * uTanF), p.y / (z * uTanF)) * 0.5 + 0.5;
}
