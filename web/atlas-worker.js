/*
 * atlas-worker.js -- the Asset Atlas's worker. It reads the visitor's own
 * files in place (WORKERFS, a slice at a time), indexes them with the
 * engine's decoders in atlas-dec.wasm, and makes thumbnails and previews.
 * Nothing it reads leaves the browser.
 */
'use strict';
importScripts('atlas.js', 'atlas-3d.js', 'atlas-dec.js');

var A = self.OKAtlas, G = self.OKAtlas3D;
var M = null;
var ready = self.AtlasDec({ print: function () {}, printErr: function () {} }).then(function (m) { M = m; });
var pkgs = new Map();         /* key -> {key, name, kind, id, files: Map(path -> File)} */
var mounts = 0;
var texmap = Object.create(null);
var palettes = new Map();     /* palette spec -> {table: Uint32Array, name} */
var held = new Map();         /* 'key|path' -> {ptr, n}, the last few files in the heap */
var HELD_MAX = 10;
var thumbGen = 0;

function str(s) {
  var n = M.lengthBytesUTF8(s) + 1, p = M._atlas_alloc(n);
  M.stringToUTF8(s, p, n);
  return p;
}
function withStr(s, fn) { var p = str(s); try { return fn(p); } finally { M._atlas_free(p); } }
function heap() { return M.HEAPU8; }

/* ── files ──────────────────────────────────────────────────────────── */

function mount(p) {
  if (pkgs.has(p.key)) close(p.key);
  var rec = { key: p.key, name: p.name, kind: p.kind, id: -1, files: null };
  if (p.kind === 'archive') {
    var dir = '/p' + (++mounts);
    M.FS.mkdir(dir);
    M.FS.mount(M.WORKERFS, { blobs: [{ name: p.name, data: p.file }] }, dir);
    rec.dir = dir;
    rec.id = withStr(dir + '/' + p.name, function (s) { return M._atlas_open(s); });
    if (rec.id < 0) {
      try { M.FS.unmount(dir); } catch (e) {}
      return { key: p.key, error: 'not an archive the game reads' };
    }
  } else {
    rec.files = new Map();
    p.files.forEach(function (f) { rec.files.set(f.path, f.file); });
  }
  pkgs.set(p.key, rec);
  return { key: p.key };
}

function close(key) {
  var rec = pkgs.get(key);
  if (!rec) return;
  dropHeld(key + '|');
  if (rec.id >= 0) M._atlas_close(rec.id);
  if (rec.dir) try { M.FS.unmount(rec.dir); } catch (e) {}
  pkgs.delete(key);
}

function setOrder(keys) {
  var ids = keys.map(function (k) { var r = pkgs.get(k); return r ? r.id : -1; }).filter(function (i) { return i >= 0; });
  var p = M._atlas_alloc(Math.max(4, ids.length * 4));
  ids.forEach(function (id, i) { M.HEAP32[(p >> 2) + i] = id; });
  M._atlas_set_order(p, ids.length);
  M._atlas_free(p);
  palettes.clear();
}

function dropHeld(prefix) {
  held.forEach(function (v, k) { if (!prefix || k.indexOf(prefix) === 0) { M._atlas_free(v.ptr); held.delete(k); } });
}

/* The file's bytes in the decoder's heap, kept for the next few asks. */
function load(key, path) {
  var hk = key + '|' + path;
  var h = held.get(hk);
  if (h) { held.delete(hk); held.set(hk, h); return h; }
  var rec = pkgs.get(key);
  if (!rec) return null;
  var ptr = 0, n = 0;
  if (rec.id >= 0) {
    ptr = withStr(path, function (s) { return M._atlas_read(rec.id, s); });
    n = M._atlas_last_size();
  } else {
    var f = rec.files.get(path);
    if (!f) return null;
    var bytes = new Uint8Array(new FileReaderSync().readAsArrayBuffer(f));
    n = bytes.length;
    ptr = M._atlas_alloc(n);
    heap().set(bytes, ptr);
  }
  if (!ptr) return null;
  h = { ptr: ptr, n: n };
  held.set(hk, h);
  while (held.size > HELD_MAX) {
    var first = held.keys().next().value;
    M._atlas_free(held.get(first).ptr);
    held.delete(first);
  }
  return h;
}

