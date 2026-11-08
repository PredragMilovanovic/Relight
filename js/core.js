// Loads the C++ core (compiled to WebAssembly by scripts/build_wasm.ps1) once for the whole app.
import createRelightCore from './relight_core.js';

export const core = await createRelightCore();
