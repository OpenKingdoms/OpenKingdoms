/*
 * mods.js -- the mod registry in the browser: reading it, checking a
 * download, and installing a mod into the storage the game files use.
 *
 * The rules are src/core/mod_registry.c and src/core/mod_install.c over
 * again, so a mod installed here lands where the desktop puts one, with
 * the same manifest and the same receipt. Change one, change the other.
 *
 * No DOM in here, so scripts/test_shell_mods.js runs it in node.
 */
(function (root, factory) {
  'use strict';
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OKMods = api;
})(this, function () {
  'use strict';

  var REGISTRY_URL = 'mods/registry.json';
  var MAX = 32;                         /* TAK_MODREG_MAX */
  var SIZE_MAX = 64 * 1024 * 1024;      /* TAK_MODREG_SIZE_MAX */
  var UNPACKED_MAX = 256 * 1024 * 1024;
  var ENTRIES_MAX = 4096;
  var MODS = 'Mods/';
  var PRESETS = 'TAKEnhanced/Presets/';
  var RECEIPT = '.registry.txt';
  /* The longest each field may be in bytes, as the engine stores it. */
  var LIMITS = { id: 47, name: 31, version: 15, author: 63, page: 255, modset: 63,
                 url: 511, manual: 511, sha256: 79, fingerprint: 39 };

  function utf8(s) { return new TextEncoder().encode(s); }
  function hex(bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; i++) out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
    return out;
  }
  function isId(s, max) { return typeof s === 'string' && s.length <= max && /^[a-z0-9][a-z0-9-]*$/.test(s); }
  function plain(s) { return typeof s === 'string' && !/[\x00-\x1f\x7f]/.test(s); }
  function isHttps(s) {
    return typeof s === 'string' && /^https:\/\/[^\/]/.test(s) && !/[\x00-\x20\x7f"<>\\]/.test(s);
  }

  /* ── the registry ─────────────────────────────────────────────────── */

  function why(e, d) {
    var k;
    for (k in LIMITS) if (typeof e[k] === 'string' && utf8(e[k]).length > LIMITS[k]) return 'a field is too long';
    if (e.size !== undefined && !(typeof e.size === 'number' && Number.isInteger(e.size) && e.size >= 0))
      return 'size is not a whole number of bytes';
    if (!isId(e.id || '', LIMITS.id) || !e.id) return 'id must be lower case letters, digits and dashes';
    if (d[e.id]) return 'an id is listed twice';
    if (!e.name || !plain(e.name)) return 'name is missing';
    if (!e.version || !plain(e.version)) return 'version is missing';
    if (!e.author || !plain(e.author)) return 'author is missing';
    if (!isHttps(e.page)) return 'page must be an https address';
    if (e.url && e.manual) return 'an entry is one click or manual, not both';
    if (e.manual) return plain(e.manual) ? '' : 'manual has control characters';
    if (!e.url) return 'an entry needs a url or manual';
    if (!isHttps(e.url)) return 'url must be an https address';
    if (!e.size) return 'size is missing';
    if (e.size > SIZE_MAX) return 'size is over the limit the relay streams';
    if (!/^[0-9a-fA-F]{64}$/.test(e.sha256 || '')) return 'sha256 must be 64 hex digits';
    if (!/^[0-9a-fA-F]{16}$/.test(e.fingerprint || '')) return 'fingerprint must be 16 hex digits';
    if (/^0+$/.test(e.fingerprint)) return 'fingerprint must not be zero';
    if (!isId(e.modset || '', LIMITS.modset) || !e.modset) return 'modset must be a mod set id';
    return '';
  }

  /* {mods, errors} with the valid entries kept, or null when the text
     is not a registry at all. */
  function parse(text) {
    var j;
    try { j = typeof text === 'string' ? JSON.parse(text) : text; } catch (e) { return null; }
    if (!j || typeof j !== 'object' || Array.isArray(j) || j.registry !== 1 || !Array.isArray(j.mods)) return null;
    var out = { mods: [], errors: [] };
    var seen = Object.create(null);
    for (var i = 0; i < j.mods.length; i++) {
      var e = j.mods[i];
      if (!e || typeof e !== 'object' || Array.isArray(e)) return null;
      for (var k in LIMITS) if (e[k] !== undefined && typeof e[k] !== 'string') return null;
      var bad = why(e, seen);
      if (!bad && out.mods.length >= MAX) bad = 'the registry lists too many mods';
      if (bad) { out.errors.push((e.id || 'an entry') + ': ' + bad); continue; }
      seen[e.id] = 1;
      var m = {};
      for (k in e) m[k] = e[k];
      if (m.sha256) m.sha256 = m.sha256.toLowerCase();
      if (m.fingerprint) m.fingerprint = m.fingerprint.toLowerCase();
      out.mods.push(m);
    }
    return out;
  }

  function oneClick(e) { return !!(e && e.url && !e.manual); }
  /* "1.5 MB", or "1 KB" for a small one. */
  function sizeText(n) {
    return n < 1048576 ? Math.max(1, Math.round(n / 1024)) + ' KB' : (n / 1048576).toFixed(1) + ' MB';
  }
  function label(e) { return e.name + (e.version ? ' ' + e.version : ''); }
  function find(reg, id) {
    for (var i = 0; reg && i < reg.mods.length; i++) if (reg.mods[i].id === id) return reg.mods[i];
    return null;
  }

  /* The entry a room's mod set comes from: by fingerprint, else by its
     name and version. */
  function forRoom(reg, room) {
    if (!reg || !room) return null;
    var fp = (room.fingerprint || '').toLowerCase();
    var i;
    for (i = 0; fp && i < reg.mods.length; i++) if (reg.mods[i].fingerprint === fp) return reg.mods[i];
    var name = (room.mod || '').toLowerCase(), version = (room.mod_version || '').toLowerCase();
    if (!name) return null;
    for (i = 0; i < reg.mods.length; i++)
      if (reg.mods[i].name.toLowerCase() === name && reg.mods[i].version.toLowerCase() === version) return reg.mods[i];
    return null;
  }

  /* ── the check on a download ──────────────────────────────────────── */

  function sha256(bytes) {
    var c = (typeof crypto !== 'undefined' && crypto.subtle) ? crypto : require('crypto').webcrypto;
    return c.subtle.digest('SHA-256', bytes).then(function (d) { return hex(new Uint8Array(d)); });
  }

  /* '' when the entry plays the room's data, else why not. */
  function roomWhy(e, roomFingerprint) {
    var fp = (roomFingerprint || '').toLowerCase();
    if (!fp || /^0+$/.test(fp) || e.fingerprint === fp) return '';
    return 'The registry’s ' + label(e) + ' plays other data than that game, so it would not let you join.';
  }

  /* '' when the bytes are the entry's and the entry is the room's, else
     the reason a player reads. */
  function check(e, bytes, roomFingerprint) {
    var name = label(e);
    var room = roomWhy(e, roomFingerprint);
    if (room) return Promise.resolve(room);
    if (bytes.length !== e.size)
      return Promise.resolve('The download of ' + name + ' is ' + bytes.length + ' bytes and the registry says ' +
                             e.size + '. It was not installed.');
    return sha256(bytes).then(function (got) {
      return got === e.sha256 ? '' : 'The download of ' + name + ' does not match the registry’s fingerprint, ' +
        'so it was changed or damaged on the way. It was not installed.';
    });
  }

  /* ── zips ─────────────────────────────────────────────────────────── */

  function u16(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)) + b[o + 3] * 16777216; }

  /* The central directory: [{name, method, csize, size, at}], or null
     for something that is not a zip this reads. */
  function listZip(b) {
    var eocd = -1;
    for (var i = b.length - 22; i >= 0 && i >= b.length - 65557; i--) {
      if (u32(b, i) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) return null;
    var n = u16(b, eocd + 10), at = u32(b, eocd + 16), out = [];
    if (n > ENTRIES_MAX || at >= b.length) return null;
    var dec = new TextDecoder('utf-8');
    for (var k = 0; k < n; k++) {
      if (at + 46 > b.length || u32(b, at) !== 0x02014b50) return null;
      var flags = u16(b, at + 8), method = u16(b, at + 10);
      var csize = u32(b, at + 20), size = u32(b, at + 24);
      var nl = u16(b, at + 28), xl = u16(b, at + 30), cl = u16(b, at + 32), local = u32(b, at + 42);
      if (csize === 0xffffffff || size === 0xffffffff || local === 0xffffffff) return null;
      var raw = b.subarray(at + 46, at + 46 + nl);
      var name = (flags & 0x800) ? dec.decode(raw) : String.fromCharCode.apply(null, raw);
      out.push({ name: name, method: method, csize: csize, size: size, local: local });
      at += 46 + nl + xl + cl;
    }
    return out;
  }

  function inflateRawBrowser(data) {
    if (typeof DecompressionStream === 'undefined')
      return Promise.reject(new Error('This browser cannot unpack zips. A current Chrome, Edge, Firefox or Safari can.'));
    var s = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Response(s).arrayBuffer().then(function (a) { return new Uint8Array(a); });
  }

  function readEntry(b, e, inflateRaw) {
    var at = e.local;
    if (at + 30 > b.length || u32(b, at) !== 0x04034b50) return Promise.reject(new Error('damaged'));
    var start = at + 30 + u16(b, at + 26) + u16(b, at + 28);
    var data = b.subarray(start, start + e.csize);
    if (data.length !== e.csize) return Promise.reject(new Error('damaged'));
    var got = e.method === 0 ? Promise.resolve(data)
            : e.method === 8 ? (inflateRaw || inflateRawBrowser)(data)
            : Promise.reject(new Error('damaged'));
    return got.then(function (out) {
      if (out.length !== e.size) throw new Error('damaged');
      return out;
    });
  }

  /* A zip entry's name as a path under the mod root: {k: 1, rel} to
     install, {k: 0} to leave out, {k: -1} for one that would climb out. */
  function rel(name) {
    var p = String(name).replace(/\\/g, '/');
    if (!p || utf8(p).length >= 400 || p[0] === '/' || p.indexOf(':') >= 0) return { k: -1 };
    var segs = p.split('/');
    for (var i = 0; i < segs.length; i++) {
      var s = segs[i];
      if (s === '' && i < segs.length - 1) return { k: -1 };
      if (s === '.' || s === '..' || /[\x00-\x1f]/.test(s)) return { k: -1 };
    }
    if (p[p.length - 1] === '/') return { k: 0 };
    var low = p.toLowerCase();
    var pre = low.indexOf(MODS.toLowerCase()) === 0 ? MODS : low.indexOf(PRESETS.toLowerCase()) === 0 ? PRESETS : '';
    if (!pre || p.length === pre.length) return { k: 0 };
    return { k: 1, rel: pre + p.slice(pre.length) };
  }

  /* A folder's mod set id, byte by byte as src/core/modset.c makes it. */
  function folderId(name) {
    var b = utf8(name), out = '';
    for (var i = 0; i < b.length; i++) {
      var c = b[i];
      if (c >= 65 && c <= 90) c += 32;
      out += (c >= 97 && c <= 122) || (c >= 48 && c <= 57) ? String.fromCharCode(c) : '-';
    }
    return out;
  }

  function manifestText(e) {
    return '[MOD]\n{\nname=' + e.name + ';\nversion=' + e.version + ';\nfingerprint=' + e.fingerprint + ';\n}\n';
  }

  function receiptText(e, rels) {
    return '# Installed from the OpenKingdoms mod registry. Removing it deletes these files.\n' +
      'id=' + e.id + '\nversion=' + e.version + '\n' +
      rels.map(function (r) { return 'file=' + r + '\n'; }).join('');
  }

  function readReceipt(text) {
    var out = { id: '', version: '', files: [] };
    String(text || '').split(/\r?\n/).forEach(function (line) {
      if (line.indexOf('file=') === 0) { var r = rel(line.slice(5)); if (r.k === 1 && out.files.indexOf(r.rel) < 0) out.files.push(r.rel); }
      else if (line.indexOf('version=') === 0) out.version = line.slice(8);
      else if (line.indexOf('id=') === 0) out.id = line.slice(3);
    });
    return out;
  }

  /* ── storage ──────────────────────────────────────────────────────── */

  /* A store is {write(rel, bytes), read(rel), remove(rel), list(dir)},
     each a promise, rel a path under the mod root. The page's is the
     origin private folder the game files are kept in. */
  function opfsStore(dirName) {
    function dir(parts, create) {
      if (typeof navigator === 'undefined' || !navigator.storage || !navigator.storage.getDirectory)
        return Promise.reject(new Error('this browser keeps no files for pages'));
      return navigator.storage.getDirectory().then(function (d) {
        return d.getDirectoryHandle(dirName || 'game', { create: !!create });
      }).then(function (d) {
        var p = Promise.resolve(d);
        parts.forEach(function (seg) { p = p.then(function (h) { return h.getDirectoryHandle(seg, { create: !!create }); }); });
        return p;
      });
    }
    function split(r) { var parts = r.split('/'); return { dirs: parts.slice(0, -1), name: parts[parts.length - 1] }; }
    return {
      write: function (r, bytes) {
        var s = split(r);
        return dir(s.dirs, true).then(function (d) { return d.getFileHandle(s.name, { create: true }); })
          .then(function (h) { return h.createWritable(); })
          .then(function (w) { return w.write(bytes).then(function () { return w.close(); }); });
      },
      read: function (r) {
        var s = split(r);
        return dir(s.dirs, false).then(function (d) { return d.getFileHandle(s.name); })
          .then(function (h) { return h.getFile(); })
          .then(function (f) { return f.arrayBuffer(); })
          .then(function (a) { return new Uint8Array(a); }, function () { return null; });
      },
      remove: function (r) {
        var s = split(r);
        /* The file, then each folder it leaves empty, short of Mods and
           TAKEnhanced/Presets. */
        function up(dirs, name) {
          return dir(dirs, false).then(function (d) { return d.removeEntry(name); }).then(function () {
            var parent = dirs.join('/');
            if (!dirs.length || parent === 'Mods' || parent === 'TAKEnhanced/Presets' || parent === 'TAKEnhanced') return;
            return up(dirs.slice(0, -1), dirs[dirs.length - 1]);
          });
        }
        return up(s.dirs, s.name).catch(function () {});
      },
      list: function (d) {
        var names = [];
        return dir(d ? d.split('/') : [], false).then(function (h) {
          var it = h.entries();
          function next() {
            return it.next().then(function (x) {
              if (x.done) return names;
              if (x.value[1].kind === 'file') names.push(x.value[0]);
              return next();
            });
          }
          return next();
        }).catch(function () { return names; });
      }
    };
  }

  /* The game's in-memory folder, so a mod installed while the game
     files are loaded is there without a reload. */
  function fsStore(FS, base) {
    function mkdirp(path) {
      var cur = '';
      path.split('/').forEach(function (seg) {
        if (!seg) return;
        cur += '/' + seg;
        try { FS.mkdir(cur); } catch (e) {}
      });
    }
    return {
      write: function (r, bytes) {
        var full = base + '/' + r;
        mkdirp(full.slice(0, full.lastIndexOf('/')));
        FS.writeFile(full, bytes);
        return Promise.resolve();
      },
      read: function (r) {
        try { return Promise.resolve(FS.readFile(base + '/' + r)); } catch (e) { return Promise.resolve(null); }
      },
      remove: function (r) { try { FS.unlink(base + '/' + r); } catch (e) {} return Promise.resolve(); },
      list: function (d) {
        try {
          return Promise.resolve(FS.readdir(base + '/' + d).filter(function (n) {
            try { return !FS.isDir(FS.stat(base + '/' + d + '/' + n).mode); } catch (e) { return false; }
          }));
        } catch (e) { return Promise.resolve([]); }
      }
    };
  }

  /* Writes go to every store, reads and lists come from the first. */
  function both(a, b) {
    if (!b) return a;
    return {
      write: function (r, x) { return a.write(r, x).then(function () { return b.write(r, x); }); },
      read: a.read, list: a.list,
      remove: function (r) { return a.remove(r).then(function () { return b.remove(r); }); }
    };
  }

  /* ── install and remove ───────────────────────────────────────────── */

  function sequence(items, fn) {
    var i = 0;
    function next() { return i < items.length ? Promise.resolve(fn(items[i++])).then(next) : Promise.resolve(); }
    return next();
  }

  /* Where e's manifest goes: beside the preset whose id is e.modset, or
     in the folder whose id it is. */
  function manifestRel(e, keep, b, inflateRaw) {
    var presets = keep.filter(function (x) {
      var f = x.rel.slice(PRESETS.length);
      return x.rel.indexOf(PRESETS) === 0 && f.indexOf('/') < 0 && /\.json$/i.test(f) && f.length >= 6;
    });
    var found = null;
    return sequence(presets, function (x) {
      if (found) return;
      return readEntry(b, x.entry, inflateRaw).then(function (data) {
        var p;
        try { p = JSON.parse(new TextDecoder('utf-8').decode(data)); } catch (err) { return; }
        var m = p && p.mods;
        if (!p || typeof p.id !== 'string' || p.id.toLowerCase() !== e.modset || !m || m.enabled !== true ||
            !Array.isArray(m.selectedMods) || !m.selectedMods.length) return;
        var f = x.rel.slice(PRESETS.length);
        var stem = /\.preset\.json$/i.test(f) && f.length > 12 ? f.slice(0, -12) : f.slice(0, -5);
        found = PRESETS + stem + '.mod.tdf';
      }, function () {});
    }).then(function () {
      if (found) return found;
      for (var i = 0; i < keep.length; i++) {
        var r = keep[i].rel;
        if (r.indexOf(MODS) !== 0) continue;
        var rest = r.slice(MODS.length), slash = rest.indexOf('/');
        if (slash < 0) continue;
        if (folderId(rest.slice(0, slash)) === e.modset) return MODS + rest.slice(0, slash) + '/mod.tdf';
      }
      return '';
    });
  }

  /* Installs checked bytes. Resolves '' on success, else the reason, with
     nothing left behind. */
  function install(e, bytes, store, inflateRaw) {
    var name = label(e);
    var list = listZip(bytes);
    if (!list) return Promise.resolve('The download of ' + name + ' is not a zip.');
    if (!list.length) return Promise.resolve('The download of ' + name + ' holds no files or too many.');
    var keep = [], total = 0;
    for (var i = 0; i < list.length; i++) {
      var r = rel(list[i].name);
      if (r.k < 0) return Promise.resolve('The download of ' + name + ' holds a file that would land outside ' +
                                          'the game folder (' + list[i].name + '). It was not installed.');
      if (r.k === 0) continue;
      total += list[i].size;
      if (total > UNPACKED_MAX) return Promise.resolve('The download of ' + name + ' unpacks larger than a mod should.');
      if (!keep.some(function (x) { return x.rel === r.rel; })) keep.push({ rel: r.rel, entry: list[i] });
    }
    var wrote = [];
    return manifestRel(e, keep, bytes, inflateRaw).then(function (manifest) {
      if (!keep.length || !manifest) return 'The download of ' + name + ' has no mod set ' + e.modset + ' in it.';
      return sequence(keep, function (x) {
        return readEntry(bytes, x.entry, inflateRaw).then(function (data) {
          return store.write(x.rel, data).then(function () { wrote.push(x.rel); });
        });
      }).then(function () {
        return store.write(manifest, utf8(manifestText(e)));
      }).then(function () {
        if (wrote.indexOf(manifest) < 0) wrote.push(manifest);
        return store.write(MODS + e.id + RECEIPT, utf8(receiptText(e, wrote)));
      }).then(function () { return ''; }, function (err) {
        return sequence(wrote.slice().reverse(), function (r) { return store.remove(r); }).then(function () {
          return err && err.message === 'damaged' ? 'The download of ' + name + ' is a damaged zip.'
            : 'Could not write ' + name + ' into this browser’s storage' + (err && err.message ? ': ' + err.message : '.');
        });
      });
    });
  }

  function receipts(store) {
    return store.list('Mods').then(function (names) {
      var out = [];
      return sequence(names.filter(function (n) { return n.length > RECEIPT.length && n.slice(-RECEIPT.length) === RECEIPT; }),
        function (n) {
          return store.read(MODS + n).then(function (b) {
            if (!b) return;
            var r = readReceipt(new TextDecoder('utf-8').decode(b));
            r.id = r.id || n.slice(0, -RECEIPT.length);
            out.push(r);
          });
        }).then(function () { return out; });
    });
  }

  /* {id: version} for every registry install in the store. */
  function installed(store) {
    return receipts(store).then(function (rs) {
      var out = Object.create(null);
      rs.forEach(function (r) { out[r.id] = r.version; });
      return out;
    });
  }

  /* Removes what the install of id wrote, keeping files another install
     lists. Resolves '' or the reason. */
  function remove(id, store) {
    return receipts(store).then(function (rs) {
      var mine = rs.filter(function (r) { return r.id === id; })[0];
      if (!mine) return id + ' was not installed from the registry.';
      var kept = Object.create(null);
      rs.forEach(function (r) { if (r !== mine) r.files.forEach(function (f) { kept[f] = 1; }); });
      return sequence(mine.files.filter(function (f) { return !kept[f]; }), function (f) { return store.remove(f); })
        .then(function () { return store.remove(MODS + id + RECEIPT); })
        .then(function () { return ''; });
    });
  }

  /* Every file the registry installed, into another store: the game's
     in-memory folder after a pick that did not keep the game files. */
  function copyInstalled(from, to) {
    return receipts(from).then(function (rs) {
      var files = [];
      rs.forEach(function (r) { r.files.forEach(function (f) { if (files.indexOf(f) < 0) files.push(f); }); });
      return sequence(files, function (f) {
        return from.read(f).then(function (b) { if (b) return to.write(f, b); });
      }).then(function () { return files.length; });
    });
  }

  /* ── fetching ─────────────────────────────────────────────────────── */

  /* Straight from the host when it is this site, else through the game
     server, which streams registry entries with the CORS header most
     mod hosts never send. */
  function downloadUrl(e, api, origin) {
    if (origin && e.url.indexOf(origin + '/') === 0) return e.url;
    return api ? api + '/mods/' + encodeURIComponent(e.id) + '/download' : e.url;
  }

  /* The bytes, at most the listed size, with progress(got, size). */
  function download(e, url, progress, fetchImpl) {
    return (fetchImpl || fetch)(url, { cache: 'no-store' }).then(function (res) {
      if (!res.ok) {
        return res.text().then(function (t) {
          var m = /"error":"([^"]*)"/.exec(t || '');
          throw new Error(m ? 'The server said: ' + m[1] + '.' : 'The download answered ' + res.status + '.');
        });
      }
      if (!res.body || !res.body.getReader) {
        return res.arrayBuffer().then(function (a) { return new Uint8Array(a); });
      }
      var reader = res.body.getReader(), parts = [], got = 0;
      function pump() {
        return reader.read().then(function (x) {
          if (x.done) {
            var out = new Uint8Array(got), at = 0;
            parts.forEach(function (p) { out.set(p, at); at += p.length; });
            return out;
          }
          got += x.value.length;
          if (got > e.size) { reader.cancel(); throw new Error('The download of ' + label(e) + ' is larger than the registry says.'); }
          parts.push(x.value);
          if (progress) progress(got, e.size);
          return pump();
        });
      }
      return pump();
    });
  }

  /* ── joining a room ───────────────────────────────────────────────── */

  /* What Join on a room needs before it goes:
       {go: true}                     nothing, or nothing the registry has
       {go: true, modset}             a mod set to choose first
       {go: false, entry, why}        a reason to stay
     env: registry() a promise of the registry, sets the mod sets the
     game files hold [{id, name}], store, api, origin, confirm(text),
     progress(text), inflateRaw, fetch. */
  function forJoin(room, env) {
    if (!room || !room.mod || /^vanilla$/i.test(room.mod)) return Promise.resolve({ go: true });
    var want = (room.mod + (room.mod_version ? ' ' + room.mod_version : '')).toLowerCase();
    return Promise.resolve(env.registry()).then(function (reg) {
      var e = forRoom(reg, room);
      return installed(env.store).then(function (have) {
        if (e && have[e.id] === e.version) return { go: true, modset: e.modset };
        var sets = env.sets || [];
        for (var i = 0; i < sets.length; i++) {
          var n = (sets[i].name || '').toLowerCase();
          if (n === want || (!room.mod_version && n === room.mod.toLowerCase())) return { go: true, modset: sets[i].id };
        }
        if (!e) return { go: true };
        if (!oneClick(e))
          return { go: false, entry: e, why: 'That game plays ' + label(e) + ' by ' + e.author +
                   ', which installs by hand. Its page says how: ' + e.page };
        if (!env.confirm('That game plays ' + label(e) + ' by ' + e.author + ', which you do not have. ' +
                         'Fetch it from the mod registry (' + sizeText(e.size) + ') and join?'))
          return { go: false, entry: e, why: '' };
        /* The room first: a fetch that could never join is not worth making. */
        var bad = roomWhy(e, room.fingerprint);
        if (bad) return { go: false, entry: e, why: bad };
        return Promise.resolve().then(function () {
          var url = downloadUrl(e, env.api, env.origin);
          if (env.progress) env.progress('Fetching ' + label(e) + '...');
          return download(e, url, function (got, size) {
            if (env.progress) env.progress('Fetching ' + label(e) + ', ' + Math.floor(got * 100 / size) + '%');
          }, env.fetch).then(function (bytes) {
            return check(e, bytes, room.fingerprint).then(function (bad2) {
              if (bad2) return { go: false, entry: e, why: bad2 };
              return install(e, bytes, env.store, env.inflateRaw).then(function (bad3) {
                return bad3 ? { go: false, entry: e, why: bad3 } : { go: true, modset: e.modset, installed: e };
              });
            });
          }, function (err) {
            return { go: false, entry: e, why: label(e) + ' could not be fetched. ' + (err && err.message ? err.message : '') };
          });
        });
      });
    });
  }

  return {
    REGISTRY_URL: REGISTRY_URL,
    parse: parse,
    find: find,
    forRoom: forRoom,
    oneClick: oneClick,
    sizeText: sizeText,
    label: label,
    check: check,
    roomWhy: roomWhy,
    sha256: sha256,
    listZip: listZip,
    readEntry: readEntry,
    rel: rel,
    folderId: folderId,
    manifestText: manifestText,
    receiptText: receiptText,
    readReceipt: readReceipt,
    install: install,
    remove: remove,
    installed: installed,
    copyInstalled: copyInstalled,
    downloadUrl: downloadUrl,
    download: download,
    forJoin: forJoin,
    opfsStore: opfsStore,
    fsStore: fsStore,
    both: both
  };
});