function copyOut(h, max) { return heap().slice(h.ptr, h.ptr + Math.min(h.n, max || h.n)); }

function listing(rec) {
  if (rec.id < 0) return Array.from(rec.files.keys());
  var p = M._atlas_list(rec.id);
  var text = p ? M.UTF8ToString(p) : '';
  if (p) M._atlas_free(p);
  return text.split('\n').filter(Boolean);
}

/* ── indexing ───────────────────────────────────────────────────────── */

function latin1(bytes) {
  var s = '';
  for (var i = 0; i < bytes.length; i += 8192) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
  return s;
}

function index(key, pkgName) {
  var rec = pkgs.get(key);
  if (!rec) throw new Error('not open');
  var pkg = { key: key, name: pkgName };
  var entries = [], links = { units: [], features: [], textures: [], palettes: [] };
  var paths = listing(rec), done = 0, last = Date.now();
  paths.forEach(function (path) {
    done++;
    if (Date.now() - last > 250) { last = Date.now(); postMessage({ op: 'progress', key: key, done: done, total: paths.length }); }
    var cls = A.classify(path);
    if (!cls) return;
    var h = load(key, path);
    if (!h) return;
    try {
      if (cls.k === 'gaf' || cls.k === 'taf') {
        var ip = M._atlas_gaf_info(h.ptr, h.n);
        if (!ip) return;
        var info = A.parseGafInfo(M.UTF8ToString(ip));
        M._atlas_free(ip);
        var es = A.gafEntries(pkg, path, cls.k, h.n, info);
        Array.prototype.push.apply(entries, es);
        if (/^textures\//.test(path)) es.forEach(function (e) { links.textures.push([A.lower(e.seq), path, e.ei]); });
      } else if (cls.k === 'img') {
        var size = A.imageSize(copyOut(h, 65536), cls.fmt);
        if (!size) return;
        if (/(^|\/)palettes\//.test(path) && cls.fmt === 'pcx') { links.palettes.push(path); return; }
        if (size.w * size.h <= 4) return;
        entries.push(A.fileEntry(pkg, path, cls, h.n, { w: size.w, h: size.h }));
      } else if (cls.k === '3do') {
        var tris = M._atlas_3do(h.ptr, h.n);
        if (tris < 0) return;
        entries.push(A.fileEntry(pkg, path, cls, h.n, { tris: tris, pieces: M._atlas_3do_piece_count(),
          tex: texNames().slice(0, 200) }));
      } else if (cls.k === 'glb') {
        var g = A.readGlb(copyOut(h));
        if (!g) return;
        entries.push(A.fileEntry(pkg, path, cls, h.n, { tris: A.glbTriangles(g.json), imgs: (g.json.images || []).length }));
      } else if (cls.k === 'fbi') {
        var u = A.unitFromFbi(latin1(copyOut(h)));
        if (u) links.units.push(u);
      } else if (cls.k === 'tdf') {
        Array.prototype.push.apply(links.features, A.featuresFromTdf(latin1(copyOut(h))));
      }
    } finally {
      if (cls.k !== 'gaf' && cls.k !== 'taf') {
        held.delete(key + '|' + path);
        M._atlas_free(h.ptr);
      }
    }
  });
  dropHeld(key + '|');
  return { key: key, entries: entries, links: links, files: paths.length };
}

/* The texture names of the 3DO the decoder last read, in its order. */
function texNames() {
  var n = M._atlas_3do_name_count(), p = M._atlas_3do_names(), out = [];
  for (var i = 0; i < n; i++) {
    var s = M.UTF8ToString(p);
    out.push(s);
    p += M.lengthBytesUTF8(s) + 1;
  }
  return out;
}

/* ── palettes and pictures ──────────────────────────────────────────── */

/* A palette by spec: {rule, gaf} for the engine's choice for a GAF, or
   {file, transparent}. {table, name}. */
function palette(spec, ownKey) {
  var k = JSON.stringify(spec) + '@' + (ownKey || '');
  var hit = palettes.get(k);
  if (hit) return hit;
  var rec = ownKey && pkgs.get(ownKey);
  M._atlas_prefer(rec ? rec.id : -1);
  var t = M._atlas_alloc(1024), name = M._atlas_alloc(256), grey = 0, label;
  if (spec.file) {
    var ok = withStr(spec.file, function (s) { return M._atlas_palette_file(s, spec.transparent | 0, t); }) === 0;
    if (!ok) { grey = 1; greyTable(t, spec.transparent | 0); }
    label = ok ? spec.file : '';
  } else {
    grey = withStr(spec.gaf, function (s) { return M._atlas_gaf_palette(s, spec.rule | 0, t, name); });
    label = M.UTF8ToString(name);
  }
  if (spec.shade && label) withStr(label, function (s) { return M._atlas_shade(s, spec.shade, t); });
  var table = new Uint32Array(M.HEAPU8.buffer.slice(t, t + 1024));
  M._atlas_free(t);
  M._atlas_free(name);
  M._atlas_prefer(-1);
  hit = { table: table, name: grey ? '' : label };
  palettes.set(k, hit);
  return hit;
}

function greyTable(t, clear) {
  for (var i = 0; i < 256; i++) M.HEAPU32[(t >> 2) + i] = i === clear ? 0 : (0xFF000000 | (i << 16) | (i << 8) | i) >>> 0;
}

function withTable(table, fn) {
  var t = M._atlas_alloc(1024);
  new Uint32Array(M.HEAPU8.buffer, t, 256).set(table);
  try { return fn(t); } finally { M._atlas_free(t); }
}

/* One GAF frame: {w, h, rgba} or null. */
function gafFrame(key, path, ei, frame, spec) {
  var h = load(key, path);
  if (!h) return null;
  var pal = palette(spec.file ? spec : { rule: spec.rule | 0, gaf: path, shade: spec.shade }, key);
  var px = withTable(pal.table, function (t) { return M._atlas_gaf_frame(h.ptr, h.n, ei, frame, t); });
  if (!px) return null;
  var w = M._atlas_last_w(), hh = M._atlas_last_h();
  var rgba = heap().slice(px, px + w * hh * 4);
  M._atlas_free(px);
  return { w: w, h: hh, x: M._atlas_last_x(), y: M._atlas_last_y(), rgba: rgba, palette: pal.name };
}

function picture(key, path, fmt) {
  var h = load(key, path);
  if (!h) return null;
  var px = M._atlas_image(h.ptr, h.n, A.FMT_CODE[fmt] || 0);
  held.delete(key + '|' + path);
  M._atlas_free(h.ptr);
  if (!px) return null;
  var w = M._atlas_last_w(), hh = M._atlas_last_h();
  var rgba = heap().slice(px, px + w * hh * 4);
  M._atlas_free(px);
  return { w: w, h: hh, rgba: rgba };
}

/* The first frame of a picture a remaster model is painted with, made as
   the remaster's engine makes it (ok_embed.c paint_indices): the
   palette's own colours, clear at the frame's clear index and, for a
   texture, at the colour of its first texel. */
function paintFrame(key, path, ei, pcx, corner) {
  var h = load(key, path);
  if (!h) return null;
  if (!pcx) pcx = palette({ rule: 1, gaf: path }, key).name || 'data/palettes/gameart.pcx';
  var rgb = M._atlas_alloc(768);
  var ok = withStr(pcx, function (s) { return M._atlas_palette_rgb(s, rgb); }) === 0;
  var pal = heap().slice(rgb, rgb + 768);
  M._atlas_free(rgb);
  if (!ok) return null;
  var idx = M._atlas_gaf_indices(h.ptr, h.n, ei, 0);
  if (!idx) return null;
  var w = M._atlas_last_w(), hh = M._atlas_last_h(), clear = M._atlas_last_key();
  var src = heap().slice(idx, idx + w * hh);
  M._atlas_free(idx);
  var out = new Uint8Array(w * hh * 4), second = corner ? src[0] : -1;
  for (var i = 0; i < src.length; i++) {
    var v = src[i];
    if (v === clear || v === second) continue;
    out[i * 4] = pal[v * 3]; out[i * 4 + 1] = pal[v * 3 + 1]; out[i * 4 + 2] = pal[v * 3 + 2]; out[i * 4 + 3] = 255;
  }
  return { w: w, h: hh, rgba: out, palette: pcx };
}

function frameCount(key, path, ei) {
  var h = load(key, path);
  if (!h || h.n < 16 + 4 * ei) return 0;
  var b = heap(), p = h.ptr + 12 + 4 * ei;
  var off = (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)) >>> 0;
  return off + 2 <= h.n ? b[h.ptr + off] | (b[h.ptr + off + 1] << 8) : 0;
}

/* A 3DO as triangles with its textures and flat colours resolved. */
function model3do(key, path, opts) {
  var h = load(key, path);
  if (!h) return null;
  var tris = M._atlas_3do(h.ptr, h.n);
  held.delete(key + '|' + path);
  M._atlas_free(h.ptr);
  if (tris < 0) return null;
  var d = {
    tris: tris,
    pos: new Float32Array(M.HEAPF32.buffer.slice(M._atlas_3do_pos(), M._atlas_3do_pos() + tris * 36)),
    uv: new Float32Array(M.HEAPF32.buffer.slice(M._atlas_3do_uv(), M._atlas_3do_uv() + tris * 24)),
    tex: new Int32Array(M.HEAP32.buffer.slice(M._atlas_3do_tex(), M._atlas_3do_tex() + tris * 4)),
    col: M.HEAPU8.slice(M._atlas_3do_col(), M._atlas_3do_col() + tris),
    names: texNames()
  };
  var np = Math.min(256, M._atlas_3do_piece_count());
  d.pieces = M.UTF8ToString(M._atlas_3do_pieces()).split('\n').slice(0, np);
  d.piece = new Int16Array(M.HEAP32.buffer.slice(M._atlas_3do_piece_of(), M._atlas_3do_piece_of() + tris * 2));
  d.parents = new Int16Array(M.HEAP32.buffer.slice(M._atlas_3do_parents(), M._atlas_3do_parents() + np * 2));
  d.offsets = new Float32Array(M.HEAPF32.buffer.slice(M._atlas_3do_offsets(), M._atlas_3do_offsets() + np * 12));
  var textures = {}, missing = [];
  d.names.forEach(function (n) {
    var where = (opts.own && opts.own[n]) || texmap[n];
    /* A texture with a frame for each player colour shows the asked
       colour, as tex_atlas.c decode_gaf_entries picks it. */
    var colour = where && frameCount(where[0], where[1], where[2]) >= 10 ? opts.colour || 0 : 0;
    var f = where ? gafFrame(where[0], where[1], where[2], colour, { rule: 1, shade: opts.shade ? 15 : 0 }) : null;
    if (f) textures[n] = f; else missing.push(n);
  });
  var flat = palette({ file: opts.flatPalette || 'data/palettes/gameart.pcx', transparent: 0 });
  d.textures = textures;
  d.missing = missing;
  d.flat = flat.table;
  d.flatName = flat.name;
  return d;
}

/* ── thumbnails ─────────────────────────────────────────────────────── */

var thumbCanvas = null, thumbGl = null;

function fitCanvas(img, box) {
  var s = Math.min(1, box / Math.max(img.w, img.h));
  var w = Math.max(1, Math.round(img.w * s)), h = Math.max(1, Math.round(img.h * s));
  var src = new OffscreenCanvas(img.w, img.h);
  src.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(img.rgba.buffer, img.rgba.byteOffset, img.w * img.h * 4), img.w, img.h), 0, 0);
  if (s === 1) return src;
  var c = new OffscreenCanvas(w, h), x = c.getContext('2d');
  x.imageSmoothingEnabled = true;
  x.imageSmoothingQuality = 'high';
  x.drawImage(src, 0, 0, w, h);
  return c;
}

