/* Browser performance probe.
 *
 * Feeds the archives from a real install to a served engine build, the
 * same way scripts/web-smoke.js does, then runs each scenario with
 * ?args=--perf-probe <name> and reads the probe lines from the console:
 *   perf-probe <name> wNN key=value ...    one per 600 sim ticks
 *   perf-probe <name> done key=value ...   whole-run frame figures
 * The run fails when no probe line prints or when a line breaks one of
 * the limits below. docs/notes/2026-09-11-perf-probes.md explains the
 * fields and records the baseline.
 *
 *   node scripts/web-perf.js [url] [gameDir] [scenario ...]
 *     url       default http://localhost:8081/tak-re.html
 *     gameDir   default C:/GOG Games/Total Annihilation Kingdoms
 *     scenario  ffa, crowd (default both)
 *
 * WEB_SMOKE_OUT sets the output folder, WEB_PERF_TICKS shortens every
 * scenario (limits that need the full length are then skipped), and
 * WEB_PERF_HEADLESS=1 runs headless on the software renderer instead of
 * a visible window on the GPU. Needs `playwright` resolvable.
 */
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const argv = process.argv.slice(2);
const url = argv[0] || 'http://localhost:8081/tak-re.html';
const gameDir = argv[1] || 'C:/GOG Games/Total Annihilation Kingdoms';
const scenarios = argv.length > 2 ? argv.slice(2) : ['ffa', 'crowd'];
const outDir = process.env.WEB_SMOKE_OUT || path.join(process.cwd(), 'web-perf-out');
const ticksOverride = parseInt(process.env.WEB_PERF_TICKS || '0', 10);
const headless = process.env.WEB_PERF_HEADLESS === '1';
const BOOT_TIMEOUT = 240000;
const FATAL = /Failed to initialize|VFS_Init: cannot|abort\(|Aborted\(|RuntimeError|PAGEERROR/;
/* Full lengths in sim ticks, matching src/ui/perf_probe.c. */
const LENGTH = { ffa: 43200, crowd: 18000 };

/* Limits from the issue #60 plan, section 4 (browser). */
const LIMIT = {
  p95: 16.7, p99: 25.0,       /* whole-frame ms over the run */
  cappedPct: 0.5,             /* frames at the 4-tick catch-up cap */
  pathAvg: 1.0,               /* pathing ms per tick, per 600-tick window */
  pathWorst: 3.0,             /* pathing ms in any one tick */
  aiWorst: 3.0,               /* AI ms in any one tick, all players */
  pmemKB: 16 * 1024,          /* planner caches */
  ffaUnits: 300,              /* live units through the last 10 sim minutes */
};

const log = [];
let page = null;

function saveLog() {
  fs.writeFileSync(path.join(outDir, 'console.log'), log.join('\n'));
}
async function fail(msg, tag) {
  try { if (page) await page.screenshot({ path: path.join(outDir, 'fail-' + (tag || 'error') + '.png') }); } catch (e) {}
  saveLog();
  console.log('--- last console lines ---');
  log.slice(-15).forEach(l => console.log('  ' + l));
  console.error('FAIL: ' + msg);
  process.exit(1);
}
function booted() {
  return page.waitForFunction(() =>
    document.getElementById('picker').hidden &&
    window.Module && window.Module.canvas && window.Module.canvas.width > 0,
    null, { timeout: BOOT_TIMEOUT });
}
async function pressStart() {
  await page.waitForSelector('#btn-start:visible', { timeout: BOOT_TIMEOUT });
  await page.click('#btn-start');
}
function fields(line) {
  const out = {};
  for (const tok of line.trim().split(/\s+/)) {
    const eq = tok.indexOf('=');
    if (eq > 0) out[tok.slice(0, eq)] = tok.slice(eq + 1);
  }
  return out;
}
function num(f, k) { return k in f ? parseFloat(f[k]) : NaN; }

/* Drives one scenario and returns its probe lines, or fails. */
async function runScenario(name, first) {
  console.log('scenario ' + name);
  const mark = log.length;
  const sep = url.includes('?') ? '&' : '?';
  let a = '--perf-probe ' + name;
  if (ticksOverride > 0) a += ' --perf-ticks ' + ticksOverride;
  await page.goto(url + sep + 'args=' + encodeURIComponent(a), { waitUntil: 'load' });
  if (first) {
    await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
    const hpis = fs.readdirSync(gameDir).filter(n => /\.hpi$/i.test(n)).map(n => path.join(gameDir, n));
    if (!hpis.length) return fail('no .hpi files in ' + gameDir);
    console.log('   feeding ' + hpis.length + ' archives');
    await page.setInputFiles('#hpi-input', hpis);
  }
  await pressStart();
  await booted();
  const ticks = ticksOverride > 0 ? ticksOverride : LENGTH[name];
  /* Real time is sim time at 60 Hz, plus loading and slack for a slow frame rate. */
  const deadline = Date.now() + 300000 + ticks * 1000 / 60 * 3;
  const firstLineBy = Date.now() + BOOT_TIMEOUT;
  const tag = 'perf-probe ' + name + ' ';
  for (;;) {
    const mine = log.slice(mark);
    const bad = mine.find(l => FATAL.test(l));
    if (bad) return fail(name + ': engine reported a failure: ' + bad, name);
    if (mine.some(l => /Unknown flag: --perf-probe/.test(l)))
      return fail(name + ': the engine does not know --perf-probe, no probe line printed', name);
    const lines = mine.filter(l => l.startsWith(tag));
    if (lines.some(l => l.startsWith(tag + 'done'))) return lines;
    if (!lines.length && Date.now() > firstLineBy)
      return fail(name + ': no probe line printed within ' + BOOT_TIMEOUT / 1000 + ' s of boot', name);
    if (Date.now() > deadline) return fail(name + ': no done line before the deadline', name);
    await page.waitForTimeout(2000);
  }
}

function judge(name, lines) {
  const rows = [];
  const check = (what, ok, got, limit) => rows.push({ what, ok, got, limit });
  const windows = lines.filter(l => / w\d+ /.test(l)).map(fields);
  const done = fields(lines.find(l => / done /.test(l)) || '');
  const full = ticksOverride <= 0 || ticksOverride >= LENGTH[name];
  if (!windows.length) { check('window lines', false, 0, '>= 1'); return rows; }
  const frames = num(done, 'frames');
  check('frame p95 ms', num(done, 'p95') <= LIMIT.p95, done.p95, '<= ' + LIMIT.p95);
  check('frame p99 ms', num(done, 'p99') <= LIMIT.p99, done.p99, '<= ' + LIMIT.p99);
  const pct = frames > 0 ? 100 * num(done, 'capped') / frames : 0;
  check('catch-up capped %', pct <= LIMIT.cappedPct, pct.toFixed(2), '<= ' + LIMIT.cappedPct);
  let pathAvg = 0, pathWorst = 0, aiWorst = 0, pmem = 0, stall = 0;
  for (const w of windows) {
    const t = num(w, 'ticks') || 600;
    pathAvg = Math.max(pathAvg, num(w, 'path') / t);
    pathWorst = Math.max(pathWorst, num(w, 'path_worst'));
    aiWorst = Math.max(aiWorst, num(w, 'ai_worst'));
    pmem = Math.max(pmem, num(w, 'pmem'));
    stall = Math.max(stall, num(w, 'stall_max'));
  }
  check('pathing ms/tick, worst window', pathAvg <= LIMIT.pathAvg, pathAvg.toFixed(3), '<= ' + LIMIT.pathAvg);
  check('pathing ms, worst tick', pathWorst <= LIMIT.pathWorst, pathWorst.toFixed(2), '<= ' + LIMIT.pathWorst);
  check('AI ms, worst tick', aiWorst <= LIMIT.aiWorst, aiWorst.toFixed(2), '<= ' + LIMIT.aiWorst);
  check('planner cache KB', pmem <= LIMIT.pmemKB, pmem, '<= ' + LIMIT.pmemKB);
  check('stall census, every sample', stall === 0, stall, '== 0');
  if (name === 'ffa' && full) {
    const late = windows.filter(w => num(w, 'tick') > 2 * 3600);
    const least = late.reduce((m, w) => Math.min(m, num(w, 'units')), Infinity);
    check('live units, last 10 sim minutes', least >= LIMIT.ffaUnits, least, '>= ' + LIMIT.ffaUnits);
  }
  check('run completed', done.end === 'complete', done.end || 'none', 'complete');
  return rows;
}

(async () => {
  fs.mkdirSync(outDir, { recursive: true });
  const profile = path.join(outDir, 'profile');
  fs.rmSync(profile, { recursive: true, force: true });
  const launch = { channel: process.env.WEB_SMOKE_CHANNEL || 'msedge', headless: headless };
  if (headless) launch.args = ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'];
  const ctx = await chromium.launchPersistentContext(profile, launch);
  page = await ctx.newPage();
  page.on('console', m => {
    const t = m.text();
    log.push(t);
    if (t.startsWith('perf-probe') || /Failed|abort|Unknown flag/i.test(t)) console.log('  > ' + t);
  });
  page.on('pageerror', e => { log.push('PAGEERROR ' + e.message); console.log('  > PAGEERROR', e.message); });

  const results = [];
  for (let i = 0; i < scenarios.length; i++) {
    const lines = await runScenario(scenarios[i], i === 0);
    fs.writeFileSync(path.join(outDir, 'probe-' + scenarios[i] + '.txt'), lines.join('\n') + '\n');
    results.push([scenarios[i], judge(scenarios[i], lines)]);
  }
  saveLog();
  await ctx.close();

  let failed = 0;
  for (const r of results) {
    console.log('');
    console.log(r[0] + (ticksOverride > 0 ? ' (' + ticksOverride + ' ticks)' : ''));
    for (const row of r[1]) {
      if (!row.ok) failed++;
      console.log('  ' + (row.ok ? 'ok  ' : 'FAIL') + '  ' + row.what.padEnd(34) +
                  String(row.got).padStart(10) + '   ' + row.limit);
    }
  }
  if (failed) { console.error('FAIL: ' + failed + ' limit(s) broken'); process.exit(1); }
  console.log('PASS');
})().catch(e => fail(e.stack || String(e)));
