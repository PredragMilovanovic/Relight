// The little math the UI needs. Everything numeric (depth processing, camera model, lamp
// placement, colour temperature, airlight, 4x4 matrices, the depth file format) lives in the C++
// core (core/src, tested with GoogleTest) and reaches JavaScript through js/core.js.

export const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));

export function hexToRGB(hex) {
  const n = parseInt(hex.replace('#', ''), 16);
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

export function rgbToHex(rgb) {
  return '#' + rgb.map((c) => Math.round(clamp(c, 0, 1) * 255).toString(16).padStart(2, '0')).join('');
}
