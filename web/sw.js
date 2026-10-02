/*
 * sw.js -- the page and the engine from this browser's cache.
 *
 * A returning player gets the page, tak-re.js and tak-re.wasm from the
 * cache at once, so the menu is up in about a second and the game
 * starts with no network at all. The set is fetched again in the
 * background and, when the build changed, stored whole as a new
 * generation of the cache. The next visit gets that one.
 *
 * The script and the wasm are made for each other, so a page gets
 * every file it asks for from the generation that served the page, even
 * when a newer one lands while it loads. A generation stays while a
 * page still uses it.
 *
 * Everything else, relay.txt, the leaderboard and the game server,
 * goes to the network as if this were not here.
 */
'use strict';

var CACHE = 'ok-engine-';
var PAGE = './index.html';
var ENGINE = ['./tak-re.js', './tak-re.wasm'];
var PICTURES = ['./scroll-pick.jpg', './scroll-ready.jpg', './favicon.png'];
var NET = '';

/* The generation each page came from, by client id, or NET for a page
   that came from the network. Kept in memory only, so a restarted
   worker pins a page again on its first engine file. */
var pins = {};

function pinned(id) { return !!id && Object.prototype.hasOwnProperty.call(pins, id); }

self.addEventListener('install', function (e) {
  e.waitUntil(fetchSet().then(function (set) { return set ? store(set) : null; })
    .then(function () { return self.skipWaiting(); }));
});

self.addEventListener('activate', function (e) {
  e.waitUntil(self.clients.claim());
});

/* The page, the script, the engine and the pictures, fresh, or null
   when any of them did not come. */
function fetchSet() {
  var urls = [PAGE].concat(ENGINE, PICTURES);
  return Promise.all(urls.map(function (u) {
    return fetch(u, { cache: 'no-cache' }).then(function (r) { return r.ok ? r : null; });
  })).then(function (rs) {
    for (var i = 0; i < rs.length; i++) if (!rs[i]) return null;
    return urls.map(function (u, i) { return [u, rs[i]]; });
  }).catch(function () { return null; });
}

/* The newest generation that is whole, or NET when there is none. The
   page is stored last, so a generation that has it has everything. */
function newest() {
  return caches.keys().then(function (keys) {
    var gens = keys.filter(function (k) { return k.indexOf(CACHE) === 0; }).sort().reverse();
    var i = 0;
    function next() {
      if (i >= gens.length) return NET;
      var k = gens[i++];
      return caches.match(PAGE, { cacheName: k }).then(function (r) { return r ? k : next(); });
    }
    return next();
  });
}

function store(set) {
  var name = CACHE + Date.now();
  return newest().then(function (prev) {
    return caches.open(name).then(function (c) {
      return Promise.all(set.filter(function (e) { return e[0] !== PAGE; })
                            .map(function (e) { return c.put(e[0], e[1]); }))
        .then(function () { return c.put(PAGE, set[0][1]); });
    }).then(function () { return prune([name, prev]); });
  });
}

/* Drops every generation but the ones to keep and those an open page
   was served from. */
function prune(keep) {
  return Promise.all([caches.keys(), self.clients.matchAll()]).then(function (r) {
    r[1].forEach(function (c) { if (pinned(c.id)) keep.push(pins[c.id]); });
    return Promise.all(r[0].filter(function (k) { return k.indexOf(CACHE) === 0 && keep.indexOf(k) < 0; })
                           .map(function (k) { return caches.delete(k); }));
  });
}

/* Whether a fresh set differs from the newest generation, by each
   file's validator, so an unchanged build is not stored again. */
function changed(set) {
  return newest().then(function (gen) {
    if (gen === NET) return true;
    return Promise.all(set.map(function (e) {
      return caches.match(e[0], { cacheName: gen }).then(function (old) {
        if (!old) return true;
        var tag = function (r) { return r.headers.get('etag') || r.headers.get('last-modified') || ''; };
        return !tag(old) || tag(old) !== tag(e[1]);
      });
    })).then(function (diff) { return diff.some(Boolean); });
  });
}

var refreshing = null;
function refresh() {
  if (!refreshing) {
    refreshing = fetchSet().then(function (set) {
      if (!set) return null;
      return changed(set).then(function (yes) { return yes ? store(set) : null; });
    }).catch(function () {}).then(function () { refreshing = null; });
  }
  return refreshing;
}

/* From one generation, or the network for NET or a file it lacks. */
function from(gen, key, req) {
  if (gen === NET) return fetch(req);
  return caches.match(key, { cacheName: gen }).then(function (hit) { return hit || fetch(req); });
}

self.addEventListener('fetch', function (e) {
  var req = e.request;
  if (req.method !== 'GET') return;
  var url = new URL(req.url);
  if (url.origin !== self.location.origin) return;
  var path = url.pathname.replace(/^.*\//, './');
  if (req.mode === 'navigate' && (path === './' || path === PAGE)) {
    /* The page from the newest whole generation, which the new page
       keeps for everything it asks for after. */
    var id = e.resultingClientId;
    e.respondWith(newest().then(function (gen) {
      if (gen === NET) return null;
      return caches.match(PAGE, { cacheName: gen }).then(function (hit) {
        if (hit && id) pins[id] = gen;
        return hit;
      });
    }).then(function (hit) {
      if (!hit && id) pins[id] = NET;
      return hit || fetch(req);
    }));
    e.waitUntil(refresh());
    return;
  }
  if (ENGINE.indexOf(path) < 0 && PICTURES.indexOf(path) < 0) return;
  /* A page this worker did not serve, or served before it restarted,
     takes the newest generation for both engine files alike. */
  var cid = e.clientId;
  e.respondWith((pinned(cid) ? Promise.resolve(pins[cid]) : newest()).then(function (gen) {
    if (pinned(cid)) gen = pins[cid];
    else if (cid && ENGINE.indexOf(path) >= 0) pins[cid] = gen;
    return from(gen, path, req);
  }));
});
