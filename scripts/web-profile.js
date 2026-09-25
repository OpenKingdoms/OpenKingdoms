/*
 * web-profile.js -- where a browser battle spends its time, by function.
 *
 *   node scripts/web-profile.js <url> <game dir> [scenario] [from] [to]
 *
 * Runs a --perf-probe scenario the way web-perf.js does, and between
 * sim ticks `from` and `to` (default 6000 and 18000) records a sampling
 * profile through the DevTools protocol. The wasm build keeps its
 * function names (--profiling-funcs), so the table names engine
 * functions. Prints the top functions by self time and writes the raw
 * profile to WEB_SMOKE_OUT. Headless on the GPU unless
 * WEB_PERF_HEADLESS=0. Needs `playwright` resolvable.
 */
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const argv = process.argv.slice(2);
const url = argv[0] || 'http://localhost:8098/tak-re.html';
const gameDir = argv[1] || 'C:/GOG Games/Total Annihilation Kingdoms';
const scenario = argv[2] || 'build8';
const from = parseInt(argv[3] || '6000', 10);
const to = parseInt(argv[4] || '18000', 10);
const outDir = process.env.WEB_SMOKE_OUT || path.join(process.cwd(), 'web-profile-out');
const headless = process.env.WEB_PERF_HEADLESS !== '0';

(async () => {
  fs.mkdirSync(outDir, { recursive: true });
  const profileDir = path.join(outDir, 'profile');
  fs.rmSync(profileDir, { recursive: true, force: true });
  const launch = { channel: process.env.WEB_SMOKE_CHANNEL || 'msedge', headless };
  if (headless) launch.args = ['--use-angle=d3d11', '--ignore-gpu-blocklist'];
  const ctx = await chromium.launchPersistentContext(profileDir, launch);
  const page = await ctx.newPage();
  let tick = 0;
  page.on('console', m => {
    const t = m.text();
    const k = /^perf-probe \S+ w\d+ tick=(\d+)/.exec(t);
    if (k) { tick = parseInt(k[1], 10); console.log('  > ' + t.slice(0, 120)); }
  });
  const sep = url.includes('?') ? '&' : '?';
  await page.goto(url + sep + 'args=' + encodeURIComponent('--perf-probe ' + scenario), { waitUntil: 'load' });
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  const hpis = fs.readdirSync(gameDir).filter(n => /\.hpi$/i.test(n)).map(n => path.join(gameDir, n));
  await page.setInputFiles('#hpi-input', hpis);
  await page.waitForSelector('#btn-start:visible', { timeout: 240000 });
  await page.click('#btn-start');

  const cdp = await ctx.newCDPSession(page);
  await cdp.send('Profiler.enable');
  await cdp.send('Profiler.setSamplingInterval', { interval: 250 });
  while (tick < from) await page.waitForTimeout(1000);
  console.log('profiling from tick ' + tick);
  await cdp.send('Profiler.start');
  while (tick < to) await page.waitForTimeout(1000);
  const { profile } = await cdp.send('Profiler.stop');
  console.log('stopped at tick ' + tick);
  fs.writeFileSync(path.join(outDir, scenario + '.cpuprofile'), JSON.stringify(profile));
  await ctx.close();

  /* Self time per function: samples land on nodes, time deltas follow. */
  const byId = new Map(profile.nodes.map(n => [n.id, n]));
  const self = new Map();
  let total = 0;
  for (let i = 0; i < profile.samples.length; i++) {
    const n = byId.get(profile.samples[i]);
    const dt = (profile.timeDeltas[i + 1] || 0) / 1000;
    const f = n.callFrame;
    const name = (f.functionName || '(anonymous)') + (f.url && !/\.wasm/.test(f.url) ? ' ' + path.basename(f.url) : '');
    self.set(name, (self.get(name) || 0) + dt);
    total += dt;
  }
  const rows = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 45);
  console.log('\n' + (total / 1000).toFixed(1) + ' s sampled, top functions by self time');
  for (const [name, ms] of rows)
    console.log('  ' + (ms / total * 100).toFixed(1).padStart(5) + '%  ' + ms.toFixed(0).padStart(7) + ' ms  ' + name);
})().catch(e => { console.error(e.stack || String(e)); process.exit(1); });