function encode(canvas) {
  return canvas.convertToBlob({ type: 'image/webp', quality: 0.86 }).then(function (b) {
    return b && b.type === 'image/webp' ? b : canvas.convertToBlob({ type: 'image/png' });
  });
}

function render3d(model, size) {
  if (!thumbCanvas) {
    thumbCanvas = new OffscreenCanvas(size, size);
    try { thumbGl = G.renderer(thumbCanvas, { preserve: true }); } catch (e) { thumbGl = null; }
  }
  if (!thumbGl) return null;
  thumbCanvas.width = thumbCanvas.height = size;
  thumbGl.view.yaw = -0.4;
  thumbGl.view.pitch = 0.55;
  thumbGl.view.zoom = 1.15;
  thumbGl.draw([model], { grid: false, background: [0.16, 0.16, 0.17] });
  var out = encode(thumbCanvas);
  thumbGl.release(model);
  return out;
}

function thumb(r) {
  var img = null;
  if (r.k === 'gaf' || r.k === 'taf') img = gafFrame(r.pkg, r.path, r.ei, r.frame || 0, r.pal || { rule: 0 });
  else if (r.k === 'img') img = picture(r.pkg, r.path, r.fmt);
  else if (r.k === '3do') {
    var d = model3do(r.pkg, r.path, { shade: true, flatPalette: r.flat, own: r.own });
    if (!d) return Promise.resolve(null);
    return Promise.resolve(render3d(G.from3do(d, d.textures, d.flat), 192));
  } else if (r.k === 'glb') {
    var h = load(r.pkg, r.path);
    if (!h) return Promise.resolve(null);
    var bytes = copyOut(h);
    return G.fromGlb(bytes, { scale: 1, image: decodeImage }).then(function (m) { return render3d(m, 192); });
  }
  if (!img) return Promise.resolve(null);
  return encode(fitCanvas(img, 160));
}

