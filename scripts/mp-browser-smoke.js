/* Does a browser reach a relay, and does it show what it finds?
 *
 * Everything under this is tested without a browser: the wire format
 * against the RFC, the room rules over a fake network, the client
 * against the real relay core in one process. None of that can say
 * whether a page opens a socket and draws what comes back, and until
 * this passes the browser path should not be described as working.
 *
 * It found two things a canvas-reading test could not. The screen drew
 * and never presented, which is a black window. And every send threw,
 * because the page reached HEAPU8 through Module, where it is not
 * exported.
 *
 * Not part of CI: it needs a browser, a served build, a relay to talk
 * to and the player's own game files.
 *
 *   okrelay --port 8811 &
 *   python -m http.server 8082 -d <wasm build>/src
 *   node scripts/mp-browser-smoke.js <repo root>
 *
 * Add a game for it to find with:
 *   relay_link ws://127.0.0.1:8811/play --hold 120 &
 */
const fs = require('fs');
const path = require('path');
const root = process.argv[2] || '.';
const { chromium } = require(path.join(root, 'node_modules', 'playwright'));

const URL_BASE = process.argv[3] || 'http://localhost:8082/tak-re.html';
const RELAY = process.argv[4] || 'ws://127.0.0.1:8811/play';
const GAME_DIR = process.argv[5] || 'C:/GOG Games/Total Annihilation Kingdoms';

function archives(dir) {
  return fs.readdirSync(dir)
    .filter(f => /\.(hpi|ufo|ccx|gpf|gp3)$/i.test(f))
    .map(f => path.join(dir, f));
}

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await (await browser.newContext()).newPage();
  const log = [];
  page.on('console', m => log.push(m.text()));
  page.on('pageerror', e => log.push('PAGEERROR ' + e.message));

  /* The socket the page opens is the thing being proven, so it is
   * watched directly rather than inferred from a screenshot. */
  const sockets = [];
  page.on('websocket', ws => sockets.push(ws.url()));

  const args = encodeURIComponent('--multiplayer --relay ' + RELAY);
  await page.goto(URL_BASE + '?args=' + args, { waitUntil: 'load' });

  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  const hpis = archives(GAME_DIR);
  console.log('feeding ' + hpis.length + ' archives');
  await page.setInputFiles('#hpi-input', hpis);
  await page.waitForSelector('#btn-start:visible', { timeout: 60000 });
  await page.click('#btn-start');

  await page.waitForFunction(() =>
    document.getElementById('picker').hidden &&
    window.Module && window.Module.canvas && window.Module.canvas.width > 0,
    null, { timeout: 120000 });
  console.log('engine booted');

  await page.waitForTimeout(10000);
  console.log('websockets opened: ' + JSON.stringify(sockets));
  const reached = sockets.some(u => u.indexOf('8811') >= 0);
  console.log('reached the relay: ' + reached);

  /* Reaching it is not the same as being let in. The screen says what
   * happened, and a session that was welcomed is in the lobby. */
  const shot = path.join(root, 'mp-smoke.png');
  await page.screenshot({ path: shot });
  console.log('screenshot: ' + shot);
  for (const line of log.slice(-20)) console.log('  > ' + line);
  await browser.close();
  process.exit(reached ? 0 : 1);
})().catch(e => { console.log('FAILED ' + e.message); process.exit(2); });
