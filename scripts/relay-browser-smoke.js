/* Can a player reach a game server that is not this page's own host,
 * and can they say who they are?
 *
 * Both were no. The page only ever tried its own origin, which on a
 * static file host answers nothing, and neither box on the screen took
 * a keystroke because nothing ever turned SDL's text input on.
 *
 *   okrelay --port 8811 &
 *   python -m http.server 8082 -d <wasm build>/src      # with relay.txt
 *   node scripts/relay-browser-smoke.js <repo root> [url] [gameDir] [relay]
 *
 * With a relay that is not the local one on port 8811, named as the
 * fourth argument or by the page's own relay.txt, the two address
 * checks look for that relay instead and the output says so.
 */
const fs = require('fs');
const path = require('path');
const root = process.argv[2] || '.';
const { chromium } = require(path.join(root, 'node_modules', 'playwright'));

const URL_BASE = process.argv[3] || 'http://localhost:8082/tak-re.html';
const GAME_DIR = process.argv[4] || 'C:/GOG Games/Total Annihilation Kingdoms';
const RELAY = process.argv[5] || '';
const OUT = path.join(root, 'reach-shots');
const LOCAL_RELAY = /^wss?:\/\/(localhost|127\.0\.0\.1|\[::1\]):8811(\/|$)/i;

const R = {
  Name:     { x: 69, y: 57, w: 194, h: 21 },
  HostGame: { x: 467, y: 406, w: 57, h: 57 },
  Join:     { x: 541, y: 407, w: 39, h: 54 },
  Row0:     { x: 64, y: 126, w: 311, h: 22 },
  Ready0:   { x: 70, y: 64, w: 13, h: 13 },
  Ready1:   { x: 70, y: 86, w: 13, h: 13 },
};

let failures = 0;
function check(what, ok) {
  console.log((ok ? '  ok   ' : '  FAIL ') + what);
  if (!ok) failures++;
}

function archives(dir) {
  return fs.readdirSync(dir)
    .filter(f => /\.(hpi|ufo|ccx|gpf|gp3)$/i.test(f))
    .map(f => path.join(dir, f));
}

async function clickVirtual(page, key) {
  const r = R[key];
  const pt = await page.evaluate((rect) => {
    const c = document.getElementById('canvas');
    const box = c.getBoundingClientRect();
    return {
      x: box.left + (rect.x + rect.w / 2) * (c.width / 640) * (box.width / c.width),
      y: box.top + (rect.y + rect.h / 2) * (c.height / 480) * (box.height / c.height),
    };
  }, r);
  await page.bringToFront();
  await page.mouse.move(pt.x, pt.y);
  await page.waitForTimeout(200);
  await page.mouse.down();
  await page.waitForTimeout(250);
  await page.mouse.up();
  await page.waitForTimeout(900);
}

async function boot(browser, label, log) {
  const page = await (await browser.newContext()).newPage();
  page.on('console', m => log.push(label + ': ' + m.text()));
  page.on('pageerror', e => log.push(label + ': PAGEERROR ' + e.message));
  page.on('websocket', ws => log.push(label + ': SOCKET ' + ws.url()));
  const args = '--multiplayer' + (RELAY ? ' --relay ' + RELAY : '');
  await page.goto(URL_BASE + '?args=' + encodeURIComponent(args),
                  { waitUntil: 'load' });
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  await page.setInputFiles('#hpi-input', archives(GAME_DIR));
  await page.waitForSelector('#btn-start:visible', { timeout: 60000 });
  await page.click('#btn-start');
  await page.waitForFunction(() =>
    document.getElementById('picker').hidden &&
    window.Module && window.Module.canvas && window.Module.canvas.width > 0,
    null, { timeout: 180000 });
  await page.waitForTimeout(6000);
  /* The page's plates sit over the lobby's lower buttons at this size,
     and a click meant for Host Game would land on Forget my game files. */
  await page.addStyleTag({ content: '#forget, #toast { display: none !important; }' });
  return page;
}

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const log = [];

  const host = await boot(browser, 'host', log);

  /* The page took the address out of relay.txt rather than assuming
     its own origin, which is the whole point on a static file host. */
  const relay = await host.evaluate(() => window.Module.okRelayUrl || '');
  const target = RELAY || relay;
  if (!target || LOCAL_RELAY.test(target)) {
    check('the page read the game server address: ' + JSON.stringify(relay),
          relay.indexOf('8811') >= 0);
    check('it opened a socket there',
          log.some(l => /SOCKET .*8811/.test(l)));
  } else {
    /* A remote relay: no port 8811 to look for, so look for its host. */
    const where = new URL(target).host;
    console.log('remote relay ' + target + ': the two local port 8811 checks look for ' + where + ' instead');
    if (RELAY) {
      console.log('  skip the relay.txt check: --relay named the address, not relay.txt');
    } else {
      check('the page read the game server address: ' + JSON.stringify(relay),
            /^wss?:\/\/[^\s]+$/.test(relay));
    }
    const opened = () => log.some(l => l.startsWith('host: SOCKET ') && l.indexOf(where) >= 0);
    for (let t = 0; t < 30 && !opened(); t++) await host.waitForTimeout(500);
    check('it opened a socket there', opened());
  }

  await host.screenshot({ path: path.join(OUT, '1-lobby.png') });

  /* A name, typed. Both boxes on this screen used to take nothing. */
  console.log('typing a name');
  await clickVirtual(host, 'Name');
  await host.keyboard.type('Zach', { delay: 60 });
  await host.waitForTimeout(600);
  await host.keyboard.press('Enter');
  await host.waitForTimeout(600);
  await host.screenshot({ path: path.join(OUT, '2-named.png') });

  console.log('hosting');
  await clickVirtual(host, 'HostGame');
  await host.waitForTimeout(2500);
  await host.screenshot({ path: path.join(OUT, '3-room.png') });

  const joiner = await boot(browser, 'join', log);
  await clickVirtual(joiner, 'Name');
  await joiner.keyboard.type('Bennett', { delay: 60 });
  await joiner.keyboard.press('Enter');
  await clickVirtual(joiner, 'Row0');
  await clickVirtual(joiner, 'Join');
  await joiner.waitForTimeout(2500);
  await joiner.screenshot({ path: path.join(OUT, '4-joined.png') });
  await host.screenshot({ path: path.join(OUT, '5-host-sees-joiner.png') });

  /* The names reached the server. The room reads its rows off the
     server's snapshot, so this is the whole path: box, greeting or
     edit, room state, row. */


  /* A name typed once is the name next time. */
  console.log('reloading to check the name stuck');
  await host.reload({ waitUntil: 'load' });
  await host.waitForFunction(() =>
    window.Module && window.Module.canvas && window.Module.canvas.width > 0,
    null, { timeout: 180000 }).catch(() => {});
  await host.waitForTimeout(8000);
  await host.screenshot({ path: path.join(OUT, '6-after-reload.png') });

  const errs = log.filter(l => l.indexOf('PAGEERROR') >= 0);
  check('the page threw nothing: ' + JSON.stringify(errs.slice(0, 2)), errs.length === 0);

  console.log('--- page lines ---');
  for (const l of log.filter(l => /openkingdoms|SOCKET|PAGEERROR/.test(l))) {
    console.log('    > ' + l);
  }
  await browser.close();
  console.log(failures ? failures + ' failure(s)' : 'all good');
  console.log('shots in ' + OUT);
  process.exit(failures ? 1 : 0);
})().catch(e => { console.log('FAILED ' + (e && e.stack || e)); process.exit(2); });
