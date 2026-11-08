// Minimal test harness: no dependencies, runs in any browser. With ?report the results are
// POSTed to scripts/serve.ps1 (.cache/test-report.json) for scripts/test.ps1.
const tests = [];

export function test(name, fn) { tests.push({ name, fn }); }

export function assert(cond, msg = 'assertion failed') { if (!cond) throw new Error(msg); }

export function near(a, b, tol, msg = '') {
  if (!(Math.abs(a - b) <= tol)) throw new Error(`${msg} expected ${b} ± ${tol}, got ${a}`);
}

export async function run() {
  const lines = [];
  let failed = 0;
  const t0 = performance.now();
  for (const t of tests) {
    try {
      await t.fn();
      lines.push(`PASS ${t.name}`);
    } catch (err) {
      failed++;
      lines.push(`FAIL ${t.name}\n     ${String(err?.message || err)}`);
    }
  }
  lines.push(`${tests.length - failed}/${tests.length} browser tests passed in ${Math.round(performance.now() - t0)} ms`);
  const el = document.getElementById('report');
  el.innerHTML = lines.map((l) => l.replace(/^(PASS|FAIL)/, (m) => `<span class="${m.toLowerCase()}">${m}</span>`)).join('\n');
  document.documentElement.dataset.failed = String(failed);
  document.documentElement.dataset.done = '1';
  if (new URLSearchParams(location.search).has('report')) {
    await fetch('/save?path=' + encodeURIComponent('.cache/test-report.json'), {
      method: 'POST', body: JSON.stringify({ failed, lines }),
    });
  }
}
