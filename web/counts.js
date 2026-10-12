/*
 * counts.js -- the site's own visit counts, sent to the game server.
 *
 * privacy.html says what is counted and why, in plain words. In short:
 * page views, how far a visit got, the battles played and how they
 * ended, error messages and a rough picture of the device, with a
 * random id this browser keeps so that each day's visitors are counted
 * once each. The server keeps daily totals and nothing else.
 *
 * Nothing is sent when the browser asks not to be tracked, by Do Not
 * Track or Global Privacy Control, or when the visitor turned counting
 * off on privacy.html. Nothing here throws into the page or waits on
 * the network: a server that is down means nothing is counted.
 *
 * No DOM in the parts worth testing, so web/test_counts.js runs them
 * in node.
 */
(function (root, factory) {
  'use strict';
  var api = factory(root);
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OKCounts = api;
})(this, function (root) {
  'use strict';

  var VERSION = 1;
  var MAX_EVENTS = 24;          /* a batch, the server's cap */
  var MAX_BODY = 3800;          /* bytes a batch, under the server's 4096 */
  var FLUSH_MS = 3000;
  var HELD_MAX = 100;           /* events held while the server's address is looked up */
  var ERRORS_MAX = 5;           /* error events one page load sends */
  var ID_KEY = 'ok.counts.id';
  var OFF_KEY = 'ok.counts.off';
  var STEPS_KEY = 'ok.counts.steps';
  var SESSION_KEY = 'ok.counts.session';

  /* The fields each kind of event may carry, as the engine names them. */
  var FIELDS = {
    start: ['mode', 'sub', 'map', 'camp', 'mission', 'ai', 'lvl', 'humans', 'rb', 'mod', 'side'],
    end: ['mode', 'out', 'secs'],
    error: ['msg']
  };

  /* ---- what a browser is, coarsely ----------------------------------- */

  function browserFamily(ua, brands) {
    ua = ua || '';
    var names = (brands || []).map(function (b) { return b && b.brand ? b.brand : ''; }).join(' ');
    if (/Edg(e|A|iOS)?\//.test(ua) || /Microsoft Edge/.test(names)) return 'edge';
    if (/OPR\/|Opera|OPiOS/.test(ua) || /Opera/.test(names)) return 'opera';
    if (/SamsungBrowser/.test(ua)) return 'samsung';
    if (/Firefox\/|FxiOS/.test(ua)) return 'firefox';
    if (/CriOS|Chrome\/|Chromium/.test(ua) || /Google Chrome|Chromium/.test(names)) return 'chrome';
    if (/Safari\//.test(ua) && /Version\//.test(ua)) return 'safari';
    return 'other';
  }

  /* An iPad asking for the desktop site says Macintosh, and only its
     touch points tell. */
  function osFamily(ua, touchPoints) {
    ua = ua || '';
    if (/Windows/.test(ua)) return 'windows';
    if (/iPhone|iPad|iPod/.test(ua)) return 'ios';
    if (/Macintosh|Mac OS X/.test(ua)) return touchPoints > 1 ? 'ios' : 'macos';
    if (/Android/.test(ua)) return 'android';
    if (/CrOS/.test(ua)) return 'chromeos';
    if (/Linux|X11/.test(ua)) return 'linux';
    return 'other';
  }

  function deviceKind(ua, mobileHint, touchPoints) {
    ua = ua || '';
    if (/iPad|Tablet/.test(ua) || (/Android/.test(ua) && !/Mobile/.test(ua)) ||
        (/Macintosh/.test(ua) && touchPoints > 1)) return 'tablet';
    if (mobileHint === true || /Mobi|iPhone|iPod|Android/.test(ua)) return 'mobile';
    return 'desktop';
  }

  /* The screen's long side in CSS pixels, in the buckets the server counts. */
  function screenBucket(w, h) {
    var side = Math.max(+w || 0, +h || 0);
    if (!side) return 'other';
    if (side < 1024) return '<1024';
    if (side < 1366) return '1024-1365';
    if (side < 1600) return '1366-1599';
    if (side < 1920) return '1600-1919';
    if (side < 2560) return '1920-2559';
    return '2560+';
  }

  /* An error's first line, with addresses taken out, so the same fault
     reads the same from every machine. */
  function cleanMessage(msg) {
    var s = String(msg == null ? '' : (msg && msg.message) || msg);
    s = s.split(/\r?\n/)[0].replace(/(https?|blob|file):\/\/\S+/g, 'url');
    s = s.replace(/\s+/g, ' ').trim();
    return (s || 'unknown').slice(0, 120);
  }

  /* Why nothing may be sent, or '' when counting may go ahead. */
  function refusal(nav, win, local) {
    nav = nav || {};
    win = win || {};
    var dnt = nav.doNotTrack || win.doNotTrack || nav.msDoNotTrack;
    if (dnt === '1' || dnt === 1 || dnt === 'yes') return 'dnt';
    if (nav.globalPrivacyControl === true) return 'gpc';
    try { if (local && local.getItem(OFF_KEY) === '1') return 'off'; } catch (e) {}
    return '';
  }

  /* The relay's counting address from the relay.txt beside the page,
     or '' when it names none. */
  function apiFrom(text) {
    var url = String(text || '').split(/[\r\n]/)[0].trim();
    url = url.replace(/^wss:\/\//i, 'https://').replace(/^ws:\/\//i, 'http://');
    url = url.replace(/\/relay\/?$/, '').replace(/\/+$/, '');
    return /^https?:\/\/[^\s/]+(\/[^\s]*)?$/.test(url) ? url + '/api/ev' : '';
  }

  /* 22 characters of 64, random, naming nothing. */
  function newId(crypto) {
    var bytes = new Uint8Array(22);
    if (crypto && crypto.getRandomValues) crypto.getRandomValues(bytes);
    else for (var i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
    var abc = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
    var out = '';
    for (var k = 0; k < bytes.length; k++) out += abc.charAt(bytes[k] & 63);
    return out;
  }

  /* A string's length in UTF-8 bytes, which is what the server's cap counts. */
  function byteLength(s) {
    var n = 0;
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 0x80) n += 1;
      else if (c < 0x800) n += 2;
      else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) { n += 4; i++; }
      else n += 3;
    }
    return n;
  }

  /* Bodies for a list of events, each under both of the server's caps. */
  function bodies(head, events) {
    var out = [];
    var batch = [];
    function close() {
      if (!batch.length) return;
      out.push(JSON.stringify(Object.assign({}, head, { ev: batch })));
      batch = [];
    }
    events.forEach(function (e) {
      batch.push(e);
      var whole = JSON.stringify(Object.assign({}, head, { ev: batch }));
      if (byteLength(whole) > MAX_BODY && batch.length > 1) {
        batch.pop();
        close();
        batch.push(e);
      }
      if (batch.length >= MAX_EVENTS) close();
    });
    close();
    return out.filter(function (b) { return byteLength(b) <= MAX_BODY; });
  }

  /* ---- the counter ------------------------------------------------- */

  /* One page load's counter. `env` holds the browser's pieces, so a
     test can hand in its own: nav, win, local and session storage,
     crypto, screen, relayText (a promise of relay.txt), now and timers. */
  function create(env) {
    env = env || {};
    var nav = env.nav || {};
    var win = env.win || {};
    var local = env.local || null;
    var session = env.session || null;
    var why = refusal(nav, win, local);
    var queue = [];
    var api = null;           /* '' once the lookup found no server */
    var timer = null;
    var errorsSent = {};
    var errorCount = 0;
    var stepsSent = {};
    var id = null;
    var made = false;
    var viewed = false;
    var battle = null;        /* the battle under way: its kind and when it began */
    var now = env.now || function () { return Date.now(); };

    function get(store, key) { try { return store ? store.getItem(key) : null; } catch (e) { return null; } }
    function put(store, key, v) { try { if (store) store.setItem(key, v); } catch (e) {} }
    function drop(store, key) { try { if (store) store.removeItem(key); } catch (e) {} }

    function visitor() {
      if (id) return id;
      id = get(local, ID_KEY);
      if (!id || !/^[A-Za-z0-9_-]{16,64}$/.test(id)) {
        id = newId(env.crypto);
        made = true;
        put(local, ID_KEY, id);
      }
      return id;
    }

    function envFields() {
      var hints = nav.userAgentData || {};
      var scr = env.screen || {};
      return {
        br: browserFamily(nav.userAgent, hints.brands),
        os: osFamily(nav.userAgent, nav.maxTouchPoints || 0),
        dev: deviceKind(nav.userAgent, hints.mobile, nav.maxTouchPoints || 0),
        scr: screenBucket(scr.width, scr.height)
      };
    }

    function send(url, body) {
      try {
        if (nav.sendBeacon && nav.sendBeacon(url, body)) return;
      } catch (e) {}
      try {
        if (win.fetch) {
          var p = win.fetch(url, { method: 'POST', body: body, keepalive: true, mode: 'no-cors',
                                   credentials: 'omit', headers: { 'Content-Type': 'text/plain' } });
          if (p && p.catch) p.catch(function () {});
        }
      } catch (e) {}
    }

    function flush() {
      if (timer) { (env.clearTimeout || clearTimeout)(timer); timer = null; }
      if (why || !api || !queue.length) {
        if (why || api === '') queue = [];
        return;
      }
      var events = queue;
      queue = [];
      var head = { v: VERSION, id: visitor() };
      if (made) head.nv = 1;
      head.env = envFields();
      bodies(head, events).forEach(function (b) { send(api, b); });
    }

    function later() {
      if (timer || why) return;
      timer = (env.setTimeout || setTimeout)(function () { timer = null; flush(); }, FLUSH_MS);
    }

    function add(event) {
      if (why) return;
      if (api === '') return;
      if (queue.length >= HELD_MAX) return;
      queue.push(event);
      if (queue.length >= MAX_EVENTS) flush();
      else later();
    }

    /* Each step a session reaches is counted once for the session. */
    function once(kind, value) {
      var key = kind + ':' + value;
      var seen = null;
      try { seen = JSON.parse(get(session, STEPS_KEY) || '[]'); } catch (e) { seen = []; }
      if (!Array.isArray(seen)) seen = [];
      if (stepsSent[key] || seen.indexOf(key) >= 0) return false;
      stepsSent[key] = true;
      seen.push(key);
      put(session, STEPS_KEY, JSON.stringify(seen));
      return true;
    }

    if (!why && env.relayText && env.relayText.then) {
      env.relayText.then(function (text) {
        api = apiFrom(text);
        if (!api) queue = [];
        else if (queue.length) later();
      }, function () { api = ''; queue = []; });
    } else if (!why) {
      api = '';
    }

    return {
      /* A page view. The first of a tab's session begins it. */
      view: function (page) {
        if (why) return;
        var fresh = !viewed && !get(session, SESSION_KEY);
        viewed = true;
        put(session, SESSION_KEY, '1');
        add({ k: 'view', page: String(page), s: fresh ? 1 : 0 });
      },
      /* How far the visit got: visit, files, menu, battle, or a screen. */
      step: function (name) {
        if (why || !once('step', name)) return;
        add({ k: 'step', step: String(name) });
      },
      /* What the engine said: 'start', 'end' or 'error' and its fields. */
      game: function (kind, data) {
        if (why || !FIELDS[kind] || !data || typeof data !== 'object') return;
        if (kind === 'error') { this.error(data.msg, false); return; }
        var e = { k: kind };
        FIELDS[kind].forEach(function (f) {
          var v = data[f];
          if (typeof v === 'string') e[f] = v.slice(0, 120);
          else if (typeof v === 'number' && isFinite(v)) e[f] = Math.max(0, Math.floor(v));
        });
        add(e);
        if (kind === 'start') {
          battle = { mode: e.mode, t0: now() };
          this.step('battle');
        } else {
          battle = null;
        }
      },
      /* The page is going. A battle still under way ended with it, after
         about as long as the page has had it. */
      leave: function () {
        if (battle && !why) {
          add({ k: 'end', mode: battle.mode, out: 'left',
                secs: Math.max(0, Math.floor((now() - battle.t0) / 1000)) });
        }
        battle = null;
        flush();
      },
      error: function (msg, fatal) {
        if (why) return;
        var m = cleanMessage(msg);
        if (errorsSent[m] || errorCount >= ERRORS_MAX) return;
        errorsSent[m] = true;
        errorCount++;
        add({ k: 'error', msg: m, fatal: fatal ? 1 : 0 });
        if (fatal) flush();
      },
      flush: flush,
      /* Whether this page load counts, and if not, why not. */
      state: function () { return { counting: !why, why: why }; },
      optOut: function () {
        put(local, OFF_KEY, '1');
        drop(local, ID_KEY);
        drop(session, STEPS_KEY);
        drop(session, SESSION_KEY);
        why = refusal(nav, win, local) || 'off';
        queue = [];
      },
      optIn: function () {
        drop(local, OFF_KEY);
      }
    };
  }

  var exported = {
    browserFamily: browserFamily,
    osFamily: osFamily,
    deviceKind: deviceKind,
    screenBucket: screenBucket,
    cleanMessage: cleanMessage,
    refusal: refusal,
    apiFrom: apiFrom,
    newId: newId,
    byteLength: byteLength,
    bodies: bodies,
    create: create,
    MAX_EVENTS: MAX_EVENTS,
    MAX_BODY: MAX_BODY
  };

  /* In a page, one counter for the load, flushed as the page goes. */
  if (root && root.document && root.navigator) {
    var store = function (name) { try { return root[name]; } catch (e) { return null; } };
    var relay = null;
    try {
      relay = root.fetch ? root.fetch('relay.txt', { cache: 'no-store' })
        .then(function (r) { return r.ok ? r.text() : ''; }) : null;
    } catch (e) { relay = null; }
    var counter = create({
      nav: root.navigator, win: root, local: store('localStorage'), session: store('sessionStorage'),
      crypto: root.crypto, screen: root.screen, relayText: relay
    });
    ['view', 'step', 'game', 'error', 'flush', 'leave', 'state', 'optOut', 'optIn'].forEach(function (k) {
      exported[k] = function () {
        try { return counter[k].apply(counter, arguments); } catch (e) { return undefined; }
      };
    });
    try {
      root.addEventListener('pagehide', function () { exported.leave(); });
      root.document.addEventListener('visibilitychange', function () {
        if (root.document.visibilityState === 'hidden') exported.flush();
      });
    } catch (e) {}
  }

  return exported;
});