function decodeImage(bytes, mime) {
  return createImageBitmap(new Blob([bytes], { type: mime }));
}

/* ── messages ───────────────────────────────────────────────────────── */

function reply(id, value, transfer) { postMessage({ op: 'reply', id: id, value: value }, transfer || []); }
function fail(id, e) { postMessage({ op: 'reply', id: id, error: (e && e.message) || String(e) }); }

var queue = Promise.resolve();
self.onmessage = function (ev) {
  var m = ev.data;
  if (m.op === 'thumbs') {
    var gen = ++thumbGen;
    queue = queue.then(function () { return ready; }).then(function () {
      var i = 0;
      function next() {
        if (gen !== thumbGen || i >= m.list.length) return null;
        var r = m.list[i++];
        return Promise.resolve().then(function () { return thumb(r); }).then(function (blob) {
          postMessage({ op: 'thumb', uid: r.uid, blob: blob || null });
        }, function () {
          postMessage({ op: 'thumb', uid: r.uid, blob: null });
        }).then(next);
      }
      return next();
    });
    return;
  }
  queue = queue.then(function () { return ready; }).then(function () {
    try {
      switch (m.op) {
        case 'mount': return reply(m.id, m.list.map(mount));
        case 'close': m.keys.forEach(close); return reply(m.id, true);
        case 'order': setOrder(m.keys); return reply(m.id, true);
        case 'texmap': texmap = m.map; return reply(m.id, true);
        case 'index': return reply(m.id, index(m.key, m.name));
        case 'frame': {
          var f = gafFrame(m.pkg, m.path, m.ei, m.frame, m.pal || { rule: 0 });
          return reply(m.id, f, f ? [f.rgba.buffer] : []);
        }
        case 'paint': {
          var pf = paintFrame(m.pkg, m.path, m.ei, m.pcx, m.corner);
          return reply(m.id, pf, pf ? [pf.rgba.buffer] : []);
        }
        case 'picture': {
          var p = picture(m.pkg, m.path, m.fmt);
          return reply(m.id, p, p ? [p.rgba.buffer] : []);
        }
        case 'model': {
          var d = model3do(m.pkg, m.path, m.opts || {});
          return reply(m.id, d);
        }
        case 'raw': {
          var h = load(m.pkg, m.path);
          var b = h ? copyOut(h) : null;
          return reply(m.id, b, b ? [b.buffer] : []);
        }
        default: return fail(m.id, 'unknown request ' + m.op);
      }
    } catch (e) {
      return fail(m.id, e);
    }
  });
};
