/*
 * sw.js -- the page and the engine from this browser's cache.
 *
 * A returning player gets the page, tak-re.js and tak-re.wasm from the
 * cache at once, so the menu is up in about a second and the game
 * starts with no network at all. The same three are fetched again in
 * the background and swapped in together only when all three arrived,
 * so a page never meets a script from one build and an engine from
 * another. The new build is the one the next visit gets.
 *
 * Everything else, relay.txt, the leaderboard and the game server,
 * goes to the network as if this were not here.
 */
'use strict';

var CACHE = 'ok-engine';
var PAGE = './index.html';
var ENGINE = ['./tak-re.js', './tak-re.wasm'];
var PICTURES = ['./scroll-pick.jpg', './scroll-ready.jpg', './favicon.png'];

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

/* Into a new cache, then made the one in use, so a half written set is
   never read. */
function store(set) {
  var next = CACHE + '-' + Date.now();
  return caches.open(next).then(function (c) {
    return Promise.all(set.map(function (e) { return c.put(e[0], e[1]); }));
  }).then(function () {
    return caches.keys();
  }).then(function (keys) {
    return Promise.all(keys.filter(function (k) { return k.indexOf(CACHE) === 0 && k !== next; })
                           .map(function (k) { return caches.delete(k); }));
  });
}

function cached(u) {
  return caches.keys().then(function (keys) {
    var mine = keys.filter(function (k) { return k.indexOf(CACHE) === 0; }).sort();
    if (!mine.length) return null;
    return caches.open(mine[mine.length - 1]).then(function (c) { return c.match(u); });
  });
}

/* Whether a fresh set differs from the one in the cache, by each
   file's validator, so an unchanged build is not stored again. */
function changed(set) {
  return Promise.all(set.map(function (e) {
    return cached(e[0]).then(function (old) {
      if (!old) return true;
      var tag = function (r) { return r.headers.get('etag') || r.headers.get('last-modified') || ''; };
      return !tag(old) || tag(old) !== tag(e[1]);
    });
  })).then(function (diff) { return diff.some(Boolean); });
}

var refreshing = null;
function refreshSoon() {
  if (refreshing) return;
  refreshing = fetchSet().then(function (set) {
    if (!set) return null;
    return changed(set).then(function (yes) { return yes ? store(set) : null; });
  }).catch(function () {}).then(function () { refreshing = null; });
}

self.addEventListener('fetch', function (e) {
  var req = e.request;
  if (req.method !== 'GET') return;
  var url = new URL(req.url);
  if (url.origin !== self.location.origin) return;
  var path = url.pathname.replace(/^.*\//, './');
  var key = null;
  if (req.mode === 'navigate' && (path === './' || path === PAGE)) key = PAGE;
  else if (ENGINE.indexOf(path) >= 0 || PICTURES.indexOf(path) >= 0) key = path;
  if (!key) return;
  e.respondWith(cached(key).then(function (hit) {
    if (key === PAGE) refreshSoon();
    return hit || fetch(req);
  }));
});
