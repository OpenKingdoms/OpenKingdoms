/*
 * atlas-app.js -- the Asset Atlas page: where the visitor's files come
 * from, the index kept in this browser, the package tree, the grid, the
 * preview and the notes. The decoding happens in atlas-worker.js and the
 * rules that need no page live in atlas.js.
 */
(function () {
  'use strict';
  var A = window.OKAtlas, G = window.OKAtlas3D;
  var THUMB_V = 1;
  var STORE = 'game';
  var CONTRIB_SOD = 'https://github.com/' + A.SOD_REPO + '/blob/main/docs/CONTRIBUTING-MODELS.md';
  var CUSTOM_MODELS = 'https://github.com/' + A.ENGINE_REPO + '/blob/main/docs/CUSTOM_MODELS.md';
  var MODDING = 'https://github.com/' + A.ENGINE_REPO + '/blob/main/docs/MODDING.md';

  function $(id) { return document.getElementById(id); }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
  }
  function svg(path, size) {
    var s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    s.setAttribute('width', size || 16); s.setAttribute('height', size || 16); s.setAttribute('viewBox', '0 0 24 24');
    s.setAttribute('aria-hidden', 'true'); s.innerHTML = path;
    return s;
  }
  var narrow = window.matchMedia('(max-width: 760px)');

  /* ── prefs ────────────────────────────────────────────────────────── */

  var prefs = {
    get: function (k, d) { try { var v = localStorage.getItem('ok.atlas.' + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
    set: function (k, v) { try { localStorage.setItem('ok.atlas.' + k, JSON.stringify(v)); } catch (e) {} }
  };

  var toastTimer = 0;
  function toast(msg, ms) {
    var t = $('toast');
    t.textContent = msg;
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, ms || 5000);
  }
  function status(msg) { $('st-main').textContent = msg; }
  function progress(frac) {
    var p = $('st-prog');
    p.hidden = frac === null;
    if (frac !== null) p.firstChild.style.width = Math.round(Math.max(0, Math.min(1, frac)) * 100) + '%';
  }

  /* ── theme ────────────────────────────────────────────────────────── */

  var SUN = '<circle cx="12" cy="12" r="4.5" fill="currentColor"/><path d="M12 2v2.5M12 19.5V22M2 12h2.5M19.5 12H22M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8" stroke="currentColor" stroke-width="2"/>';
  var MOON = '<path d="M20 14.5A8.5 8.5 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5z" fill="currentColor"/>';
  var AUTO = '<circle cx="12" cy="12" r="8.5" fill="none" stroke="currentColor" stroke-width="2"/><path d="M12 3.5a8.5 8.5 0 0 1 0 17z" fill="currentColor"/>';
  function theme(next) {
    var t = next || prefs.get('theme', 'auto');
    if (t === 'light' || t === 'dark') document.documentElement.setAttribute('data-theme', t);
    else document.documentElement.removeAttribute('data-theme');
    try { localStorage.setItem('ok.atlas.theme', t); } catch (e) {}
    var b = $('theme');
    b.textContent = '';
    b.appendChild(svg(t === 'light' ? SUN : t === 'dark' ? MOON : AUTO, 18));
    var label = t === 'auto' ? 'Theme follows the system' : t === 'light' ? 'Light theme' : 'Dark theme';
    b.title = label + '. Click to change.';
    b.setAttribute('aria-label', label);
    if (V.e) redrawSoon();
  }
  function isDark() {
    var t = document.documentElement.getAttribute('data-theme');
    return t ? t === 'dark' : window.matchMedia('(prefers-color-scheme: dark)').matches;
  }

  /* ── the database in this browser ─────────────────────────────────── */

  var db = null;
  function dbOpen() {
    return new Promise(function (res) {
      var r;
      try { r = indexedDB.open('ok-atlas', 1); } catch (e) { return res(null); }
      r.onupgradeneeded = function () {
        var d = r.result;
        ['pkgs', 'index', 'notes', 'thumbs', 'meta'].forEach(function (n) { if (!d.objectStoreNames.contains(n)) d.createObjectStore(n); });
      };
      r.onsuccess = function () { res(r.result); };
      r.onerror = r.onblocked = function () { res(null); };
    });
  }
  function req(store, mode, fn) {
    return new Promise(function (res) {
      if (!db) return res(undefined);
      var t;
      try { t = db.transaction(store, mode); } catch (e) { return res(undefined); }
      var r = fn(t.objectStore(store));
      t.oncomplete = function () { res(r && 'result' in r ? r.result : undefined); };
      t.onerror = t.onabort = function () { res(undefined); };
    });
  }
  var DB = {
    get: function (s, k) { return req(s, 'readonly', function (o) { return o.get(k); }); },
    put: function (s, k, v) { return req(s, 'readwrite', function (o) { o.put(v, k); }); },
    del: function (s, k) { return req(s, 'readwrite', function (o) { o.delete(k); }); },
    all: function (s) {
      return Promise.all([req(s, 'readonly', function (o) { return o.getAllKeys(); }), req(s, 'readonly', function (o) { return o.getAll(); })])
        .then(function (r) { return (r[0] || []).map(function (k, i) { return [k, r[1][i]]; }); });
    },
    many: function (s, keys) {
      return new Promise(function (res) {
        if (!db || !keys.length) return res(keys.map(function () { return undefined; }));
        var out = [], t = db.transaction(s, 'readonly'), o = t.objectStore(s);
        keys.forEach(function (k, i) { var r = o.get(k); r.onsuccess = function () { out[i] = r.result; }; });
        t.oncomplete = function () { res(out); };
        t.onerror = t.onabort = function () { res(keys.map(function () { return undefined; })); };
      });
    },
    putMany: function (s, pairs) { return req(s, 'readwrite', function (o) { pairs.forEach(function (p) { o.put(p[1], p[0]); }); }); },
    delPrefix: function (s, prefix) { return req(s, 'readwrite', function (o) { o.delete(IDBKeyRange.bound(prefix, prefix + '￿')); }); },
    clear: function (s) { return req(s, 'readwrite', function (o) { o.clear(); }); }
  };

  /* ── state ────────────────────────────────────────────────────────── */

  var S = {
    pkgs: new Map(),          /* key -> package record */
    indexes: new Map(),       /* key -> {entries, links} */
    entries: [], byId: new Map(),
    sod: null,
    book: null,
    notes: Object.create(null),
    community: Object.create(null),
    hay: new Map(),
    results: [],
    query: '',
    filters: { kind: '', source: '', remaster: '', rating: 0 },
    sort: prefs.get('sort', 'path'),
    scope: null,              /* {pkg, prefix, group} */
    sel: null,
    live: new Map(),          /* key -> mount record, files this visit */
    source: null,
    open: new Set(prefs.get('open', ['g:game', 'g:sod'])),
    cell: prefs.get('cell', 132),
    busy: false
  };

  /* ── the worker ───────────────────────────────────────────────────── */

  var worker = null, seq = 0, waiting = new Map();
  function startWorker() {
    worker = new Worker('atlas-worker.js');
    worker.onmessage = function (ev) {
      var m = ev.data;
      if (m.op === 'reply') {
        var w = waiting.get(m.id);
        waiting.delete(m.id);
        if (w) { if (m.error) w.rej(new Error(m.error)); else w.res(m.value); }
      } else if (m.op === 'progress') {
        indexProgress(m.key, m.done / Math.max(1, m.total));
      } else if (m.op === 'thumb') {
        gotThumb(m.uid, m.blob);
      }
    };
    worker.onerror = function (e) {
      e.preventDefault();
      restartWorker('The decoder stopped (' + (e.message || 'an error') + '). It has been started again.');
    };
  }
  function call(op, data, transfer) {
    return new Promise(function (res, rej) {
      var id = ++seq;
      waiting.set(id, { res: res, rej: rej });
      var msg = data || {};
      msg.op = op;
      msg.id = id;
      worker.postMessage(msg, transfer || []);
    });
  }
  function restartWorker(why) {
    try { worker.terminate(); } catch (e) {}
    waiting.forEach(function (w) { w.rej(new Error('the decoder restarted')); });
    waiting.clear();
    thumbAsked.last = '';
    startWorker();
    if (why) toast(why);
    var list = Array.from(S.live.values());
    if (list.length) mountAll(list).then(sendOrder);
  }

  /* ── where the files come from ────────────────────────────────────── */

  var WALK = { maps: 1, objects3d: 1, models3d: 1, mods: 1 };

  /* Whether a file under the game folder is one the atlas reads. */
  function wanted(rel) {
    var p = rel.split('/'), top = p[0].toLowerCase();
    if (p.length === 1) return A.isArchive(p[0]);
    if (top === 'maps') return p.length === 2 && /\.kmp$/i.test(p[1]);
    if (top === 'objects3d') return p.length === 2 && /\.3do$/i.test(p[1]);
    if (top === 'models3d') return p.length === 2 && /\.glb$/i.test(p[1]);
    if (top === 'mods') return p.length >= 3;
    return false;
  }

  function collect(iter, fn) {
    return iter.next().then(function (r) { if (r.done) return; fn(r.value[0], r.value[1]); return collect(iter, fn); });
  }

  /* Every wanted file under a directory handle: [{rel, file}]. */
  function walkHandle(dir) {
    var out = [];
    function walk(h, prefix, depth) {
      var subs = [], files = [];
      return collect(h.entries(), function (n, c) {
        var rel = prefix ? prefix + '/' + n : n;
        if (c.kind === 'file') { if (wanted(rel)) files.push(c.getFile().then(function (f) { out.push({ rel: rel, file: f }); })); }
        else if ((depth === 0 && WALK[n.toLowerCase()]) || (depth > 0 && prefix.toLowerCase().indexOf('mods') === 0 && depth < 8)) subs.push([c, rel]);
      }).then(function () { return Promise.all(files); }).then(function () {
        var i = 0;
        function next() { return i < subs.length ? walk(subs[i][0], subs[i][1], depth + 1).then(function () { i++; return next(); }) : null; }
        return next();
      });
    }
    return walk(dir, '', 0).then(function () { return out; });
  }

  /* The folder under dir, itself included, that holds data.hpi, looked for
     two folders deep as the game page looks. */
  function findGameDir(dir, depth) {
    var subs = [], here = false;
    return collect(dir.entries(), function (n, h) {
      if (h.kind === 'file' && n.toLowerCase() === 'data.hpi') here = true;
      else if (h.kind === 'directory') subs.push(h);
    }).then(function () {
      if (here) return dir;
      if (depth <= 0) return null;
      var i = 0;
      function next() { return i < subs.length ? findGameDir(subs[i++], depth - 1).then(function (r) { return r || next(); }) : null; }
      return next();
    });
  }

  /* Picked files with webkitRelativePath, rebased on the folder that holds
     data.hpi when there is one. {files, full}. */
  function fromFileList(list) {
    var items = [], best = null;
    for (var i = 0; i < list.length; i++) {
      var f = list[i].file || list[i], rel = (list[i].rel || f.webkitRelativePath || f.name).replace(/^\/+/, '');
      items.push({ rel: rel, file: f });
      var parts = rel.split('/');
      if (parts[parts.length - 1].toLowerCase() === 'data.hpi' && (!best || parts.length < best.length)) best = parts;
    }
    var prefix = best ? best.slice(0, -1).join('/') : '';
    var out = [];
    items.forEach(function (it) {
      var rel = it.rel;
      if (prefix) {
        if (rel.indexOf(prefix + '/') !== 0) return;
        rel = rel.slice(prefix.length + 1);
      } else {
        /* Not a game folder: an archive stands on its own, and loose
           files are a folder mod named after the folder they came in. */
        var name = A.baseName(rel);
        if (A.isArchive(name)) rel = /\.kmp$/i.test(name) ? 'Maps/' + name : name;
        else if (rel.indexOf('/') > 0) rel = 'Mods/' + rel;
        else rel = 'Mods/Dropped files/' + name;
      }
      if (wanted(rel)) out.push({ rel: rel, file: it.file });
    });
    return { files: out, full: !!best };
  }

  function opfsFiles() {
    if (!navigator.storage || !navigator.storage.getDirectory) return Promise.resolve(null);
    return navigator.storage.getDirectory()
      .then(function (root) { return root.getDirectoryHandle(STORE); })
      .then(walkHandle)
      .then(function (files) { return files.some(function (f) { return f.rel.indexOf('/') < 0; }) ? files : null; })
      .catch(function () { return null; });
  }

  /* ── packages ─────────────────────────────────────────────────────── */

  /* The packages a list of files makes: each archive one, and each folder
     of loose files one (objects3d, models3d, a folder mod). */
  function packagesFrom(files) {
    var out = new Map();
    files.forEach(function (f) {
      var parts = f.rel.split('/'), top = parts[0].toLowerCase(), info;
      if (A.isArchive(parts[parts.length - 1])) {
        info = A.packageInfo(f.rel);
        info.kind = 'archive';
        info.file = f.file;
        info.size = f.file.size;
        out.set(info.key, info);
        return;
      }
      var folder = top === 'mods' ? parts.slice(0, 2).join('/') : parts[0];
      var inner = f.rel.slice(folder.length + 1).toLowerCase();
      if (!A.classify(inner)) return;
      var key = folder.toLowerCase(), p = out.get(key);
      if (!p) {
        p = { rel: folder, key: key, name: A.baseName(folder), group: top === 'mods' ? 'mods' : 'loose', official: false,
              label: '', kind: 'folder', files: [], size: 0 };
        out.set(key, p);
      }
      p.files.push({ path: inner, file: f.file });
      p.size += f.file.size;
    });
    return out;
  }

  function stampOf(p) {
    if (p.kind === 'folder') {
      var list = p.files.map(function (f) { return f.path + ':' + f.file.size + ':' + f.file.lastModified; }).sort().join('|');
      return Promise.resolve('f' + p.size + '-' + A.crc32(new TextEncoder().encode(list)).toString(16));
    }
    var f = p.file, n = f.size;
    return Promise.all([f.slice(0, 4096).arrayBuffer(), f.slice(Math.max(0, n - 4096)).arrayBuffer()]).then(function (b) {
      return 'a' + n + '-' + A.crc32(new Uint8Array(b[0])).toString(16) + '-' + A.crc32(new Uint8Array(b[1])).toString(16);
    });
  }

  function mountAll(list) {
    return call('mount', {
      list: list.map(function (p) {
        return p.kind === 'archive' ? { key: p.key, name: p.name, kind: 'archive', file: p.file }
                                    : { key: p.key, name: p.name, kind: 'folder', files: p.files };
      })
    });
  }

  function sendOrder() {
    var base = A.mountOrder(Array.from(S.live.values()).filter(function (p) { return p.group === 'game' || p.group === 'maps'; })
      .map(function (p) { return p.rel; }));
    return call('order', { keys: base.map(function (r) { return r.toLowerCase(); }) }).then(function () {
      return call('texmap', { map: texmap() });
    });
  }

  /* Texture name to [package, path, entry], the game's own archives first
     in mount order, as the engine's texture atlas finds one. */
  function texmap() {
    var map = Object.create(null);
    var keys = A.mountOrder(Array.from(S.pkgs.values()).filter(function (p) { return p.group === 'game'; }).map(function (p) { return p.rel; }))
      .map(function (r) { return r.toLowerCase(); });
    keys.forEach(function (k) {
      var ix = S.indexes.get(k);
      if (!ix || !S.live.has(k)) return;
      ix.links.textures.forEach(function (t) { if (!map[t[0]]) map[t[0]] = [k, t[1], t[2]]; });
    });
    return map;
  }
  function ownTextures(pkgKey) {
    var p = S.pkgs.get(pkgKey), ix = S.indexes.get(pkgKey);
    if (!p || p.group === 'game' || !ix || !ix.links.textures.length) return null;
    var own = Object.create(null);
    ix.links.textures.forEach(function (t) { own[t[0]] = [pkgKey, t[1], t[2]]; });
    return own;
  }

  /* ── loading a source ─────────────────────────────────────────────── */

  /* Opens a set of files: mounts them, indexes what is new or changed and,
     when the set is a whole game folder, prunes what is no longer there. */
  function useSource(src) {
    if (S.busy) return Promise.resolve();
    S.busy = true;
    S.source = src;
    var found = packagesFrom(src.files);
    if (!found.size) {
      S.busy = false;
      toast('There are no archives or models in that folder. Pick the folder with data.hpi and terrain.hpi.');
      return Promise.resolve();
    }
    status('Opening ' + A.count(found.size, 'package') + '.');
    var list = Array.from(found.values());
    var gone = [];
    return Promise.all(list.map(stampOf)).then(function (stamps) {
      list.forEach(function (p, i) { p.stamp = stamps[i]; });
      var drop = S.live.size ? Array.from(S.live.keys()).filter(function (k) { return !found.has(k); }) : [];
      return (drop.length ? call('close', { keys: drop }) : Promise.resolve()).then(function () { return mountAll(list); });
    }).then(function (res) {
      var bad = res.filter(function (r) { return r.error; });
      bad.forEach(function (r) { found.delete(r.key); });
      if (bad.length) toast(A.count(bad.length, 'archive') + ' would not open: ' + bad.slice(0, 3).map(function (b) { return b.key; }).join(', ') + '.');
      S.live = new Map();
      found.forEach(function (p, k) { S.live.set(k, p); });
      if (src.full) {
        S.pkgs.forEach(function (p, k) { if (k !== A.SOD && !found.has(k)) gone.push(k); });
      }
      return Promise.all(gone.map(forgetPackage));
    }).then(function () {
      var todo = [];
      found.forEach(function (p, k) {
        var old = S.pkgs.get(k);
        if (!old || old.stamp !== p.stamp || old.v !== A.INDEX_VERSION || !S.indexes.has(k)) todo.push(p);
        else { old.file = p.file; old.files = p.files; }
      });
      rebuild();
      return sendOrder().then(function () { return indexAll(todo); });
    }).then(function () {
      S.busy = false;
      rebuild();
      return sendOrder();
    }).then(function () {
      var n = S.entries.length;
      status(A.count(n, 'asset') + ' in ' + A.count(S.pkgs.size, 'package') + (gone.length ? ', ' + A.count(gone.length, 'package') + ' no longer there and pruned' : '') + '.');
      progress(null);
      sourceNote();
    }).catch(function (e) {
      S.busy = false;
      progress(null);
      status('Could not read those files: ' + (e && e.message ? e.message : e));
    });
  }

  function forgetPackage(key) {
    S.pkgs.delete(key);
    S.indexes.delete(key);
    S.live.delete(key);
    return Promise.all([DB.del('pkgs', key), DB.del('index', key), DB.delPrefix('thumbs', THUMB_V + ':' + key + '|')]);
  }

  var indexing = null;
  function indexProgress(key, frac) {
    if (!indexing) return;
    progress((indexing.done + frac) / indexing.total);
    var row = treeRows.get('p:' + key);
    if (row) { row.classList.add('busy'); var b = row.querySelector('.bar') || row.appendChild(el('i', 'bar')); b.style.width = Math.round(frac * 100) + '%'; }
  }

  function indexAll(todo) {
    if (!todo.length) return Promise.resolve();
    todo = A.mountOrder(todo.map(function (p) { return p.rel; })).map(function (r) { return todo.filter(function (p) { return p.rel === r; })[0]; });
    indexing = { done: 0, total: todo.length };
    var i = 0, lastBuild = 0;
    function next() {
      if (i >= todo.length) { indexing = null; return Promise.resolve(); }
      var p = todo[i++];
      status('Indexing ' + p.rel + ' (' + i + ' of ' + todo.length + ').');
      return call('index', { key: p.key, name: p.name.toLowerCase() }).then(function (r) {
        var rec = { key: p.key, rel: p.rel, name: p.name, group: p.group, official: p.official, label: p.label, kind: p.kind,
                    size: p.size, stamp: p.stamp, v: A.INDEX_VERSION, count: r.entries.length, files: r.files, at: Date.now() };
        var ix = { entries: r.entries, links: r.links };
        S.indexes.set(p.key, ix);
        var stored = Object.assign({}, rec);
        S.pkgs.set(p.key, Object.assign(rec, { file: p.file, files: p.files }));
        return Promise.all([DB.put('pkgs', p.key, stored), DB.put('index', p.key, ix), DB.delPrefix('thumbs', THUMB_V + ':' + p.key + '|')]);
      }, function (e) {
        toast(p.rel + ' could not be indexed: ' + e.message);
      }).then(function () {
        indexing.done++;
        var row = treeRows.get('p:' + p.key);
        if (row) row.classList.remove('busy');
        if (Date.now() - lastBuild > 1500 || i >= todo.length) { lastBuild = Date.now(); rebuild(); }
        return next();
      });
    }
    return next();
  }

  /* ── derived data ─────────────────────────────────────────────────── */

  function rebuild() {
    var all = [], links = [];
    A.displayOrder(Array.from(S.pkgs.values())).forEach(function (p) {
      var ix = S.indexes.get(p.key);
      if (!ix) return;
      Array.prototype.push.apply(all, ix.entries);
      links.push(ix.links);
    });
    var sodEntries = S.sod ? S.sod.entries : [];
    S.book = A.nameBook(links);
    A.linkRemasters(all, S.book, sodEntries);
    S.entries = all.concat(sodEntries);
    S.byId = new Map();
    S.entries.forEach(function (e) { S.byId.set(e.id, e); });
    S.hay = new Map();
    thumbMiss.clear();
    dbChecked.clear();
    renderTree();
    refresh(true);
    if (S.sel && S.byId.has(S.sel)) renderInfo();
  }

  function pkgOf(key) { return key === A.SOD ? SOD_PKG : S.pkgs.get(key); }
  var SOD_PKG = { key: A.SOD, rel: 'Stars of Darien', name: 'stars-of-darien', group: 'sod', official: false, label: 'Remaster' };
  function pkgName(key) { var p = pkgOf(key); return p ? p.name : key; }
  function hay(e) {
    var h = S.hay.get(e.id);
    if (h === undefined) { h = A.haystack(e, pkgName(e.pkg), S.notes[e.nk], S.community[e.nk]); S.hay.set(e.id, h); }
    return h;
  }
  function rating(e) {
    var n = S.notes[e.nk], c = S.community[e.nk];
    return (n && n.rating) || (c && c.rating) || 0;
  }

  /* ── search, filters, scope ───────────────────────────────────────── */

  function refresh(keep) {
    var f = { kind: S.filters.kind, source: S.filters.source, remaster: S.filters.remaster, rating: S.filters.rating, pkgs: null };
    var sc = S.scope;
    if (sc && sc.pkg) { f.pkgs = {}; f.pkgs[sc.pkg] = 1; }
    else if (sc && sc.group) {
      f.pkgs = {};
      if (sc.group === 'sod') f.pkgs[A.SOD] = 1;
      S.pkgs.forEach(function (p, k) { if (p.group === sc.group) f.pkgs[k] = 1; });
    }
    var list = A.search(S.entries, S.query, f, hay, rating, pkgOf);
    if (sc && sc.prefix) {
      var pre = sc.prefix, exact = sc.file;
      list = list.filter(function (e) { return exact ? e.path === pre : e.path.indexOf(pre) === 0; });
    }
    if (S.sort !== 'path') list = list.slice().sort(A.SORTS[S.sort]);
    S.results = list;
    if (!keep) gridScrollTop(0);
    renderScope();
    renderGrid(true);
    welcome();
  }

  function setScope(sc) {
    S.scope = sc;
    treeRows.forEach(function (row, id) { row.classList.toggle('on', !!sc && id === scopeId(sc)); });
    refresh();
    if (narrow.matches) tab('center');
  }
  function scopeId(sc) {
    if (!sc) return 'all';
    if (sc.group) return 'g:' + sc.group;
    return 'p:' + sc.pkg + (sc.prefix ? '/' + sc.prefix : '');
  }

  function renderScope() {
    var box = $('scope');
    box.textContent = '';
    var sc = S.scope, crumb = el('span', 'crumb');
    if (!sc) crumb.textContent = S.query ? 'Search' : 'All assets';
    else if (sc.group) crumb.textContent = GROUPS[sc.group];
    else {
      var p = pkgOf(sc.pkg);
      crumb.textContent = (p ? (p.group === 'sod' ? 'Stars of Darien' : p.rel) : sc.pkg) + (sc.prefix ? ' › ' + sc.prefix.replace(/\/$/, '').split('/').join(' › ') : '');
    }
    box.appendChild(crumb);
    box.appendChild(el('span', 'n', A.count(S.results.length, 'asset')));
    if (sc && sc.pkg) {
      var about = el('button', 'linkish', sc.file ? 'About this file' : 'About this package');
      about.type = 'button';
      about.onclick = function () { showPackage(sc.pkg, sc.file ? sc.prefix : ''); };
      box.appendChild(about);
    }
    if (S.query || S.filters.kind || S.filters.source || S.filters.remaster || S.filters.rating || sc) {
      var clear = el('button', 'linkish', 'Show everything');
      clear.type = 'button';
      clear.onclick = function () {
        S.query = ''; $('q').value = '';
        S.filters = { kind: '', source: '', remaster: '', rating: 0 };
        $('f-rating').value = '0';
        document.querySelectorAll('#filters .seg').forEach(function (g) {
          g.querySelectorAll('button').forEach(function (b) { b.setAttribute('aria-pressed', b.dataset.v === '' ? 'true' : 'false'); });
        });
        setScope(null);
      };
      box.appendChild(clear);
    }
  }

  /* ── the package tree ─────────────────────────────────────────────── */

  var GROUPS = { game: 'The game', maps: 'Map packs', mods: 'Mods', loose: 'Your loose files', sod: 'Stars of Darien', custom: 'Other archives' };
  var treeRows = new Map();

  function countIn(key, prefix) {
    var ix = key === A.SOD ? { entries: S.sod ? S.sod.entries : [] } : S.indexes.get(key);
    if (!ix) return 0;
    if (!prefix) return ix.entries.length;
    var n = 0;
    ix.entries.forEach(function (e) { if (e.path.indexOf(prefix) === 0) n++; });
    return n;
  }

  function row(id, label, opts) {
    var r = el('div', 'row' + (opts.group ? ' group' : '') + (opts.gone ? ' gone' : ''));
    r.setAttribute('role', 'treeitem');
    r.tabIndex = -1;
    var tw = el('span', 'tw', opts.kids ? '▶' : '');
    r.appendChild(tw);
    var nm = el('span', 'nm', label);
    r.appendChild(nm);
    if (opts.title) r.title = opts.title;
    if (opts.chip) r.appendChild(el('span', 'chip' + (opts.custom ? ' custom' : ''), opts.chip));
    if (opts.star) r.appendChild(el('span', 'star', '★' + opts.star));
    if (opts.count !== undefined) r.appendChild(el('span', 'ct', opts.count.toLocaleString('en-US')));
    if (S.scope ? scopeId(S.scope) === id : id === 'all') r.classList.add('on');
    treeRows.set(id, r);
    var kids = null;
    if (opts.kids) {
      kids = el('div', 'kids');
      kids.setAttribute('role', 'group');
      var open = S.open.has(id);
      r.setAttribute('aria-expanded', open ? 'true' : 'false');
      if (open) { r.classList.add('open'); opts.kids(kids); } else kids.hidden = true;
      tw.onclick = function (ev) {
        ev.stopPropagation();
        var now = !S.open.has(id);
        if (now) S.open.add(id); else S.open.delete(id);
        prefs.set('open', Array.from(S.open));
        r.classList.toggle('open', now);
        r.setAttribute('aria-expanded', now ? 'true' : 'false');
        if (now && !kids.childNodes.length) opts.kids(kids);
        kids.hidden = !now;
      };
    }
    r.onclick = function () { setScope(opts.scope || null); };
    r.ondblclick = function () { if (tw.onclick) tw.onclick(new Event('click')); };
    return { row: r, kids: kids };
  }

  function addRow(parent, r) { parent.appendChild(r.row); if (r.kids) parent.appendChild(r.kids); }

  function folderKids(pkgKey, prefix) {
    return function (box) {
      var ix = pkgKey === A.SOD ? { entries: S.sod.entries } : S.indexes.get(pkgKey);
      if (!ix) return;
      var dirs = new Map(), files = new Map();
      ix.entries.forEach(function (e) {
        if (e.path.indexOf(prefix) !== 0) return;
        var rest = e.path.slice(prefix.length), slash = rest.indexOf('/');
        if (slash >= 0) { var d = rest.slice(0, slash + 1); dirs.set(d, (dirs.get(d) || 0) + 1); }
        else if (e.seq) files.set(rest, (files.get(rest) || 0) + 1);
      });
      Array.from(dirs.keys()).sort().forEach(function (d) {
        var sub = prefix + d;
        addRow(box, row('p:' + pkgKey + '/' + sub, d.slice(0, -1), { count: dirs.get(d), kids: folderKids(pkgKey, sub),
          scope: { pkg: pkgKey, prefix: sub } }));
      });
      Array.from(files.keys()).sort().forEach(function (f) {
        var path = prefix + f, n = S.notes[A.noteKey(pkgName(pkgKey), path)];
        addRow(box, row('p:' + pkgKey + '/' + path, f, { count: files.get(f), star: n && n.rating, title: 'A GAF file of ' + files.get(f) + ' sequences',
          scope: { pkg: pkgKey, prefix: path, file: true } }));
      });
    };
  }

  function renderTree() {
    var tree = $('tree');
    var top = tree.scrollTop;
    tree.textContent = '';
    treeRows = new Map();
    addRow(tree, row('all', 'All assets', { count: S.entries.length, scope: null }));
    var groups = { game: [], maps: [], mods: [], loose: [], custom: [] };
    S.pkgs.forEach(function (p) { (groups[p.group] || groups.custom).push(p); });
    ['game', 'maps', 'mods', 'loose', 'custom'].forEach(function (g) {
      /* A package with no pictures or models in it has nothing to show. */
      var list = A.displayOrder(groups[g]).filter(function (p) { return countIn(p.key) > 0; });
      if (!list.length) return;
      var total = 0;
      list.forEach(function (p) { total += countIn(p.key); });
      addRow(tree, row('g:' + g, GROUPS[g], { group: true, count: total, scope: { group: g }, kids: function (box) {
        list.forEach(function (p) {
          var n = S.notes[A.noteKey(p.name, '')];
          addRow(box, row('p:' + p.key, g === 'maps' ? A.stem(p.name) : p.name, {
            count: countIn(p.key), chip: p.official ? p.label : g === 'loose' ? '' : 'custom', custom: !p.official, star: n && n.rating,
            gone: !S.live.has(p.key), title: p.rel + (S.live.has(p.key) ? '' : ' (not loaded this visit)'),
            scope: { pkg: p.key }, kids: S.indexes.get(p.key) && S.indexes.get(p.key).entries.length ? folderKids(p.key, '') : null }));
        });
      } }));
    });
    if (S.sod) {
      addRow(tree, row('g:sod', 'Stars of Darien', { group: true, count: S.sod.entries.length, scope: { group: 'sod' },
        title: 'The remaster’s models at commit ' + S.sod.commit.slice(0, 10), kids: folderKids(A.SOD, '') }));
    }
    tree.scrollTop = top;
  }

  function sourceNote() {
    var box = $('source-note');
    box.textContent = '';
    var src = S.source;
    var line = el('div');
    if (src && src.kind === 'opfs') line.textContent = 'Reading the game files kept in this browser.';
    else if (src) line.textContent = 'Reading ' + (src.name ? src.name : 'the files you picked') + ' for this visit.';
    else if (S.pkgs.size) line.textContent = 'Your game files are not loaded, so only pictures made before show. Load them to see the rest.';
    else line.textContent = 'No game files loaded yet.';
    box.appendChild(line);
    var b = el('button', 'btn quiet small', src ? 'Load another folder' : 'Load your game folder');
    b.type = 'button';
    b.onclick = pickFolder;
    box.appendChild(b);
  }

  /* ── the grid ─────────────────────────────────────────────────────── */

  var grid, sizer, cells = new Map(), layout = { cols: 1, w: 132, rowH: 176 };

  function gridScrollTop(v) { if (grid) grid.scrollTop = v; }
  function measure() {
    var inner = grid.clientWidth - 16;
    var cols = Math.max(1, Math.floor(inner / S.cell));
    var w = inner / cols;
    layout = { cols: cols, w: w, rowH: Math.round(w - 8 + 50) };
  }

  function renderGrid(reset) {
    if (!grid) return;
    if (reset) { cells.forEach(function (c) { c.remove(); }); cells.clear(); }
    measure();
    var win = A.gridWindow(grid.scrollTop, grid.clientHeight, layout.rowH, layout.cols, S.results.length, 2);
    sizer.style.height = (win.total + 8) + 'px';
    var keep = new Set();
    for (var i = win.first; i <= win.last; i++) {
      var e = S.results[i];
      keep.add(i);
      var c = cells.get(i);
      if (!c || c.dataset.id !== e.id) {
        if (c) c.remove();
        c = makeCell(e, i);
        cells.set(i, c);
        grid.appendChild(c);
      }
      c.style.left = (8 + (i % layout.cols) * layout.w) + 'px';
      c.style.top = (Math.floor(i / layout.cols) * layout.rowH + 4) + 'px';
      c.style.width = (layout.w - 8) + 'px';
      c.classList.toggle('on', e.id === S.sel);
      c.setAttribute('aria-selected', e.id === S.sel ? 'true' : 'false');
    }
    cells.forEach(function (c, i) { if (!keep.has(i)) { c.remove(); cells.delete(i); } });
    var empty = grid.querySelector('.empty-note');
    if (!S.results.length && S.entries.length) {
      if (!empty) { empty = el('div', 'empty-note'); grid.appendChild(empty); }
      empty.textContent = S.query ? 'Nothing matches “' + S.query + '” here.' : 'Nothing here matches the filters.';
    } else if (empty) empty.remove();
    flushWants();
  }

  function subline(e) {
    if (e.pkg === A.SOD) return (e.sod === 'card' ? 'Unit card' : e.sod === 'unit' ? 'Unit' : 'Scenery') + ' · ' + A.count(e.tris, 'triangle');
    if (e.k === '3do') return A.count(e.tris || 0, 'triangle') + (e.title ? ' · ' + e.title : '');
    if (e.k === 'glb') return A.count(e.tris || 0, 'triangle');
    return e.w + '×' + e.h + (e.f > 1 ? ' · ' + e.f + ' frames' : '') + (e.title ? ' · ' + e.title : '');
  }

  function makeCell(e, i) {
    var c = el('div', 'cell');
    c.dataset.id = e.id;
    c.id = 'cell-' + i;
    c.setAttribute('role', 'option');
    c.title = pkgName(e.pkg) + ' / ' + e.path + (e.seq ? ' # ' + e.seq : '') + (e.title ? '\n' + e.title : '');
    var th = el('div', 'thumb');
    var badges = el('div', 'badges');
    if (e.f > 1) badges.appendChild(el('span', 'badge', '×' + e.f));
    if (A.isModel(e) && e.pkg !== A.SOD) badges.appendChild(el('span', 'badge', '3D'));
    if (e.rm) badges.appendChild(el('span', 'badge rm', 'Remaster'));
    else if (e.pkg === A.SOD && e.orig && e.orig.length) badges.appendChild(el('span', 'badge rm', 'Original'));
    else if (e.rmable && S.filters.remaster === 'needs') badges.appendChild(el('span', 'badge need', 'Needs one'));
    var r = rating(e);
    if (r) badges.appendChild(el('span', 'badge r', '★' + r));
    th.appendChild(badges);
    c.appendChild(th);
    var cap = el('div', 'cap');
    cap.appendChild(el('div', 'a', e.seq || e.name));
    cap.appendChild(el('div', 'b', subline(e)));
    c.appendChild(cap);
    fillThumb(c, e);
    c.onclick = function () { select(e.id); grid.focus({ preventScroll: true }); };
    c.ondblclick = function () { select(e.id); full(true); };
    return c;
  }

  /* ── thumbnails ───────────────────────────────────────────────────── */

  var thumbUrl = new Map(), thumbMiss = new Set(), thumbAsked = { last: '' }, dbChecked = new Set(), wantTick = 0;

  function fillThumb(c, e) {
    var th = c.firstChild, url = thumbUrl.get(e.id);
    var old = th.querySelector('img, .ph');
    if (old) old.remove();
    th.classList.remove('loading');
    if (url) {
      var img = el('img');
      img.alt = '';
      img.src = url;
      img.decoding = 'async';
      if (A.isModel(e) || e.k === 'img' && e.fmt === 'jpg') img.className = 'smooth';
      th.insertBefore(img, th.firstChild);
    } else {
      th.insertBefore(el('span', 'ph', e.pkg === A.SOD ? 'GLB' : A.isModel(e) ? '3DO' : (e.fmt && e.k === 'img' ? e.fmt : e.k).toUpperCase()), th.firstChild);
      if (!thumbMiss.has(e.id)) th.classList.add('loading');
    }
  }

  /* Pictures for the cells on screen: from this browser's store first,
     then from the worker, which drops the list it was on for this one. */
  function flushWants() {
    if (wantTick) return;
    wantTick = requestAnimationFrame(function () {
      wantTick = 0;
      var shown = [];
      cells.forEach(function (c) { var e = S.byId.get(c.dataset.id); if (e && !thumbUrl.has(e.id) && !thumbMiss.has(e.id)) shown.push(e); });
      var look = shown.filter(function (e) { return !dbChecked.has(e.id); });
      look.forEach(function (e) { dbChecked.add(e.id); });
      DB.many('thumbs', look.map(function (e) { return THUMB_V + ':' + e.id; })).then(function (blobs) {
        look.forEach(function (e, i) { if (blobs[i]) setThumb(e.id, URL.createObjectURL(blobs[i])); });
        var ask = [];
        shown.forEach(function (e) {
          if (thumbUrl.has(e.id) || thumbMiss.has(e.id)) return;
          if (!S.live.has(e.pkg) || e.pkg === A.SOD) { missThumb(e.id); return; }
          var names = A.namesOf(e, S.book), pal = A.paletteChoice(e, names);
          ask.push({ uid: e.id, pkg: e.pkg, path: e.path, k: e.k, ei: e.ei, fmt: e.fmt, pal: pal ? pal.spec : null,
                     flat: A.flatPalette(e), own: e.k === '3do' ? ownTextures(e.pkg) : null });
        });
        var key = ask.map(function (r) { return r.uid; }).join('\n');
        if (ask.length && key !== thumbAsked.last) { thumbAsked.last = key; worker.postMessage({ op: 'thumbs', list: ask }); }
      });
    });
  }

  function gotThumb(uid, blob) {
    if (!blob) { missThumb(uid); return; }
    DB.put('thumbs', THUMB_V + ':' + uid, blob);
    setThumb(uid, URL.createObjectURL(blob));
  }
  function missThumb(uid) {
    thumbMiss.add(uid);
    cells.forEach(function (c) { if (c.dataset.id === uid) c.firstChild.classList.remove('loading'); });
  }
  function setThumb(uid, url) {
    var old = thumbUrl.get(uid);
    if (old) URL.revokeObjectURL(old);
    thumbUrl.set(uid, url);
    thumbMiss.delete(uid);
    if (thumbUrl.size > 4000) {
      var first = thumbUrl.keys().next().value;
      URL.revokeObjectURL(thumbUrl.get(first));
      thumbUrl.delete(first);
    }
    var e = S.byId.get(uid);
    cells.forEach(function (c) { if (c.dataset.id === uid && e) fillThumb(c, e); });
  }

  /* A remaster model's thumbnail, drawn once it has been fetched. */
  var thumbR = null, thumbCanvas = null;
  function remasterThumb(e, model) {
    if (thumbUrl.has(e.id)) return;
    if (!thumbCanvas) {
      thumbCanvas = document.createElement('canvas');
      thumbCanvas.width = thumbCanvas.height = 192;
      try { thumbR = G.renderer(thumbCanvas, { preserve: true }); } catch (err) { thumbR = null; }
    }
    if (!thumbR) return;
    thumbR.view.yaw = -0.4; thumbR.view.pitch = 0.55; thumbR.view.zoom = 1.15;
    thumbR.draw([model], { grid: false, background: [0.16, 0.16, 0.17] });
    thumbCanvas.toBlob(function (b) {
      if (!b) return;
      DB.put('thumbs', THUMB_V + ':' + e.id, b);
      setThumb(e.id, URL.createObjectURL(b));
    }, 'image/webp', 0.86);
  }

  /* ── selection ────────────────────────────────────────────────────── */

  function select(id, keepTab) {
    var e = S.byId.get(id);
    if (!e) return;
    S.sel = id;
    S.pkgInfo = null;
    cells.forEach(function (c) {
      var on = c.dataset.id === id;
      c.classList.toggle('on', on);
      c.setAttribute('aria-selected', on ? 'true' : 'false');
      if (on) grid.setAttribute('aria-activedescendant', c.id);
    });
    if ($('app').classList.contains('no-detail')) layoutToggle('detail', true);
    preview(e);
    renderInfo();
    if (narrow.matches && !keepTab) tab('detail');
  }

  function moveSel(d) {
    var i = S.results.findIndex(function (e) { return e.id === S.sel; });
    var n = i < 0 ? 0 : Math.max(0, Math.min(S.results.length - 1, i + d));
    if (!S.results[n]) return;
    select(S.results[n].id, true);
    var top = Math.floor(n / layout.cols) * layout.rowH;
    if (top < grid.scrollTop) grid.scrollTop = top;
    else if (top + layout.rowH > grid.scrollTop + grid.clientHeight) grid.scrollTop = top + layout.rowH - grid.clientHeight;
  }

  /* ── the preview ──────────────────────────────────────────────────── */

  var V = { e: null, token: 0, mode: '', frames: [], frame: 0, playing: false, timer: 0, zoom: 1, fit: true, px: 0, py: 0,
            img: null, pal: null, shade: false, colour: 0, tiles: 1, spin: prefs.get('spin', true), model: null, pair: null,
            r: null, raf: 0, bg: prefs.get('bg', 'check'), lit: true };

  function msg(text) {
    var m = $('v-msg');
    m.hidden = !text;
    m.firstChild.textContent = text || '';
  }

  function setModes(list, current) {
    var box = $('mode');
    box.textContent = '';
    list.forEach(function (m) {
      var b = el('button', '', m[1]);
      b.type = 'button';
      b.dataset.v = m[0];
      b.setAttribute('aria-pressed', m[0] === current ? 'true' : 'false');
      b.onclick = function () { V.mode = m[0]; setModes(list, m[0]); showMode(); };
      box.appendChild(b);
    });
    $('vt-mode').hidden = list.length < 2;
  }

  function preview(e) {
    var token = ++V.token;
    stopPlay();
    V.e = e;
    V.frames = [];
    V.raw = [];
    V.frame = 0;
    V.img = null;
    V.plane = null;
    V.pal = null;
    V.pairLoading = 0;
    V.colour = 0;
    V.fit = true;
    V.px = V.py = 0;
    if (V.r) { V.r.release(V.model); V.r.release(V.pair && V.pair[1]); }
    V.model = null;
    V.pair = null;
    msg('');
    $('frames').hidden = true;
    $('vlabels').hidden = true;
    var live = S.live.has(e.pkg) || e.pkg === A.SOD;
    if (A.isTexture(e)) {
      setModes([['2d', 'Picture'], ['plane', 'On a plane']].concat(e.rm ? [['pair', 'Side by side']] : []), '2d');
      V.mode = '2d';
      if (!live) { showMode(); msg('Load your game folder to see this picture.'); return; }
      loadFrame(0, token).then(function () { if (token === V.token) { showMode(); framesBar(); } });
    } else {
      var modes = [['3d', 'Model']];
      if (e.rm || (e.pkg === A.SOD && e.orig && e.orig.length)) modes.push(['pair', 'Side by side']);
      V.mode = prefs.get('pairFirst', true) && modes.length > 1 ? 'pair' : '3d';
      setModes(modes, V.mode);
      showMode();
      if (!live) { msg('Load your game folder to see this model.'); return; }
      msg('Reading the model.');
      loadModel(e).then(function (m) {
        if (token !== V.token) return;
        V.model = m;
        if (e.pkg === A.SOD) remasterThumb(e, m);
        msg('');
        showMode();
      }, function (err) { if (token === V.token) msg(err.message); });
    }
  }

  function palSpec(e) {
    if (V.pal) return V.pal;
    var c = A.paletteChoice(e, A.namesOf(e, S.book));
    var spec = c ? Object.assign({}, c.spec) : { rule: 0 };
    if (V.shade) spec.shade = 15;
    return spec;
  }

  /* One frame of the selected texture as an ImageBitmap, kept for playback. */
  function loadFrame(i, token) {
    var e = V.e;
    if (V.frames[i]) { V.frame = i; V.img = V.frames[i]; return Promise.resolve(); }
    var p = e.k === 'img' ? call('picture', { pkg: e.pkg, path: e.path, fmt: e.fmt })
                          : call('frame', { pkg: e.pkg, path: e.path, ei: e.ei, frame: i, pal: palSpec(e) });
    return p.then(function (f) {
      if (token !== V.token) return;
      if (!f) { msg('This picture would not decode.'); return; }
      V.palName = f.palette || '';
      var data = new ImageData(new Uint8ClampedArray(f.rgba.buffer), f.w, f.h);
      return createImageBitmap(data).then(function (bm) {
        if (token !== V.token) return;
        V.frames[i] = bm;
        V.frame = i;
        V.img = bm;
        V.raw = V.raw || [];
        V.raw[i] = f;
        updatePalName();
      });
    }, function (err) { if (token === V.token) msg(err.message); });
  }

  function framesBar() {
    var e = V.e, n = e.f || 1;
    $('frames').hidden = !(e.f > 1) || V.mode !== '2d' && V.mode !== 'plane';
    $('f-pos').max = n - 1;
    $('f-pos').value = V.frame;
    var fh = e.k !== 'img' && V.raw && V.raw[V.frame];
    $('f-info').textContent = (V.frame + 1) + ' of ' + n + (fh ? ' · ' + fh.w + '×' + fh.h + (fh.x || fh.y ? ' at ' + fh.x + ',' + fh.y : '') : '');
  }

  function goFrame(i) {
    var e = V.e;
    if (!e || !(e.f > 1)) return;
    i = ((i % e.f) + e.f) % e.f;
    var token = V.token;
    return loadFrame(i, token).then(function () { if (token === V.token) { framesBar(); drawNow(); } });
  }
  function stopPlay() { V.playing = false; clearTimeout(V.timer); $('f-play').setAttribute('aria-pressed', 'false'); $('f-play').innerHTML = '&#9654;'; }
  function play() {
    if (V.playing) { stopPlay(); return; }
    V.playing = true;
    $('f-play').setAttribute('aria-pressed', 'true');
    $('f-play').innerHTML = '&#10074;&#10074;';
    var tick = function () {
      if (!V.playing) return;
      var t0 = performance.now(), fps = +$('f-fps').value || 15;
      Promise.resolve(goFrame(V.frame + 1)).then(function () {
        V.timer = setTimeout(tick, Math.max(0, 1000 / fps - (performance.now() - t0)));
      });
    };
    tick();
  }

  function showMode() {
    var viewer = $('viewer'), d3 = V.mode !== '2d';
    viewer.classList.toggle('idle', !V.e);
    viewer.classList.toggle('d3', d3);
    viewer.classList.toggle('d2', !d3);
    $('vt-zoom').hidden = d3;
    $('vt-3d').hidden = !d3;
    $('tiles').hidden = V.mode !== 'plane';
    $('vlabels').hidden = V.mode !== 'pair';
    if (V.e) framesBar();
    if (V.mode === 'pair') loadPair();
    drawNow();
  }

  /* ── 2D ── */

  var c2d, ctx2d;
  function size(canvas) {
    var r = canvas.getBoundingClientRect(), dpr = Math.min(2, window.devicePixelRatio || 1);
    var w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    return dpr;
  }
  function draw2d() {
    var dpr = size(c2d);
    ctx2d.setTransform(1, 0, 0, 1, 0, 0);
    ctx2d.clearRect(0, 0, c2d.width, c2d.height);
    var img = V.img;
    if (!img) return;
    var W = c2d.width, H = c2d.height;
    if (V.fit) {
      V.zoom = Math.min((W - 24 * dpr) / img.width, (H - 24 * dpr) / img.height);
      if (V.zoom > 1) V.zoom = Math.max(1, Math.floor(V.zoom));
      V.zoom /= dpr;
      V.px = V.py = 0;
    }
    var z = V.zoom * dpr;
    ctx2d.imageSmoothingEnabled = z < 1;
    ctx2d.imageSmoothingQuality = 'high';
    var x = Math.round(W / 2 - img.width * z / 2 + V.px * dpr), y = Math.round(H / 2 - img.height * z / 2 + V.py * dpr);
    ctx2d.drawImage(img, x, y, Math.round(img.width * z), Math.round(img.height * z));
    var fh = V.raw && V.raw[V.frame];
    if (fh && V.e && V.e.k !== 'img' && V.showHot) {
      ctx2d.strokeStyle = 'rgba(201,162,74,.9)';
      ctx2d.strokeRect(x + 0.5, y + 0.5, Math.round(img.width * z), Math.round(img.height * z));
    }
  }
  function zoomBy(f, cx, cy) {
    if (!V.img) return;
    var old = V.zoom;
    V.fit = false;
    V.zoom = Math.max(0.05, Math.min(64, V.zoom * f));
    if (cx !== undefined) {
      var r = c2d.getBoundingClientRect(), mx = cx - r.left - r.width / 2, my = cy - r.top - r.height / 2;
      V.px = mx - (mx - V.px) * V.zoom / old;
      V.py = my - (my - V.py) * V.zoom / old;
    }
    drawNow();
  }

  /* ── 3D ── */

  var c3d;
  function renderer() {
    if (V.r && !V.r.lost()) return V.r;
    try { V.r = G.renderer(c3d); } catch (e) { V.r = null; }
    if (!V.r) msg('This browser has no WebGL, so the 3D view is not available.');
    return V.r;
  }
  function bgColour() {
    if (V.bg === 'light') return [0.93, 0.92, 0.9];
    if (V.bg === 'dark') return [0.07, 0.07, 0.08];
    return isDark() ? [0.11, 0.115, 0.13] : [0.86, 0.84, 0.8];
  }
  function draw3d() {
    var r = renderer();
    if (!r) return;
    size(c3d);
    var o = { background: bgColour(), gridColor: isDark() ? [0.24, 0.24, 0.27, 1] : [0.72, 0.69, 0.63, 1] };
    if (V.mode === 'plane') {
      if (!V.img) return;
      if (!V.plane || V.plane.img !== V.img || V.plane.tiles !== V.tiles) {
        if (V.plane) r.release(V.plane.model);
        V.plane = { img: V.img, tiles: V.tiles, model: G.planeModel(V.img.width, V.img.height, V.img, V.tiles) };
      }
      o.grid = false;
      r.draw([V.plane.model], o);
    } else if (V.mode === 'pair') {
      if (V.pair) r.draw(V.pair, o);
    } else if (V.model) {
      r.draw([V.model], o);
    }
  }

  function drawNow() {
    if (!V.e) return;
    if (V.mode === '2d') draw2d(); else draw3d();
  }
  var redrawQueued = false;
  function redrawSoon() {
    if (redrawQueued) return;
    redrawQueued = true;
    requestAnimationFrame(function () { redrawQueued = false; drawNow(); });
  }
  function spinLoop() {
    cancelAnimationFrame(V.raf);
    var last = performance.now();
    function f(t) {
      V.raf = requestAnimationFrame(f);
      var dt = Math.min(0.05, (t - last) / 1000);
      last = t;
      if (!V.e || V.mode === '2d' || !V.spin || dragging || document.hidden) return;
      if (!V.r) return;
      V.r.view.yaw += dt * 0.5;
      draw3d();
    }
    V.raf = requestAnimationFrame(f);
  }

  /* The selected model, ready to draw. */
  function loadModel(e) {
    if (e.pkg === A.SOD) return remasterModel(e);
    if (e.k === 'glb') {
      return call('raw', { pkg: e.pkg, path: e.path }).then(function (bytes) {
        if (!bytes) throw new Error('That model would not read.');
        var g = A.readGlb(bytes), s = 1;
        var x = g && g.json && (g.json.extras || {}).tak_scale;
        (g && g.json.nodes || []).forEach(function (n) { if (n.extras && n.extras.tak_scale) x = n.extras.tak_scale; });
        if (+x > 0) s = +x;
        return G.fromGlb(bytes, { scale: s, image: decodeImage });
      });
    }
    return call('model', { pkg: e.pkg, path: e.path, opts: { shade: V.lit, colour: V.colour, flatPalette: A.flatPalette(e), own: ownTextures(e.pkg) } })
      .then(function (d) {
        if (!d) throw new Error('This model would not read.');
        V.d3do = d;
        return G.from3do(d, d.textures, d.flat);
      });
  }

  function decodeImage(bytes, mime) { return createImageBitmap(new Blob([bytes], { type: mime })); }

  function fetchRemaster(e) {
    function get() {
      return fetch(e.url, { mode: 'cors', credentials: 'omit' }).then(function (r) {
        if (!r.ok) throw new Error('GitHub answered ' + r.status + ' for this model.');
        return r;
      });
    }
    var cached = window.caches ? caches.open('ok-atlas-remaster').then(function (c) {
      return c.match(e.url).then(function (hit) {
        return hit || get().then(function (r) { c.put(e.url, r.clone()).catch(function () {}); return r; });
      });
    }) : get();
    return cached.catch(get).then(function (r) { return r.arrayBuffer(); }).then(function (b) { return new Uint8Array(b); });
  }

  /* A Stars of Darien model, its painted materials made from the visitor's
     own pictures when the game files are loaded. */
  function remasterModel(e) {
    msg('Fetching the model from GitHub.');
    return fetchRemaster(e).then(function (bytes) {
      return G.fromGlb(bytes, { scale: 16, image: decodeImage, paint: paintFor });
    });
  }

  function paintFor(spec) {
    var src = A.paintSource(spec, S.book || A.nameBook([]));
    if (!src) return null;
    var hit = null;
    if (src.texture) {
      var t = texmap()[src.texture];
      if (t) hit = { pkg: t[0], path: t[1], ei: t[2] };
    } else {
      for (var i = 0; i < S.entries.length && !hit; i++) {
        var x = S.entries[i];
        if (x.k === 'gaf' && x.seq && x.seq.toLowerCase() === src.seq && A.stem(x.path) === src.file && S.live.has(x.pkg))
          hit = { pkg: x.pkg, path: x.path, ei: x.ei };
      }
    }
    if (!hit) return null;
    return call('paint', { pkg: hit.pkg, path: hit.path, ei: hit.ei, pcx: src.pcx, corner: src.corner }).then(function (img) {
      if (!img) return null;
      if (spec.size && (spec.size[0] !== img.w || spec.size[1] !== img.h)) return null;
      var p = G.paintPicture(img, spec);
      return createImageBitmap(new ImageData(new Uint8ClampedArray(p.rgba.buffer), p.w, p.h));
    });
  }

  /* Original and remaster, side by side under one camera. */
  function loadPair() {
    var e = V.e, token = V.token;
    if (V.pair || V.pairLoading === token) return;
    V.pairLoading = token;
    var orig = e.pkg === A.SOD ? S.byId.get((e.orig || []).filter(function (id) { var x = S.byId.get(id); return x && S.live.has(x.pkg); })[0] || e.orig[0]) : e;
    var sod = e.pkg === A.SOD ? e : S.byId.get(e.rm);
    if (!sod) return;
    var labels = $('vlabels').children;
    labels[0].textContent = orig ? 'Original · ' + (orig.seq || orig.name) : 'Original';
    labels[1].textContent = 'Stars of Darien · ' + sod.name;
    msg('Fetching the Stars of Darien model.');
    var left = !orig ? Promise.resolve(null) : orig === e && V.model ? Promise.resolve(V.model)
      : S.live.has(orig.pkg) ? (A.isModel(orig) ? loadModel(orig) : spriteCard(orig)) : Promise.resolve(null);
    var right = sod === e && V.model ? Promise.resolve(V.model) : remasterModel(sod);
    Promise.all([left, right]).then(function (m) {
      if (token !== V.token) return;
      V.pair = m;
      if (sod !== e) remasterThumb(sod, m[1]);
      msg(m[0] ? '' : 'Load your game folder to see the original beside it.');
      drawNow();
    }, function (err) { if (token === V.token) msg(err.message); });
  }

  /* A feature picture stood up on a card, so a flat feature can stand
     beside the model that replaces it. */
  function spriteCard(e) {
    return call('frame', { pkg: e.pkg, path: e.path, ei: e.ei, frame: 0, pal: palSpec(e) }).then(function (f) {
      if (!f) return null;
      return createImageBitmap(new ImageData(new Uint8ClampedArray(f.rgba.buffer), f.w, f.h)).then(function (bm) {
        var m = G.planeModel(f.w, f.h, bm, 1);
        m.groups[0].unlit = false;
        return m;
      });
    });
  }

  /* ── pointer handling on the viewer ── */

  var dragging = false;
  function viewerInput() {
    var viewer = $('viewer'), pts = new Map(), last = null, pinch = 0;
    viewer.addEventListener('pointerdown', function (ev) {
      if (ev.target.closest('.vt')) return;
      viewer.setPointerCapture(ev.pointerId);
      pts.set(ev.pointerId, { x: ev.clientX, y: ev.clientY });
      last = { x: ev.clientX, y: ev.clientY, pan: ev.button === 2 || ev.shiftKey };
      dragging = true;
      if (pts.size === 2) { var a = Array.from(pts.values()); pinch = Math.hypot(a[0].x - a[1].x, a[0].y - a[1].y); }
    });
    viewer.addEventListener('pointermove', function (ev) {
      if (!pts.has(ev.pointerId)) return;
      pts.set(ev.pointerId, { x: ev.clientX, y: ev.clientY });
      if (pts.size === 2) {
        var a = Array.from(pts.values()), d = Math.hypot(a[0].x - a[1].x, a[0].y - a[1].y);
        if (pinch) { if (V.mode === '2d') zoomBy(d / pinch); else if (V.r) { V.r.view.zoom = Math.max(0.2, Math.min(8, V.r.view.zoom * d / pinch)); draw3d(); } }
        pinch = d;
        return;
      }
      var dx = ev.clientX - last.x, dy = ev.clientY - last.y;
      last.x = ev.clientX; last.y = ev.clientY;
      if (V.mode === '2d') { V.fit = false; V.px += dx; V.py += dy; draw2d(); return; }
      if (!V.r) return;
      if (last.pan) { V.r.view.panX -= dx / 200; V.r.view.panY += dy / 200; }
      else { V.r.view.yaw -= dx * 0.01; V.r.view.pitch = Math.max(-1.45, Math.min(1.45, V.r.view.pitch + dy * 0.01)); }
      draw3d();
    });
    function up(ev) { pts.delete(ev.pointerId); if (!pts.size) { dragging = false; pinch = 0; } }
    viewer.addEventListener('pointerup', up);
    viewer.addEventListener('pointercancel', up);
    viewer.addEventListener('contextmenu', function (ev) { if (V.mode !== '2d') ev.preventDefault(); });
    viewer.addEventListener('wheel', function (ev) {
      if (ev.target.closest('.vt')) return;
      ev.preventDefault();
      var f = Math.exp(-ev.deltaY * (ev.deltaMode ? 0.05 : 0.0015));
      if (V.mode === '2d') zoomBy(f, ev.clientX, ev.clientY);
      else if (V.r) { V.r.view.zoom = Math.max(0.2, Math.min(8, V.r.view.zoom * f)); draw3d(); }
    }, { passive: false });
    viewer.addEventListener('dblclick', function (ev) {
      if (ev.target.closest('.vt')) return;
      if (V.mode === '2d') { if (V.fit) { V.fit = false; V.zoom = 1; V.px = V.py = 0; } else V.fit = true; draw2d(); }
      else if (V.r) { V.r.view.zoom = 1; V.r.view.panX = V.r.view.panY = 0; draw3d(); }
    });
  }

  function full(on) {
    var app = $('app');
    var now = on === undefined ? !app.classList.contains('full') : on;
    if (now && !V.e) return;
    app.classList.toggle('full', now);
    $('v-full').setAttribute('aria-pressed', now ? 'true' : 'false');
    setTimeout(drawNow, 30);
  }

  /* ── details ──────────────────────────────────────────────────────── */

  function fact(dl, k, v, cls) {
    if (v === undefined || v === null || v === '') return;
    dl.appendChild(el('dt', '', k));
    var dd = el('dd', cls || '');
    if (v instanceof Node) dd.appendChild(v); else dd.textContent = v;
    dl.appendChild(dd);
  }
  function linkBtn(text, fn) { var b = el('button', 'linkish', text); b.type = 'button'; b.onclick = fn; return b; }
  function kindName(e) {
    if (e.pkg === A.SOD) return 'Stars of Darien model (' + (e.sod === 'card' ? 'unit card' : e.sod === 'unit' ? 'unit' : 'scenery') + ')';
    return { gaf: 'GAF sequence', taf: 'TAF sequence, true colour', img: (e.fmt || '').toUpperCase() + ' picture', '3do': '3DO model', glb: 'glTF model' }[e.k] || e.k;
  }

  function renderInfo() {
    var e = S.byId.get(S.sel), box = $('info');
    box.textContent = '';
    if (!e) { box.appendChild(el('p', 'fine', 'Pick an asset to see it here.')); return; }
    var names = e.pkg === A.SOD ? { units: [], features: [] } : A.namesOf(e, S.book);
    box.appendChild(el('h3', '', e.seq || e.name));
    box.appendChild(el('div', 'sub', [e.title, kindName(e)].filter(Boolean).join(' · ')));
    var dl = el('dl', 'facts');
    var p = pkgOf(e.pkg);
    fact(dl, 'Package', linkBtn(p ? (e.pkg === A.SOD ? 'Stars of Darien' : p.rel) + (p.official && p.label ? ' (' + p.label + ')' : p.group !== 'sod' && !p.official ? ' (custom)' : '') : e.pkg,
                                function () { setScope({ pkg: e.pkg }); }));
    fact(dl, 'Path', e.path + (e.seq ? '  #' + e.seq : ''), 'mono');
    if (A.isTexture(e)) {
      fact(dl, 'Size', e.w + ' × ' + e.h + (e.f > 1 && (e.fw !== e.w || e.fh !== e.h) ? ' (largest frame)' : ''));
      if (e.f) fact(dl, 'Frames', String(e.f));
      if (e.k === 'taf') fact(dl, 'Colour', e.fmt === 'argb4444' ? 'ARGB 4444, its own alpha' : 'ARGB 1555, one bit of alpha');
    } else {
      fact(dl, 'Triangles', (e.tris || 0).toLocaleString('en-US'));
      if (e.pieces) fact(dl, 'Pieces', String(e.pieces));
      if (e.tex) fact(dl, 'Textures', e.tex.length ? e.tex.length + ': ' + e.tex.slice(0, 12).join(', ') + (e.tex.length > 12 ? ', …' : '') : 'none, flat colours');
      if (e.imgs !== undefined) fact(dl, 'Pictures', String(e.imgs));
    }
    if (names.units.length) fact(dl, 'Unit', names.units.map(function (u) { return u.name + ' (' + u.unit + ')'; }).join(', '));
    if (names.features.length) fact(dl, 'Feature', names.features.slice(0, 6).map(function (f) { return f.name + (f.world ? ' (' + f.world + ')' : ''); }).join(', '));
    if (e.b) fact(dl, e.seq ? 'File' : 'File size', e.seq ? 'Part of a file of ' + A.sizeText(e.b) : A.sizeText(e.b));
    if (e.pkg === A.SOD) {
      var origs = (e.orig || []).map(function (id) { return S.byId.get(id); }).filter(Boolean);
      if (origs.length) {
        var ob = el('span');
        origs.slice(0, 6).forEach(function (o, i) { if (i) ob.appendChild(document.createTextNode(', ')); ob.appendChild(linkBtn(o.seq || o.name, function () { select(o.id); })); });
        fact(dl, 'Replaces', ob);
      } else fact(dl, 'Replaces', S.pkgs.size ? 'Nothing in your files that the atlas could match' : 'Load your game files to see the original');
      if (e.painted) fact(dl, 'Painting', e.painted + (e.painted === 1 ? ' material is' : ' materials are') + ' painted from your own game files, as the game does');
      if (e.rp) fact(dl, 'Card', 'Takes the place of the piece ' + e.rp + ' (texture ' + e.rt + ')');
      var gh = el('a', '', 'On GitHub');
      gh.href = 'https://github.com/' + A.SOD_REPO + '/blob/' + S.sod.commit + '/unity/Assets/Overrides/' + e.path;
      gh.target = '_blank'; gh.rel = 'noopener';
      fact(dl, 'Source', gh);
    } else if (e.rmable) {
      var rm = e.rm && S.byId.get(e.rm);
      fact(dl, 'Remaster', rm ? linkBtn('Stars of Darien: ' + rm.name, function () { select(rm.id); }) : 'None yet. This one needs a model.');
    }
    box.appendChild(dl);

    if (e.k === 'gaf' || e.k === '3do') box.appendChild(paletteBlock(e, names));
    box.appendChild(notesBlock(e.nk, 'this asset'));
    box.appendChild(exportBlock(e));
    var c = contributeBlock(e, names);
    if (c) box.appendChild(c);
  }

  function updatePalName() {
    var n = $('pal-name');
    if (n) n.textContent = V.palName || 'a grey ramp, as no palette was found';
  }

  function paletteBlock(e, names) {
    var wrap = el('div');
    wrap.appendChild(el('h4', '', e.k === '3do' ? 'How it is drawn' : 'Palette'));
    if (e.k === '3do') {
      var lit = el('label', 'toggle');
      var cb = el('input'); cb.type = 'checkbox'; cb.checked = V.lit;
      cb.onchange = function () { V.lit = cb.checked; reloadModelColour(e); };
      lit.appendChild(cb);
      lit.appendChild(document.createTextNode('Textures darkened as a battle draws them (light level 15)'));
      wrap.appendChild(lit);
      var col = el('div', 'pal-row');
      col.appendChild(el('span', 'fine', 'Team colour'));
      var sel = el('select');
      for (var i = 0; i < 10; i++) { var o = el('option', '', String(i + 1)); o.value = i; sel.appendChild(o); }
      sel.value = V.colour;
      sel.onchange = function () { V.colour = +sel.value; reloadModelColour(e); };
      col.appendChild(sel);
      wrap.appendChild(col);
      wrap.appendChild(el('div', 'why', 'Flat faces take the map’s kingdom palette in a battle, so ' + A.flatPalette(e).replace('data/palettes/', '') + ' stands in.'));
      return wrap;
    }
    var choice = A.paletteChoice(e, names);
    var row = el('div', 'pal-row');
    row.appendChild(el('span', '', 'Shown in '));
    var nm = el('span', 'mono');
    nm.id = 'pal-name';
    nm.textContent = V.palName || '…';
    row.appendChild(nm);
    wrap.appendChild(row);
    if (choice) wrap.appendChild(el('div', 'why', choice.why));
    var pick = el('div', 'pal-row');
    var sel2 = el('select');
    sel2.setAttribute('aria-label', 'Show it in another palette');
    var def = el('option', '', 'As the game draws it');
    def.value = '';
    sel2.appendChild(def);
    allPalettes().forEach(function (pth) { var o = el('option', '', pth.replace(/^(data\/)?palettes\//, '')); o.value = pth; sel2.appendChild(o); });
    sel2.value = V.pal && V.pal.file ? V.pal.file : '';
    sel2.onchange = function () {
      V.pal = sel2.value ? { file: sel2.value.indexOf('data/') === 0 ? sel2.value : 'data/' + sel2.value, transparent: 0 } : null;
      reloadFrames();
    };
    pick.appendChild(sel2);
    if (/^textures\//.test(e.path)) {
      var lt = el('label', 'toggle');
      var cb2 = el('input'); cb2.type = 'checkbox'; cb2.checked = V.shade;
      cb2.onchange = function () { V.shade = cb2.checked; reloadFrames(); };
      lt.appendChild(cb2);
      lt.appendChild(document.createTextNode('Darkened as in a battle'));
      pick.appendChild(lt);
    }
    wrap.appendChild(pick);
    return wrap;
  }

  function reloadModelColour(e) {
    var token = ++V.token;
    msg('Reading the model.');
    loadModel(e).then(function (m) {
      if (token !== V.token) return;
      if (V.r) { V.r.release(V.model); if (V.pair) V.r.release(V.pair[0]); }
      V.model = m;
      V.pair = null;
      V.pairLoading = 0;
      msg('');
      showMode();
    }, function (err) { if (token === V.token) msg(err.message); });
  }

  function reloadFrames() {
    var e = V.e, i = V.frame;
    V.frames = [];
    V.raw = [];
    var token = V.token;
    loadFrame(i, token).then(function () { if (token === V.token) { V.plane = null; drawNow(); framesBar(); } });
  }

  function allPalettes() {
    var set = new Set();
    S.indexes.forEach(function (ix) { (ix.links.palettes || []).forEach(function (p) { set.add(p); }); });
    return Array.from(set).sort();
  }

  /* ── notes ── */

  function notesBlock(nk, what) {
    var wrap = el('div');
    wrap.appendChild(el('h4', '', 'Your notes'));
    var n = S.notes[nk] || { note: '', rating: 0, tags: [] };
    var stars = el('div', 'stars');
    stars.setAttribute('role', 'radiogroup');
    stars.setAttribute('aria-label', 'Your rating of ' + what);
    var saved = el('div', 'saved');
    function paint(r) { Array.prototype.forEach.call(stars.children, function (b, i) { b.classList.toggle('on', i < r); b.setAttribute('aria-checked', i + 1 === r ? 'true' : 'false'); }); }
    for (var i = 1; i <= 5; i++) {
      (function (v) {
        var b = el('button', '', '★');
        b.type = 'button';
        b.setAttribute('role', 'radio');
        b.setAttribute('aria-label', v + (v === 1 ? ' star' : ' stars'));
        b.onclick = function () {
          var cur = (S.notes[nk] || {}).rating || 0;
          save(nk, { rating: cur === v ? 0 : v });
          paint((S.notes[nk] || {}).rating || 0);
          saved.textContent = 'Saved in this browser.';
        };
        stars.appendChild(b);
      })(i);
    }
    paint(n.rating);
    wrap.appendChild(stars);
    var tags = el('input', 'tags');
    tags.placeholder = 'Tags, split by commas';
    tags.value = (n.tags || []).join(', ');
    tags.setAttribute('aria-label', 'Tags');
    var text = el('textarea', 'note');
    text.placeholder = 'Anything worth remembering about ' + what + '. Search reads these too.';
    text.value = n.note || '';
    text.setAttribute('aria-label', 'Notes');
    var t = 0;
    function later() {
      saved.textContent = 'Saving.';
      clearTimeout(t);
      t = setTimeout(function () { save(nk, { note: text.value, tags: tags.value }); saved.textContent = 'Saved in this browser.'; }, 400);
    }
    tags.oninput = later;
    text.oninput = later;
    wrap.appendChild(tags);
    wrap.appendChild(text);
    wrap.appendChild(saved);
    var c = S.community[nk];
    if (c) {
      var cm = el('div', 'community');
      cm.appendChild(el('strong', '', 'Community note' + (c.rating ? ' · ★' + c.rating : '')));
      if (c.note) cm.appendChild(el('div', '', c.note));
      if (c.tags && c.tags.length) cm.appendChild(el('div', 'fine', c.tags.join(', ')));
      wrap.appendChild(cm);
    }
    return wrap;
  }

  function save(nk, part) {
    var cur = S.notes[nk] || { note: '', rating: 0, tags: [] };
    var next = A.cleanNote({ note: part.note !== undefined ? part.note : cur.note, rating: part.rating !== undefined ? part.rating : cur.rating,
                             tags: part.tags !== undefined ? part.tags : cur.tags, at: new Date().toISOString() });
    if (next) { S.notes[nk] = next; DB.put('notes', nk, next); }
    else { delete S.notes[nk]; DB.del('notes', nk); }
    S.entries.forEach(function (e) { if (e.nk === nk) S.hay.delete(e.id); });
    cells.forEach(function (c) { var e = S.byId.get(c.dataset.id); if (e && e.nk === nk) { c.remove(); } });
    cells.forEach(function (c, i) { if (!c.parentNode) cells.delete(i); });
    renderGrid();
    if (part.rating !== undefined) {
      var k = A.splitKey(nk);
      if (k && !k.path) renderTree();
    }
  }

  /* ── export ── */

  function download(name, blob) {
    var a = el('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 4000);
  }
  function pngOf(f) {
    var c = document.createElement('canvas');
    c.width = f.w; c.height = f.h;
    c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(f.rgba.buffer.slice(0)), f.w, f.h), 0, 0);
    return new Promise(function (res) { c.toBlob(function (b) { b.arrayBuffer().then(function (a) { res(new Uint8Array(a)); }); }, 'image/png'); });
  }
  function safe(n) { return String(n).replace(/[^\w.\-]+/g, '_'); }

  function allFrames(e) {
    var out = [], i = 0;
    function next() {
      if (i >= (e.f || 1)) return Promise.resolve(out);
      return call('frame', { pkg: e.pkg, path: e.path, ei: e.ei, frame: i, pal: palSpec(e) }).then(function (f) { out.push(f); i++; return next(); });
    }
    return next();
  }

  function exportBlock(e) {
    var wrap = el('div');
    wrap.appendChild(el('h4', '', 'Export'));
    var row = el('div', 'row-btns');
    function btn(text, fn) {
      var b = el('button', 'btn quiet small', text);
      b.type = 'button';
      b.onclick = function () {
        b.disabled = true;
        Promise.resolve().then(fn).catch(function (err) { toast('That export failed: ' + err.message); }).then(function () { b.disabled = false; });
      };
      row.appendChild(b);
    }
    var live = S.live.has(e.pkg);
    if (e.pkg === A.SOD) {
      var a = el('a', 'btn quiet small', 'Download the .glb');
      a.href = e.url; a.download = e.name + '.glb'; a.rel = 'noopener';
      row.appendChild(a);
    } else if (!live) {
      wrap.appendChild(el('p', 'fine', 'Load your game folder to export this.'));
      return wrap;
    }
    if (live && A.isTexture(e)) {
      var base = safe(e.seq || e.name);
      btn('PNG', function () {
        var f = V.raw && V.raw[V.frame];
        return (f ? Promise.resolve(f) : e.k === 'img' ? call('picture', { pkg: e.pkg, path: e.path, fmt: e.fmt }) : call('frame', { pkg: e.pkg, path: e.path, ei: e.ei, frame: 0, pal: palSpec(e) }))
          .then(pngOf).then(function (b) { download(base + (e.f > 1 ? '_' + (V.frame + 1) : '') + '.png', new Blob([b], { type: 'image/png' })); });
      });
      if (e.f > 1) {
        btn('Frames as a zip', function () {
          return allFrames(e).then(function (fs) {
            return Promise.all(fs.map(pngOf)).then(function (pngs) {
              var files = pngs.map(function (b, i) { return { name: base + '_' + String(i + 1).padStart(3, '0') + '.png', bytes: b }; });
              var meta = { sequence: e.seq, source: pkgName(e.pkg) + '/' + e.path, palette: V.palName || '',
                           frames: fs.map(function (f, i) { return { file: files[i].name, w: f.w, h: f.h, x: f.x, y: f.y }; }) };
              files.push({ name: base + '.json', bytes: new TextEncoder().encode(JSON.stringify(meta, null, 1)) });
              download(base + '_frames.zip', new Blob([A.zipStore(files, new Date())], { type: 'application/zip' }));
            });
          });
        });
        btn('Sheet PNG', function () {
          return allFrames(e).then(function (fs) {
            var cols = Math.ceil(Math.sqrt(fs.length)), cw = 0, ch = 0;
            fs.forEach(function (f) { cw = Math.max(cw, f.w); ch = Math.max(ch, f.h); });
            var c = document.createElement('canvas');
            c.width = cw * cols; c.height = ch * Math.ceil(fs.length / cols);
            var x = c.getContext('2d');
            fs.forEach(function (f, i) { x.putImageData(new ImageData(new Uint8ClampedArray(f.rgba.buffer), f.w, f.h), (i % cols) * cw, Math.floor(i / cols) * ch); });
            return new Promise(function (res) { c.toBlob(function (b) { download(base + '_sheet.png', b); res(); }, 'image/png'); });
          });
        });
      }
    }
    if (live && e.k === '3do') {
      btn('GLB for the engine', function () { return exportGlb(e, 1, 'engine'); });
      btn('GLB for Stars of Darien', function () { return exportGlb(e, 1 / 16, 'sod'); });
    }
    if (live) btn('Original file', function () {
      return call('raw', { pkg: e.pkg, path: e.path }).then(function (b) { download(A.baseName(e.path), new Blob([b])); });
    });
    wrap.appendChild(row);
    if (live && e.k === '3do')
      wrap.appendChild(el('p', 'fine', 'The GLB keeps the model’s pieces and their names, with the textures as PNG. The engine’s scale is one unit to a pixel, Stars of Darien’s one unit to a cell of 16 pixels.'));
    wrap.appendChild(el('p', 'fine', 'Made in this browser from your own files. They hold the original game’s art, so keep them for your own use.'));
    return wrap;
  }

  function exportGlb(e, scale, kind) {
    return call('model', { pkg: e.pkg, path: e.path, opts: { shade: false, colour: V.colour, flatPalette: A.flatPalette(e), own: ownTextures(e.pkg) } }).then(function (d) {
      if (!d) throw new Error('the model would not read');
      var names = Object.keys(d.textures);
      return Promise.all(names.map(function (n) { return pngOf(d.textures[n]); })).then(function (pngs) {
        var png = {};
        names.forEach(function (n, i) { png[n] = pngs[i]; });
        var groups = new Map();
        for (var t = 0; t < d.tris; t++) {
          var id = d.tex[t], tn = id >= 0 ? d.names[id] : '', has = tn && png[tn];
          var key = d.piece[t] + '|' + (has ? tn : '#' + d.col[t]);
          var g = groups.get(key);
          if (!g) {
            var w = d.flat[d.col[t]];
            g = { node: d.piece[t], name: has ? tn : 'colour' + d.col[t], png: has ? png[tn] : null, alpha: !!has, pos: [], uv: [],
                  color: has ? null : [(w & 255) / 255, ((w >>> 8) & 255) / 255, ((w >>> 16) & 255) / 255, 1] };
            groups.set(key, g);
          }
          for (var k = 0; k < 9; k++) g.pos.push(d.pos[t * 9 + k]);
          for (k = 0; k < 6; k++) g.uv.push(d.uv[t * 6 + k]);
        }
        var list = Array.from(groups.values()).map(function (g) {
          var pos = new Float32Array(g.pos);
          return { node: g.node, name: g.name, png: g.png, alpha: g.alpha, color: g.color, pos: pos, uv: new Float32Array(g.uv), normal: G.flatNormals(pos) };
        });
        var nodes = d.pieces.map(function (n, i) { return { name: n || 'piece' + i, parent: d.parents[i], t: [d.offsets[i * 3], d.offsets[i * 3 + 1], d.offsets[i * 3 + 2]] }; });
        var glb = A.writeGlb(list, { nodes: nodes, scale: scale, name: e.name });
        download(e.name.toLowerCase() + (kind === 'sod' ? '_cells' : '') + '.glb', new Blob([glb], { type: 'model/gltf-binary' }));
      });
    });
  }

  /* ── contribute ── */

  function contributeBlock(e, names) {
    var c = A.contribute(e, names);
    if (!c.engine.length && !c.sod.length) return null;
    var d = el('details', 'contrib');
    if (prefs.get('contribOpen', false)) d.open = true;
    d.ontoggle = function () { prefs.set('contribOpen', d.open); };
    d.appendChild(el('summary', '', 'Contribute a replacement'));
    function slots(title, list, doc, docText) {
      if (!list.length) return;
      d.appendChild(el('h4', '', title));
      list.forEach(function (s) {
        var slot = el('div', 'slot');
        var p = el('div', 'path');
        p.appendChild(el('code', '', s.path));
        var copy = el('button', 'icon small', '');
        copy.type = 'button';
        copy.title = 'Copy the path';
        copy.setAttribute('aria-label', 'Copy ' + s.path);
        copy.appendChild(svg('<rect x="8" y="8" width="12" height="12" rx="2" fill="none" stroke="currentColor" stroke-width="2"/><path d="M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3" fill="none" stroke="currentColor" stroke-width="2"/>'));
        copy.onclick = function () {
          (navigator.clipboard ? navigator.clipboard.writeText(s.path) : Promise.reject()).then(function () { toast('Copied ' + s.path); }, function () { toast(s.path); });
        };
        p.appendChild(copy);
        slot.appendChild(p);
        slot.appendChild(el('div', 'fine', s.note));
        d.appendChild(slot);
      });
      var a = el('a', 'fine', docText);
      a.href = doc; a.target = '_blank'; a.rel = 'noopener';
      d.appendChild(a);
    }
    slots('For OpenKingdoms', c.engine, e.k === '3do' || (names.features || []).length ? CUSTOM_MODELS : MODDING,
          e.k === '3do' || (names.features || []).length ? 'Custom models for the 3D view' : 'How folder mods work');
    if (c.sod.length) {
      slots('For Stars of Darien', c.sod, CONTRIB_SOD, 'Contributing models, from fork to merge');
      var row = el('div', 'row-btns');
      row.style.margin = '12px 0';
      var iss = el('a', 'btn small', 'Say you are making it');
      iss.href = c.issue; iss.target = '_blank'; iss.rel = 'noopener';
      row.appendChild(iss);
      d.appendChild(row);
      d.appendChild(el('p', 'fine', 'Opens an issue on Stars of Darien with the name filled in, so no one builds the same model twice. Nothing from the original game may go into a model: no pictures, no traced shapes.'));
    } else if (e.pkg !== A.SOD) {
      d.appendChild(el('p', 'fine', 'Stars of Darien replaces units and scenery with models. A picture like this one is drawn from the player’s own game files there.'));
    }
    return d;
  }

  /* ── a package's own page ─────────────────────────────────────────── */

  function showPackage(key, path) {
    S.sel = null;
    var p = pkgOf(key), box = $('info');
    if (!p) return;
    if ($('app').classList.contains('no-detail')) layoutToggle('detail', true);
    V.e = null;
    msg('');
    $('frames').hidden = true;
    var ctx = c2d.getContext('2d'); ctx.clearRect(0, 0, c2d.width, c2d.height);
    $('viewer').classList.add('d2', 'idle'); $('viewer').classList.remove('d3');
    box.textContent = '';
    box.appendChild(el('h3', '', path ? A.baseName(path) : key === A.SOD ? 'Stars of Darien' : p.name));
    box.appendChild(el('div', 'sub', path ? 'A file in ' + p.rel : key === A.SOD ? 'The remaster’s models at commit ' + S.sod.commit.slice(0, 10)
      : p.official ? 'Official, ' + p.label : p.group === 'loose' ? 'Loose files in your game folder' : 'Custom'));
    var dl = el('dl', 'facts');
    var n = countIn(key, path || '');
    fact(dl, 'Assets', n.toLocaleString('en-US'));
    if (!path && p.size) fact(dl, 'Size', A.sizeText(p.size));
    if (!path && p.files && typeof p.files === 'number') fact(dl, 'Files', p.files.toLocaleString('en-US'));
    if (!path && p.rel && key !== A.SOD) fact(dl, 'Where', p.rel, 'mono');
    if (!path && key !== A.SOD) fact(dl, 'Loaded', S.live.has(key) ? 'Yes, this visit' : 'No. Pictures made before still show.');
    if (!path && p.at) fact(dl, 'Indexed', new Date(p.at).toLocaleString());
    var hits = 0, needs = 0;
    (key === A.SOD ? S.sod.entries : (S.indexes.get(key) || { entries: [] }).entries).forEach(function (e) {
      if (path && e.path !== path) return;
      if (e.rm) hits++;
      else if (e.rmable) needs++;
    });
    if (key !== A.SOD && (hits || needs)) fact(dl, 'Remastered', hits + ' of ' + (hits + needs) + ' models and feature pictures');
    box.appendChild(dl);
    box.appendChild(notesBlock(A.noteKey(p.name, path || ''), path ? 'this file' : 'this package'));
    if (narrow.matches) tab('detail');
  }

  /* ── notes in and out ─────────────────────────────────────────────── */

  function exportNotes() {
    var n = Object.keys(S.notes).length;
    if (!n) { toast('You have no notes yet. Rate or write about an asset and it is kept here.'); return; }
    download('atlas-notes.json', new Blob([A.exportNotes(S.notes)], { type: 'application/json' }));
    toast('Exported ' + A.count(n, 'note') + '. The file holds names, paths and your words, never pictures.');
  }
  function importNotes(file) {
    file.text().then(function (text) {
      var got = A.parseNotes(text);
      if (got.error) { toast(got.error); return; }
      var m = A.mergeNotes(S.notes, got.notes), pairs = [];
      Object.keys(m.notes).forEach(function (k) { if (m.notes[k] !== S.notes[k]) pairs.push([k, m.notes[k]]); });
      S.notes = m.notes;
      DB.putMany('notes', pairs);
      S.hay = new Map();
      rebuild();
      toast('Added ' + A.count(m.added, 'note') + ' and updated ' + m.changed + '. Where you both had a note, the later edit won and the tags were joined.', 8000);
    });
  }

  /* ── layout ───────────────────────────────────────────────────────── */

  function layoutToggle(which, on) {
    var app = $('app'), cls = 'no-' + which;
    var show = on === undefined ? app.classList.contains(cls) : on;
    app.classList.toggle(cls, !show);
    prefs.set('show-' + which, show);
    $(which === 'side' ? 'side-open' : 'detail-open').hidden = show;
    setTimeout(function () { renderGrid(); drawNow(); }, 0);
  }

  function resizer(id, varName, side) {
    var r = $(id), start = 0, w0 = 0;
    function move(ev) {
      var d = ev.clientX - start, w = side === 'left' ? w0 + d : w0 - d;
      w = Math.max(180, Math.min(window.innerWidth * 0.6, w));
      document.documentElement.style.setProperty(varName, w + 'px');
      renderGrid();
      redrawSoon();
    }
    function up() {
      r.classList.remove('drag');
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      prefs.set(varName, getComputedStyle(document.documentElement).getPropertyValue(varName).trim());
    }
    r.addEventListener('pointerdown', function (ev) {
      start = ev.clientX;
      w0 = (side === 'left' ? $('side') : $('detail')).getBoundingClientRect().width;
      r.classList.add('drag');
      window.addEventListener('pointermove', move);
      window.addEventListener('pointerup', up);
      ev.preventDefault();
    });
    r.addEventListener('dblclick', function () { document.documentElement.style.removeProperty(varName); prefs.set(varName, null); renderGrid(); redrawSoon(); });
    r.addEventListener('keydown', function (ev) {
      if (ev.key !== 'ArrowLeft' && ev.key !== 'ArrowRight') return;
      var w = (side === 'left' ? $('side') : $('detail')).getBoundingClientRect().width;
      var d = (ev.key === 'ArrowRight' ? 20 : -20) * (side === 'left' ? 1 : -1);
      document.documentElement.style.setProperty(varName, Math.max(180, w + d) + 'px');
      renderGrid();
      redrawSoon();
    });
    var saved = prefs.get(varName, null);
    if (saved) document.documentElement.style.setProperty(varName, saved);
  }

  function tab(name) {
    $('app').setAttribute('data-tab', name);
    document.querySelectorAll('#tabs button').forEach(function (b) { b.setAttribute('aria-selected', b.dataset.tab === name ? 'true' : 'false'); });
    setTimeout(function () { renderGrid(); drawNow(); }, 0);
  }

  function welcome() {
    var w = $('welcome');
    var noGame = !S.pkgs.size && !S.live.size;
    var show = noGame && !(S.scope && S.scope.group === 'sod') && !S.query;
    w.hidden = !show;
    if (S.sod) $('welcome-sod').textContent = 'The ' + S.sod.entries.length + ' Stars of Darien models are listed already. Pick Stars of Darien in Packages to look through them now.';
  }

  /* ── picking ──────────────────────────────────────────────────────── */

  function pickFolder() {
    if (window.showDirectoryPicker) {
      window.showDirectoryPicker({ id: 'ok-atlas-game', mode: 'read' }).then(useHandle, function (e) {
        if (e && e.name !== 'AbortError') toast('That folder could not be read: ' + e.message);
      });
    } else {
      $('dir-input').click();
    }
  }

  function useHandle(handle) {
    return findGameDir(handle, 2).then(function (dir) {
      var d = dir || handle;
      DB.put('meta', 'dir', handle).catch(function () {});
      status('Reading ' + d.name + '.');
      return walkHandle(d).then(function (files) {
        return useSource({ kind: 'handle', name: d.name, files: files, full: !!dir });
      });
    });
  }

  function dropped(dt) {
    var items = dt.items ? Array.from(dt.items) : [];
    if (items.length && items[0].getAsFileSystemHandle) {
      return Promise.all(items.map(function (i) { return i.getAsFileSystemHandle(); })).then(function (hs) {
        var dir = hs.filter(function (h) { return h && h.kind === 'directory'; })[0];
        if (dir && hs.length === 1) return useHandle(dir);
        return Promise.all(hs.filter(function (h) { return h && h.kind === 'file'; }).map(function (h) { return h.getFile(); }))
          .then(function (fs) { var r = fromFileList(fs.map(function (f) { return { rel: f.name, file: f }; })); return useSource({ kind: 'drop', files: r.files, full: false }); });
      });
    }
    var entries = items.map(function (i) { return i.webkitGetAsEntry && i.webkitGetAsEntry(); }).filter(Boolean);
    if (entries.length) {
      var out = [];
      function walk(en, path) {
        if (en.isFile) return new Promise(function (res) { en.file(function (f) { out.push({ rel: path + en.name, file: f }); res(); }, function () { res(); }); });
        var reader = en.createReader();
        return new Promise(function (res) {
          var all = [];
          (function more() {
            reader.readEntries(function (batch) {
              if (!batch.length) {
                var i = 0;
                (function next() { if (i >= all.length) return res(); walk(all[i++], path + en.name + '/').then(next); })();
                return;
              }
              all = all.concat(batch);
              more();
            }, function () { res(); });
          })();
        });
      }
      return Promise.all(entries.map(function (en) { return walk(en, ''); })).then(function () {
        var r = fromFileList(out);
        return useSource({ kind: 'drop', name: entries.length === 1 ? entries[0].name : '', files: r.files, full: r.full });
      });
    }
    var r = fromFileList(Array.from(dt.files || []).map(function (f) { return { rel: f.name, file: f }; }));
    return useSource({ kind: 'drop', files: r.files, full: false });
  }

  function findSource() {
    return opfsFiles().then(function (files) {
      if (files) return useSource({ kind: 'opfs', files: files, full: true });
      return DB.get('meta', 'dir').then(function (h) {
        if (!h || !h.queryPermission) { sourceNote(); welcome(); return; }
        return h.queryPermission({ mode: 'read' }).then(function (state) {
          if (state === 'granted') return useHandle(h);
          var b = $('reopen');
          b.hidden = false;
          b.textContent = 'Open ' + h.name + ' again';
          b.onclick = function () { h.requestPermission({ mode: 'read' }).then(function (s) { if (s === 'granted') useHandle(h); }); };
          sourceNote();
          var note = $('source-note');
          var again = b.cloneNode(true);
          again.className = 'btn small';
          again.onclick = b.onclick;
          note.insertBefore(again, note.lastChild);
          welcome();
        });
      }).catch(function () { sourceNote(); welcome(); });
    });
  }

  /* ── wiring ───────────────────────────────────────────────────────── */

  function wire() {
    grid = $('grid');
    sizer = $('grid-sizer');
    c2d = $('v2d');
    ctx2d = c2d.getContext('2d');
    c3d = $('v3d');
    document.documentElement.style.setProperty('--cell', S.cell + 'px');
    $('f-size').value = S.cell;
    $('f-sort').value = S.sort;

    $('theme').onclick = function () {
      var t = prefs.get('theme', 'auto');
      var next = t === 'auto' ? (isDark() ? 'light' : 'dark') : t === 'light' ? 'dark' : 'auto';
      prefs.set('theme', next);
      theme(next);
    };
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () { theme(); });

    var menu = $('menu'), mb = $('menu-btn');
    mb.onclick = function (ev) { ev.stopPropagation(); menu.hidden = !menu.hidden; mb.setAttribute('aria-expanded', menu.hidden ? 'false' : 'true'); };
    document.addEventListener('click', function (ev) { if (!menu.hidden && !menu.contains(ev.target)) { menu.hidden = true; mb.setAttribute('aria-expanded', 'false'); } });
    $('m-pick').onclick = function () { menu.hidden = true; pickFolder(); };
    $('m-rescan').onclick = function () {
      menu.hidden = true;
      if (S.source && S.source.kind === 'handle') DB.get('meta', 'dir').then(function (h) { if (h) useHandle(h); });
      else if (S.source && S.source.kind !== 'opfs') toast('Pick the folder again to rescan it.');
      else findSource();
    };
    $('m-export').onclick = function () { menu.hidden = true; exportNotes(); };
    $('m-import').onclick = function () { menu.hidden = true; $('notes-input').click(); };
    $('notes-input').onchange = function () { var f = $('notes-input').files[0]; if (f) importNotes(f); $('notes-input').value = ''; };
    $('m-forget').onclick = function () {
      menu.hidden = true;
      if (!confirm('Clear the atlas index and its pictures from this browser? Your notes stay, and your game files are not touched.')) return;
      Promise.all([DB.clear('pkgs'), DB.clear('index'), DB.clear('thumbs'), DB.del('meta', 'dir'),
                   window.caches ? caches.delete('ok-atlas-remaster') : null]).then(function () { location.reload(); });
    };

    $('pick').onclick = pickFolder;
    $('dir-input').onchange = function () {
      var r = fromFileList($('dir-input').files);
      var first = $('dir-input').files[0];
      useSource({ kind: 'files', name: first ? (first.webkitRelativePath || '').split('/')[0] : '', files: r.files, full: r.full });
      $('dir-input').value = '';
    };
    var depth = 0;
    window.addEventListener('dragenter', function (ev) { if (ev.dataTransfer && Array.from(ev.dataTransfer.types).indexOf('Files') >= 0) { depth++; document.body.classList.add('dragging'); ev.preventDefault(); } });
    window.addEventListener('dragover', function (ev) { if (ev.dataTransfer && Array.from(ev.dataTransfer.types).indexOf('Files') >= 0) ev.preventDefault(); });
    window.addEventListener('dragleave', function () { if (--depth <= 0) { depth = 0; document.body.classList.remove('dragging'); } });
    window.addEventListener('drop', function (ev) {
      depth = 0;
      document.body.classList.remove('dragging');
      if (!ev.dataTransfer || !ev.dataTransfer.files || !ev.dataTransfer.files.length) return;
      ev.preventDefault();
      dropped(ev.dataTransfer).catch(function (e) { toast('Those files could not be read: ' + e.message); });
    });

    var qt = 0;
    $('q').addEventListener('input', function () {
      clearTimeout(qt);
      qt = setTimeout(function () { S.query = $('q').value.trim(); refresh(); }, 60);
    });
    document.querySelectorAll('#filters .seg').forEach(function (g) {
      g.addEventListener('click', function (ev) {
        var b = ev.target.closest('button');
        if (!b) return;
        g.querySelectorAll('button').forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); });
        S.filters[g.dataset.f] = b.dataset.v;
        refresh();
      });
    });
    $('f-rating').onchange = function () { S.filters.rating = +$('f-rating').value; refresh(); };
    $('f-sort').onchange = function () { S.sort = $('f-sort').value; prefs.set('sort', S.sort); refresh(); };
    $('f-size').oninput = function () {
      S.cell = +$('f-size').value;
      document.documentElement.style.setProperty('--cell', S.cell + 'px');
      prefs.set('cell', S.cell);
      renderGrid(true);
    };

    grid.addEventListener('scroll', function () { requestAnimationFrame(function () { renderGrid(); }); }, { passive: true });
    new ResizeObserver(function () { renderGrid(); redrawSoon(); }).observe(grid);
    new ResizeObserver(function () { redrawSoon(); }).observe($('viewer'));
    grid.addEventListener('keydown', function (ev) {
      var k = ev.key;
      if (k === 'ArrowRight') moveSel(1);
      else if (k === 'ArrowLeft') moveSel(-1);
      else if (k === 'ArrowDown') moveSel(layout.cols);
      else if (k === 'ArrowUp') moveSel(-layout.cols);
      else if (k === 'PageDown') moveSel(layout.cols * Math.max(1, Math.floor(grid.clientHeight / layout.rowH)));
      else if (k === 'PageUp') moveSel(-layout.cols * Math.max(1, Math.floor(grid.clientHeight / layout.rowH)));
      else if (k === 'Home') moveSel(-1e9);
      else if (k === 'End') moveSel(1e9);
      else if (k === 'Enter') full(true);
      else return;
      ev.preventDefault();
    });
    document.addEventListener('keydown', function (ev) {
      var typing = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement && document.activeElement.tagName);
      if (ev.key === '/' && !typing) { ev.preventDefault(); $('q').focus(); $('q').select(); }
      else if (ev.key === 'Escape') {
        if ($('app').classList.contains('full')) full(false);
        else if (document.activeElement === $('q') && $('q').value) { $('q').value = ''; S.query = ''; refresh(); }
      } else if (!typing && (ev.key === 'f' || ev.key === 'F') && !ev.ctrlKey && !ev.metaKey) full();
      else if (!typing && ev.key === ' ' && V.e && V.e.f > 1 && document.activeElement !== grid) { ev.preventDefault(); play(); }
      else if (!typing && $('app').classList.contains('full') && (ev.key === 'ArrowRight' || ev.key === 'ArrowLeft')) { moveSel(ev.key === 'ArrowRight' ? 1 : -1); }
    });

    $('side-close').onclick = function () { layoutToggle('side', false); };
    $('side-open').onclick = function () { layoutToggle('side', true); };
    $('detail-close').onclick = function () { if ($('app').classList.contains('full')) full(false); else layoutToggle('detail', false); };
    $('detail-open').onclick = function () { layoutToggle('detail', true); };
    if (!prefs.get('show-side', true)) layoutToggle('side', false); else $('side-open').hidden = true;
    if (!prefs.get('show-detail', true)) layoutToggle('detail', false); else $('detail-open').hidden = true;
    resizer('rs-left', '--side-w', 'left');
    resizer('rs-right', '--detail-w', 'right');
    document.querySelectorAll('#tabs button').forEach(function (b) { b.onclick = function () { tab(b.dataset.tab); }; });

    viewerInput();
    $('v-full').onclick = function () { full(); };
    var BGS = ['check', 'dark', 'light'];
    function applyBg() { var v = $('viewer'); BGS.forEach(function (b) { v.classList.toggle('bg-' + b, V.bg === b && b !== 'check'); }); redrawSoon(); }
    $('v-bg').onclick = function () { V.bg = BGS[(BGS.indexOf(V.bg) + 1) % BGS.length]; prefs.set('bg', V.bg); applyBg(); };
    applyBg();
    $('z-in').onclick = function () { zoomBy(1.5); };
    $('z-out').onclick = function () { zoomBy(1 / 1.5); };
    $('z-fit').onclick = function () { V.fit = true; draw2d(); };
    $('z-one').onclick = function () { V.fit = false; V.zoom = 1; V.px = V.py = 0; draw2d(); };
    $('tiles').addEventListener('click', function (ev) {
      var b = ev.target.closest('button');
      if (!b) return;
      V.tiles = +b.dataset.v;
      $('tiles').querySelectorAll('button').forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); });
      draw3d();
    });
    $('spin').setAttribute('aria-pressed', V.spin ? 'true' : 'false');
    $('spin').onclick = function () { V.spin = !V.spin; prefs.set('spin', V.spin); $('spin').setAttribute('aria-pressed', V.spin ? 'true' : 'false'); };
    $('f-prev').onclick = function () { stopPlay(); goFrame(V.frame - 1); };
    $('f-next').onclick = function () { stopPlay(); goFrame(V.frame + 1); };
    $('f-play').onclick = play;
    $('f-pos').oninput = function () { stopPlay(); goFrame(+$('f-pos').value); };
    narrow.addEventListener('change', function () { renderGrid(); redrawSoon(); });
    spinLoop();
  }

  /* ── start ────────────────────────────────────────────────────────── */

  function boot() {
    theme();
    wire();
    startWorker();
    renderInfo();
    status('Opening the atlas.');
    dbOpen().then(function (d) {
      db = d;
      if (!db) $('st-store').textContent = 'This browser keeps nothing for this page, so the index lasts only this visit';
      return Promise.all([
        DB.all('notes'), DB.all('pkgs'), DB.all('index'),
        fetch('atlas/remaster.json').then(function (r) { return r.ok ? r.json() : null; }).catch(function () { return null; }),
        fetch('atlas/community.json').then(function (r) { return r.ok ? r.text() : ''; }).catch(function () { return ''; })
      ]);
    }).then(function (r) {
      r[0].forEach(function (kv) { var c = A.cleanNote(kv[1]); if (c) S.notes[kv[0]] = c; });
      r[1].forEach(function (kv) { if (kv[1] && kv[1].v === A.INDEX_VERSION) S.pkgs.set(kv[0], kv[1]); });
      r[2].forEach(function (kv) { if (S.pkgs.has(kv[0]) && kv[1] && kv[1].entries) S.indexes.set(kv[0], kv[1]); });
      S.pkgs.forEach(function (p, k) { if (!S.indexes.has(k)) S.pkgs.delete(k); });
      S.sod = r[3] ? A.parseRemaster(r[3]) : null;
      if (r[4]) { var c = A.parseNotes(r[4]); if (c.notes) S.community = c.notes; }
      rebuild();
      sourceNote();
      status(S.entries.length ? A.count(S.entries.length, 'asset') + ' from the index in this browser.' : 'Ready.');
      return findSource();
    });
  }

  boot();
})();
