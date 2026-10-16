/*
 * atlas.js -- the Asset Atlas without the page: which archives are the
 * game's own, what a file is, the links between models and the names the
 * game gives them, search, the notes and their merge rule, and the files
 * the atlas writes (zip, GLB). The page, its worker and node all load it.
 *
 * No DOM in here, so scripts/test_atlas.js runs it in node.
 */
(function (root, factory) {
  'use strict';
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OKAtlas = api;
})(this, function () {
  'use strict';

  /* Bumped when an index entry changes shape, so a stored index is rebuilt. */
  var INDEX_VERSION = 1;
  var SOD = 'stars-of-darien';
  var SOD_REPO = 'OpenKingdoms/Stars-of-Darien';
  var ENGINE_REPO = 'OpenKingdoms/OpenKingdoms';

  /* ── the game's own archives ──────────────────────────────────────── */

  var LANGS = ['english', 'french', 'german', 'italian', 'spanish'];
  var OFFICIAL = { 'data.hpi': 'Kingdoms', 'terrain.hpi': 'Kingdoms', 'sections.hpi': 'Kingdoms',
                   'maps.hpi': 'Kingdoms', 'missions.hpi': 'Kingdoms', 'boneyards.hpi': 'Boneyards',
                   'boneyards2.hpi': 'Boneyards', 'meta.hpi': 'Boneyards', 'jersey.hpi': 'Kingdoms',
                   'v2rocket.hpi': 'Patch 2.0', 'v3rocket.hpi': 'Patch 3.0', 'ipdata.hpi': 'The Iron Plague',
                   'ipmissions.hpi': 'The Iron Plague', 'ipsections.hpi': 'The Iron Plague' };
  LANGS.forEach(function (l) {
    OFFICIAL[l + '.hpi'] = 'Kingdoms';
    OFFICIAL['ip' + l + '.hpi'] = 'The Iron Plague';
  });
  /* The Darien Crusades map packs, which the retail install carries in Maps. */
  var DARIEN = ("adamantine gate|aibel's seaport|alchemist glen|arches of zhon|barb coast|bay of mist|bell toll|" +
    "black forest|blood feud|brandon's woods|burial mound|caer conlin|canal crossing|canal walls|cauldron waters|" +
    "clan village|crab cove|crash barrens|crossings|cursed temple|dancing river|dan'gel ravine|drake point|" +
    "dreadrock canal|drialia's cliffs|dry ring|east kaluen|edmont's field|endocott isle|ensign's lament|" +
    "evergreen ridge|extinct volcano|fallow fields|fel doom|fey citadel|field of dasanko|fire ant field|" +
    "fossil cleft|fountainhead|four barracks|gaerion wood|ghost shipping|green island|greenstone point|" +
    "greenwater|grimm farms|hall of heroes|hallowed river|hanging gardens|harsh dunes|hedgemaze|highland village|" +
    "hillspire vista|human menagerie|icy peaks|ihom coast|ill fens|inclined forest|into the woods|invasion bluffs|" +
    "inwaari temple|inzacata|iobar's theatre|irontaste lake|isle of sand|ivy mountains|jungle adventure|" +
    "jungle hunt|jungle island|kandran moat|kingmaker isle|kraken temple|lantern district|lieber's project|" +
    "living wastes|lobster manor|long canal|lost river valley|mana quarry|manorhome|marina largo|marlok bay|" +
    "mateo grasslands|m'hari outskirts|mithril run|mogrium mound|mountain madness|mountain temple|mud fern lakes|" +
    "mukiltrom valley|narmer harbor|navigator island|neegam's pass|new hindigal|oggin landing|oldhenge|" +
    "oracle at pentus|orc rocks|overseer island|overseer plateau|parched sea|path of pardu|peasant grove|" +
    "peninsular vina|peream land|phantom crest|plateau aerinarr|precipices|qinzi plazas|quartz springs|" +
    "red rivers|riverfork wood|riverlook|robber road|rump's roost|rynn's valley|salt pillars|sandy shore|" +
    "sardonic isles|savage isle|scribe's slough|sea castle|sea snake|seas of flame|seygon canal|shiaugh lakes|" +
    "shifting sands|signal pointe|sinibad's cove|siren rocks|slaver den|soulspit|sparkly bay|spectre keep|" +
    "still lake|stone baths|streets of pain|syphon channels|talgath's toll|tartaglia narrows|tarza's river|" +
    "temple of blood|tenfinger road|terror hamlet|terror town|theatre of cruelty|the glavnos|the lochs|" +
    "the magic knot|the muldoons|the quandary|the sea wood|the vault|thorn boscage|three jewels|toad river|" +
    "treehenge|tribal shores|tribelands|troubled waters|twisted grove|twitching rock|two sins|ulasem|" +
    "unholy purpose|urban riverside|valysia city|verdun cliffs|volcanic isles|volcanic stone|voyager isles|" +
    "walls of carin vel|walls of elam|waterworks|whispering wood|worm wood|yew wood|zhon highlands|zhorl valley|" +
    "zirra channel").split('|');
  var DARIEN_SET = Object.create(null);
  DARIEN.forEach(function (n) { DARIEN_SET[n] = 1; });

  function lower(s) { return String(s == null ? '' : s).toLowerCase(); }
  function baseName(p) { var s = String(p); var i = s.lastIndexOf('/'); return i < 0 ? s : s.slice(i + 1); }
  function stem(p) { var b = baseName(p); var i = b.lastIndexOf('.'); return i <= 0 ? b : b.slice(0, i); }
  function ext(p) { var b = baseName(p); var i = b.lastIndexOf('.'); return i < 0 ? '' : lower(b.slice(i + 1)); }

  /* Where a file of the game folder sits in the package tree, and whether
     it is the game's own. rel is its path under the game folder. */
  function packageInfo(rel) {
    var r = String(rel).replace(/\\/g, '/').replace(/^\/+/, '');
    var parts = r.split('/');
    var low = lower(r), name = baseName(r);
    var out = { rel: r, key: low, name: name, group: 'custom', official: false, label: '' };
    if (parts.length === 1 && OFFICIAL[low]) {
      out.group = 'game';
      out.official = true;
      out.label = OFFICIAL[low];
    } else if (parts.length === 1) {
      out.group = 'game';
    } else if (lower(parts[0]) === 'maps' && parts.length === 2) {
      out.group = 'maps';
      if (DARIEN_SET[lower(stem(name))]) { out.official = true; out.label = 'Darien Crusades'; }
    } else if (lower(parts[0]) === 'mods') {
      out.group = 'mods';
    } else {
      out.group = 'loose';
    }
    return out;
  }

  /* The order the engine mounts archives in: .hpi, then .ufo, then the map
     packs, each by name (hpi_vfs.c VFS_Init). Others keep their order after. */
  function mountOrder(rels) {
    function rank(r) {
      var p = String(r).split('/');
      if (p.length === 1 && /\.hpi$/i.test(r)) return 0;
      if (p.length === 1 && /\.ufo$/i.test(r)) return 1;
      if (p.length === 2 && /^maps$/i.test(p[0]) && /\.kmp$/i.test(r)) return 2;
      return 3;
    }
    return rels.slice().sort(function (a, b) {
      var d = rank(a) - rank(b);
      if (d) return d;
      var la = lower(a), lb = lower(b);
      return la < lb ? -1 : la > lb ? 1 : 0;
    });
  }

  /* The order packages are shown in: the base game, The Iron Plague, the
     patches and the online extras, then everything else by name. */
  var SHOWN = ['data.hpi', 'terrain.hpi', 'sections.hpi', 'maps.hpi', 'missions.hpi'];
  LANGS.forEach(function (l) { SHOWN.push(l + '.hpi'); });
  SHOWN.push('ipdata.hpi', 'ipsections.hpi', 'ipmissions.hpi');
  LANGS.forEach(function (l) { SHOWN.push('ip' + l + '.hpi'); });
  SHOWN.push('v2rocket.hpi', 'v3rocket.hpi', 'jersey.hpi', 'boneyards.hpi', 'boneyards2.hpi', 'meta.hpi');
  var GROUP_RANK = { game: 0, maps: 1, mods: 2, loose: 3, custom: 4, sod: 5 };
  function displayOrder(pkgs) {
    function rank(p) {
      var i = SHOWN.indexOf(lower(p.rel));
      return (GROUP_RANK[p.group] === undefined ? 4 : GROUP_RANK[p.group]) * 1000 + (i < 0 ? 999 : i);
    }
    return pkgs.slice().sort(function (a, b) { return rank(a) - rank(b) || cmp(lower(a.rel), lower(b.rel)); });
  }

  function isArchive(name) { return /\.(hpi|ufo|kmp|ccx|gp3)$/i.test(name); }

  /* What a file is to the atlas: {k, fmt} for an asset, {k: 'tdf'|'fbi'}
     for the data that names models, or null. */
  function classify(path) {
    var e = ext(path);
    if (e === 'gaf') return { k: 'gaf' };
    if (e === 'taf') return { k: 'taf' };
    if (e === 'jpg' || e === 'jpeg') return { k: 'img', fmt: 'jpg' };
    if (e === 'png' || e === 'pcx' || e === 'bmp' || e === 'tga') return { k: 'img', fmt: e };
    if (e === '3do') return { k: '3do' };
    if (e === 'glb') return { k: 'glb' };
    if (e === 'fbi') return { k: 'fbi' };
    if (e === 'tdf' && /(^|\/)features\//i.test(path)) return { k: 'tdf' };
    return null;
  }

  var KIND_GROUP = { gaf: 'texture', taf: 'texture', img: 'texture', '3do': 'model', glb: 'model' };
  var FMT_CODE = { jpg: 1, png: 2, pcx: 3, bmp: 4, tga: 5 };

  /* ── keys ─────────────────────────────────────────────────────────── */

  /* The key notes are kept under: the archive's file name and the path in
     it, with the sequence for a GAF, all lower case. A package's own notes
     have an empty path. */
  function noteKey(pkgName, path, seq) {
    return lower(pkgName) + '|' + lower(path || '') + (seq ? '#' + lower(seq) : '');
  }
  /* An entry's own id: like its note key but under the package's whole
     path, so two archives of one name in two mods stay apart. */
  function uid(pkgKey, path, seq) { return lower(pkgKey) + '|' + lower(path || '') + (seq ? '#' + lower(seq) : ''); }
  function splitKey(key) {
    var m = /^([^|]+)\|([^#]*)(?:#(.+))?$/.exec(String(key));
    return m ? { pkg: m[1], path: m[2], seq: m[3] || '' } : null;
  }

  /* ── TDF, FBI ─────────────────────────────────────────────────────── */

  /* Sections of a TDF-style file: [{name, keys: {lowercased: value}, sub: [...]}]. */
  function parseTdf(text) {
    var s = String(text || '').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
    var top = [], stack = [], i = 0, pending = null;
    var cur = function () { return stack.length ? stack[stack.length - 1] : null; };
    while (i < s.length) {
      var c = s[i];
      if (c === '[') {
        var end = s.indexOf(']', i);
        if (end < 0) break;
        pending = { name: s.slice(i + 1, end).trim(), keys: {}, sub: [] };
        i = end + 1;
      } else if (c === '{') {
        var sec = pending || { name: '', keys: {}, sub: [] };
        pending = null;
        if (cur()) cur().sub.push(sec); else top.push(sec);
        stack.push(sec);
        i++;
      } else if (c === '}') {
        stack.pop();
        i++;
      } else if (/\s/.test(c)) {
        i++;
      } else {
        var semi = s.indexOf(';', i);
        var stop = semi < 0 ? s.length : semi;
        var nb = s.slice(i, stop).search(/[{}[]/);
        if (nb >= 0) stop = i + nb;
        var line = s.slice(i, stop);
        var eq = line.indexOf('=');
        if (eq > 0 && cur()) cur().keys[lower(line.slice(0, eq).trim())] = line.slice(eq + 1).trim();
        i = stop === semi ? stop + 1 : Math.max(stop, i + 1);
      }
    }
    return top;
  }

  /* A unit's names from its FBI: {unit, object, name, side, desc}, or null. */
  function unitFromFbi(text) {
    var secs = parseTdf(text);
    for (var i = 0; i < secs.length; i++) {
      if (lower(secs[i].name) !== 'unitinfo') continue;
      var k = secs[i].keys;
      if (!k.unitname && !k.objectname) return null;
      return { unit: k.unitname || '', object: k.objectname || k.unitname || '', name: k.name || '',
               side: k.side || '', desc: k.description || '' };
    }
    return null;
  }

  /* The features a feature TDF defines: [{name, file, seq, object, world, desc}]. */
  function featuresFromTdf(text) {
    return parseTdf(text).filter(function (s) { return s.name; }).map(function (s) {
      var k = s.keys;
      return { name: s.name, file: k.filename || '', seq: k.seqname || '', object: k.object || '',
               world: k.world || '', desc: k.description || '' };
    }).filter(function (f) { return f.seq || f.object; });
  }

  /* ── picture headers ──────────────────────────────────────────────── */

  function u16le(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32le(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)) + b[o + 3] * 16777216; }
  function u32be(b, o) { return b[o] * 16777216 + ((b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]); }

  /* {w, h} from a picture's header without decoding it, or null. */
  function imageSize(b, fmt) {
    if (!b || !b.length) return null;
    if (fmt === 'png') {
      if (b.length < 24 || b[0] !== 0x89 || b[1] !== 0x50) return null;
      return { w: u32be(b, 16), h: u32be(b, 20) };
    }
    if (fmt === 'jpg') {
      if (b[0] !== 0xFF || b[1] !== 0xD8) return null;
      var o = 2;
      while (o + 9 < b.length) {
        if (b[o] !== 0xFF) { o++; continue; }
        var m = b[o + 1];
        if (m === 0xD8 || m === 0x01 || (m >= 0xD0 && m <= 0xD7) || m === 0xFF) { o += m === 0xFF ? 1 : 2; continue; }
        var len = (b[o + 2] << 8) | b[o + 3];
        if (m >= 0xC0 && m <= 0xCF && m !== 0xC4 && m !== 0xC8 && m !== 0xCC)
          return { w: (b[o + 7] << 8) | b[o + 8], h: (b[o + 5] << 8) | b[o + 6] };
        o += 2 + len;
      }
      return null;
    }
    if (fmt === 'pcx') {
      if (b.length < 128 || b[0] !== 0x0A) return null;
      return { w: u16le(b, 8) - u16le(b, 4) + 1, h: u16le(b, 10) - u16le(b, 6) + 1 };
    }
    if (fmt === 'bmp') {
      if (b.length < 26 || b[0] !== 0x42 || b[1] !== 0x4D) return null;
      var hs = u32le(b, 14);
      if (hs === 12) return { w: u16le(b, 18), h: u16le(b, 20) };
      var h = u32le(b, 22);
      if (h > 0x7fffffff) h = 0x100000000 - h;
      return { w: u32le(b, 18), h: h };
    }
    if (fmt === 'tga') {
      if (b.length < 18) return null;
      return { w: u16le(b, 12), h: u16le(b, 14) };
    }
    return null;
  }

  /* The decoder's GAF listing: [{ei, name, frames: [{w, h, x, y, c}]}]. */
  function parseGafInfo(text) {
    var out = [];
    String(text || '').split('\n').forEach(function (line) {
      if (!line) return;
      var t = line.split('\t');
      if (t.length < 4) return;
      var frames = t[3] ? t[3].split(';').map(function (f) {
        var n = f.split(',').map(Number);
        return { w: n[0], h: n[1], x: n[2], y: n[3], c: n[4] };
      }) : [];
      out.push({ ei: +t[0], name: t[1].replace(/\s+$/, ''), frames: frames });
    });
    return out;
  }

  /* ── entries ──────────────────────────────────────────────────────── */

  /* The asset entries of a GAF or TAF at path in package pkg. */
  function gafEntries(pkg, path, k, bytes, info) {
    return info.filter(function (e) { return e.name; }).map(function (e) {
      var f0 = e.frames[0] || { w: 0, h: 0, c: -1 };
      var w = 0, h = 0;
      e.frames.forEach(function (f) { if (f.w > w) w = f.w; if (f.h > h) h = f.h; });
      var taf = e.frames.some(function (f) { return f.c === 4 || f.c === 5; });
      return { id: uid(pkg.key, path, e.name), nk: noteKey(pkg.name, path, e.name), pkg: pkg.key, path: path, seq: e.name,
               ei: e.ei, k: taf ? 'taf' : k,
               name: e.name, w: w, h: h, f: e.frames.length, b: bytes, fw: f0.w, fh: f0.h,
               fmt: taf ? (e.frames[0].c === 4 ? 'argb4444' : 'argb1555') : 'paletted' };
    });
  }

  function fileEntry(pkg, path, cls, bytes, extra) {
    var e = { id: uid(pkg.key, path), nk: noteKey(pkg.name, path), pkg: pkg.key, path: path, k: cls.k, name: stem(path),
              b: bytes };
    if (cls.fmt) e.fmt = cls.fmt;
    for (var k in extra) if (extra[k] !== undefined) e[k] = extra[k];
    return e;
  }

  function isTexture(e) { return KIND_GROUP[e.k] === 'texture'; }
  function isModel(e) { return KIND_GROUP[e.k] === 'model'; }

  /* ── the remaster ─────────────────────────────────────────────────── */

  /* The remaster's list (scripts/atlas-remaster-index.py) as entries of
     its own package, or null when the file is not one. */
  function parseRemaster(json) {
    var j = json;
    if (typeof j === 'string') { try { j = JSON.parse(j); } catch (e) { return null; } }
    if (!j || j.okAtlasRemaster !== 1 || !Array.isArray(j.models) || !/^https:\/\//.test(j.base || '')) return null;
    var entries = j.models.filter(function (m) {
      return m && typeof m.n === 'string' && typeof m.p === 'string' && /\.glb$/i.test(m.p) && !/\.\./.test(m.p);
    }).map(function (m) {
      var short = m.p.replace(/^unity\/Assets\/Overrides\//, '');
      return { id: noteKey(SOD, short), nk: noteKey(SOD, short), pkg: SOD, path: short, k: 'glb', name: m.n, b: m.b | 0, tris: m.t | 0,
               imgs: m.i | 0, painted: m.pt | 0, sod: m.k, url: j.base + m.p.split('/').map(encodeURIComponent).join('/'),
               rp: m.rp || '', rt: m.rt || '' };
    });
    return { commit: j.commit || '', repo: j.repo || SOD_REPO, entries: entries };
  }

  /* The names the game knows each object by, from every package's links. */
  function nameBook(linkSets) {
    var book = { unitsByObject: Object.create(null), featuresByObject: Object.create(null),
                 featuresBySeq: Object.create(null), units: Object.create(null), features: Object.create(null) };
    function push(map, k, v) { k = lower(k); if (!k) return; (map[k] = map[k] || []).push(v); }
    linkSets.forEach(function (l) {
      if (!l) return;
      (l.units || []).forEach(function (u) { push(book.unitsByObject, u.object, u); book.units[lower(u.unit)] = u; });
      (l.features || []).forEach(function (f) {
        if (f.object) push(book.featuresByObject, f.object, f);
        if (f.seq) push(book.featuresBySeq, f.seq, f);
        book.features[lower(f.name)] = f;
      });
    });
    return book;
  }

  /* The last of each name, as a later archive's copy wins. */
  function uniqueBy(list, k) {
    var seen = Object.create(null), out = [];
    for (var i = list.length - 1; i >= 0; i--) {
      var n = lower(list[i][k]);
      if (seen[n]) continue;
      seen[n] = 1;
      out.unshift(list[i]);
    }
    return out;
  }

  /* What the game calls an entry and what could replace it:
     {units, features, title, replaceable}. */
  function namesOf(e, book) {
    var units = [], features = [];
    if (e.k === '3do') {
      units = book.unitsByObject[lower(e.name)] || [];
      features = book.featuresByObject[lower(e.name)] || [];
    } else if (e.seq) {
      features = (book.featuresBySeq[lower(e.seq)] || []).filter(function (f) {
        return !f.file || lower(f.file) === lower(stem(e.path));
      });
    }
    units = uniqueBy(units, 'unit');
    features = uniqueBy(features, 'name');
    var title = units.length ? units[0].name : features.length ? features[0].desc : '';
    return { units: units, features: features, title: title, replaceable: e.k === '3do' || features.length > 0 };
  }

  /* Links each original model and feature picture to the remaster's model
     for it, the way Stars of Darien looks one up (docs/STUDIO.md, Drop-in
     models): a unit by its unit name then its model name, a feature by its
     name, then its sequence name, then its model name. Sets e.rm to the
     remaster entry's id, e.rmable when one could exist, and e.title. On a
     remaster entry, e.orig lists the originals it stands for. */
  function linkRemasters(entries, book, sodEntries) {
    var units = Object.create(null), feats = Object.create(null), byId = Object.create(null);
    (sodEntries || []).forEach(function (s) {
      s.orig = [];
      byId[s.id] = s;
      (s.sod === 'feature' ? feats : units)[lower(s.name)] = s;
    });
    var linked = 0;
    entries.forEach(function (e) {
      if (e.pkg === SOD) return;
      var n = namesOf(e, book);
      e.title = n.title || undefined;
      e.rmable = n.replaceable || undefined;
      e.rm = undefined;
      if (!n.replaceable) return;
      var hit = null, i;
      for (i = 0; !hit && i < n.units.length; i++) hit = units[lower(n.units[i].unit)];
      if (!hit && e.k === '3do') hit = units[lower(e.name)];
      for (i = 0; !hit && i < n.features.length; i++) hit = feats[lower(n.features[i].name)];
      if (!hit && e.seq) hit = feats[lower(e.seq)];
      if (!hit && e.k === '3do') hit = feats[lower(e.name)];
      if (hit) {
        e.rm = hit.id;
        if (hit.orig.length < 50) hit.orig.push(e.id);
        linked++;
      }
    });
    return linked;
  }

  /* ── palettes ─────────────────────────────────────────────────────── */

  var KINGDOMS = { aramon: 'Aramon', taros: 'Taros', veruna: 'Veruna', zhon: 'Zhon' };
  var PREFIX = { ara: 'aramon', tar: 'taros', ver: 'veruna', zon: 'zhon', zho: 'zhon' };

  var TEXTURE_PREFIX = { aramon: 'ara', ara: 'ara', taros: 'tar', tar: 'tar', veruna: 'ver', ver: 'ver',
                         zhon: 'zon', zon: 'zon', npc: 'npc' };

  function kingdomOf(name) { return PREFIX[lower(name).slice(0, 3)] || ''; }

  /* The palette an entry is first shown in, as the game would draw it,
     and a note on why: {spec, why}. spec is what the worker takes. */
  function paletteChoice(e, names) {
    if (e.k !== 'gaf') return null;
    if (/^textures\//.test(e.path))
      return { spec: { rule: 1 }, why: 'A 3DO texture. The engine picks its side\u2019s _textures palette by the file\u2019s name, as its texture atlas does.' };
    var f = names && names.features && names.features[0];
    if (f) {
      var w = lower(f.world).replace(/\s+/g, '');
      if (KINGDOMS[w])
        return { spec: { file: 'data/palettes/' + w + '_features.pcx', transparent: 0 },
                 why: 'A feature of ' + KINGDOMS[w] + ', drawn in that kingdom\u2019s feature palette.' };
      return { spec: { file: 'data/palettes/aramon_features.pcx', transparent: 0 },
               why: 'A feature of every kingdom. A battle draws it in the palette of the map\u2019s kingdom, so pick another to see it there.' };
    }
    return { spec: { rule: 0 }, why: 'The palette the engine looks up for this file: the .pcx beside it, or else the one its name calls for.' };
  }

  /* The palette a 3DO's flat colours come from: the map's kingdom in a
     battle, so the one the model's name suggests. */
  function flatPalette(e) {
    var k = kingdomOf(e.name);
    return 'data/palettes/' + (k || 'gameart') + '.pcx';
  }

  /* Where the picture an okPaint material names comes from, as the
     remaster's engine finds it (ok_embed.c okx_sprite_by_name and
     okx_texture_by_name): {seq, file, pcx, corner} for the page to find,
     or null. */
  function paintSource(spec, book) {
    if (!spec || !spec.name) return null;
    var world = lower(spec.world);
    if (spec.kind === 'texture') {
      var pre = TEXTURE_PREFIX[world] || '';
      return { texture: lower(spec.name), pcx: pre ? 'data/palettes/' + pre + '_textures.pcx' : '', corner: true };
    }
    if (spec.kind !== 'feature') return null;
    var f = book.features[lower(spec.name)];
    if (!f || f.object) {
      var seqs = book.featuresBySeq[lower(spec.name)] || [];
      f = seqs.filter(function (x) { return !x.object; })[0] || f;
    }
    if (!f || !f.file || !f.seq) return null;
    var k = KINGDOMS[world] ? world : KINGDOMS[lower(f.world)] ? lower(f.world) : 'aramon';
    return { seq: lower(f.seq), file: lower(f.file), pcx: 'data/palettes/' + k + '_features.pcx', corner: false };
  }

  /* ── search ───────────────────────────────────────────────────────── */

  function tokens(q) {
    var out = [], re = /"([^"]*)"|(\S+)/g, m;
    while ((m = re.exec(String(q || ''))) !== null) {
      var t = lower(m[1] !== undefined ? m[1] : m[2]).trim();
      if (t) out.push(t);
    }
    return out;
  }

  /* The text a search looks through for an entry. */
  function haystack(e, pkgName, note, community) {
    var parts = [e.name, e.seq, e.path, pkgName, e.title, e.k, e.fmt];
    [note, community].forEach(function (n) {
      if (!n) return;
      parts.push(n.note);
      if (n.tags) parts.push(n.tags.join(' '));
    });
    return lower(parts.filter(Boolean).join(' \u0001 '));
  }

  /* Entries matching every word of the query and every filter. filters:
     kind 'texture'|'model'|'', source 'official'|'custom'|'remaster'|'',
     remaster 'has'|'needs'|'', rating 0..5 (at least), pkgs a set of
     package keys or null. hay(e) and rate(e) come from the page. */
  function search(entries, q, filters, hay, rate, pkgOf) {
    var words = tokens(q), f = filters || {};
    var out = [];
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      if (f.kind && KIND_GROUP[e.k] !== f.kind) continue;
      if (f.pkgs && !f.pkgs[e.pkg]) continue;
      if (f.source) {
        var p = pkgOf(e.pkg);
        var src = e.pkg === SOD ? 'remaster' : p && p.official ? 'official' : 'custom';
        if (src !== f.source) continue;
      }
      if (f.remaster === 'has' && !e.rm && !(e.pkg === SOD && e.orig && e.orig.length)) continue;
      if (f.remaster === 'needs' && (!e.rmable || e.rm)) continue;
      if (f.rating && (rate(e) || 0) < f.rating) continue;
      if (words.length) {
        var h = hay(e), ok = true;
        for (var w = 0; w < words.length && ok; w++) ok = h.indexOf(words[w]) >= 0;
        if (!ok) continue;
      }
      out.push(e);
    }
    return out;
  }

  var SORTS = {
    name: function (a, b) { return cmp(lower(a.name), lower(b.name)) || cmp(a.id, b.id); },
    size: function (a, b) { return (b.w * b.h || b.tris || 0) - (a.w * a.h || a.tris || 0) || cmp(a.id, b.id); },
    frames: function (a, b) { return (b.f || 0) - (a.f || 0) || cmp(a.id, b.id); },
    path: function (a, b) { return cmp(a.pkg + a.path + (a.ei || 0) / 1e6, b.pkg + b.path + (b.ei || 0) / 1e6) || cmp(a.id, b.id); }
  };
  function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }

  /* ── notes ────────────────────────────────────────────────────────── */

  var NOTE_MAX = 4000, TAG_MAX = 40, TAGS_MAX = 20;

  function cleanText(s, max) {
    return String(s == null ? '' : s).replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, '').slice(0, max);
  }
  function cleanTags(tags) {
    var out = [], seen = Object.create(null);
    (Array.isArray(tags) ? tags : String(tags || '').split(','))
      .forEach(function (t) {
        var c = cleanText(t, TAG_MAX).replace(/\s+/g, ' ').trim().toLowerCase();
        if (c && !seen[c] && out.length < TAGS_MAX) { seen[c] = 1; out.push(c); }
      });
    return out.sort();
  }
  function validTime(s) { return typeof s === 'string' && !isNaN(Date.parse(s)) ? new Date(Date.parse(s)).toISOString() : ''; }

  /* A note as it is kept, or null when it says nothing. */
  function cleanNote(n) {
    if (!n || typeof n !== 'object') return null;
    var r = Math.round(Number(n.rating) || 0);
    var out = { note: cleanText(n.note, NOTE_MAX).trim(), rating: r >= 1 && r <= 5 ? r : 0,
                tags: cleanTags(n.tags), at: validTime(n.at) };
    return out.note || out.rating || out.tags.length ? out : null;
  }

  function validKey(k) { return typeof k === 'string' && k.length < 600 && !!splitKey(k) && k === lower(k); }

  /* A notes file read into {key: note}, or a reason it cannot be. */
  function parseNotes(text) {
    var j;
    try { j = typeof text === 'string' ? JSON.parse(text) : text; } catch (e) { return { error: 'That file is not JSON.' }; }
    if (!j || j.okAtlasNotes !== 1 || !j.notes || typeof j.notes !== 'object' || Array.isArray(j.notes))
      return { error: 'That file is not an Asset Atlas notes file.' };
    var out = Object.create(null), n = 0, bad = 0;
    for (var k in j.notes) {
      var c = validKey(k) ? cleanNote(j.notes[k]) : null;
      if (c) { out[k] = c; n++; } else bad++;
    }
    return { notes: out, count: n, skipped: bad };
  }

  function exportNotes(notes, now) {
    var keys = Object.keys(notes).sort(), out = {};
    keys.forEach(function (k) { var c = cleanNote(notes[k]); if (c) out[k] = c; });
    return JSON.stringify({ okAtlasNotes: 1, exported: (now || new Date()).toISOString(), notes: out }, null, 1) + '\n';
  }

  /* Merges incoming notes into mine. A note you do not have is added.
     Where both have one, the text and the rating come from whichever was
     edited later, and the tags of both are kept. Returns {notes, added,
     changed}; mine is not touched. */
  function mergeNotes(mine, incoming) {
    var out = Object.create(null), added = 0, changed = 0, k;
    for (k in mine) out[k] = mine[k];
    for (k in incoming) {
      var b = cleanNote(incoming[k]);
      if (!b || !validKey(k)) continue;
      var a = out[k];
      if (!a) { out[k] = b; added++; continue; }
      var later = b.at && (!a.at || b.at > a.at);
      var m = { note: later && b.note ? b.note : a.note || b.note,
                rating: later && b.rating ? b.rating : a.rating || b.rating,
                tags: cleanTags(a.tags.concat(b.tags)),
                at: later ? b.at : a.at || b.at };
      if (m.note !== a.note || m.rating !== a.rating || m.tags.join() !== a.tags.join()) changed++;
      out[k] = m;
    }
    return { notes: out, added: added, changed: changed };
  }

  /* ── what a replacement is called ─────────────────────────────────── */

  function issueUrl(replaces, kind, notes) {
    var q = ['template=model.yml', 'title=' + encodeURIComponent('[model] ' + replaces),
             'replaces=' + encodeURIComponent(replaces), 'kind=' + encodeURIComponent(kind)];
    if (notes) q.push('notes=' + encodeURIComponent(notes));
    return 'https://github.com/' + SOD_REPO + '/issues/new?' + q.join('&');
  }

  /* What a replacement for e must be called and where it goes, for the
     engine (docs/CUSTOM_MODELS.md, docs/MODDING.md) and for Stars of Darien
     (docs/CONTRIBUTING-MODELS.md). names is namesOf(e, book). */
  function contribute(e, names) {
    var out = { engine: [], sod: [], issue: '' };
    var units = names.units || [], features = names.features || [];
    if (e.k === '3do') {
      var obj = lower(e.name);
      out.engine.push({ path: 'models3d/' + obj + '.glb', note: 'A glTF binary in your game folder, drawn in the 3D view. One unit is one world pixel.' });
      out.engine.push({ path: 'objects3d/' + obj + '.3do', note: 'A 3DO in your game folder replaces the shipped one in both views.' });
      if (units.length) {
        var u = units[0].unit.toUpperCase();
        out.sod.push({ path: 'unity/Assets/Overrides/Units/' + u + '.glb', note: 'The whole unit. Name its parts after the original’s pieces to follow its animation. One unit is one cell, 16 pixels.' });
        out.issue = issueUrl(u, 'Unit', 'From the Asset Atlas: ' + e.id);
      } else if (features.length) {
        out.sod.push({ path: 'unity/Assets/Overrides/Features/' + features[0].name + '.glb', note: 'Scenery, named after the feature. One unit is one cell, 16 pixels.' });
        out.issue = issueUrl(features[0].name, 'Scenery', 'From the Asset Atlas: ' + e.id);
      } else {
        out.sod.push({ path: 'unity/Assets/Overrides/Units/' + e.name.toUpperCase() + '.glb', note: 'Named after the model. One unit is one cell, 16 pixels.' });
        out.issue = issueUrl(e.name.toUpperCase(), 'Not sure', 'From the Asset Atlas: ' + e.id);
      }
    } else if (features.length && e.seq) {
      out.engine.push({ path: 'models3d/' + lower(e.seq) + '.glb', note: 'A flat feature takes a model named after its sequence in the 3D view.' });
      out.engine.push({ path: 'Mods/<your mod>/' + e.path, note: 'A new picture goes in a folder mod at the same path, with the same sequence names and frame counts.' });
      out.sod.push({ path: 'unity/Assets/Overrides/Features/' + features[0].name + '.glb', note: 'Scenery, named after the feature. One unit is one cell, 16 pixels.' });
      out.issue = issueUrl(features[0].name, 'Scenery', 'From the Asset Atlas: ' + e.id);
    } else if (e.pkg !== SOD) {
      out.engine.push({ path: 'Mods/<your mod>/' + e.path, note: 'A folder mod at the same path replaces it' + (e.seq ? ', with the same sequence names and frame counts.' : '.') });
    }
    return out;
  }

  /* ── zip ──────────────────────────────────────────────────────────── */

  var CRC_TABLE = (function () {
    var t = new Uint32Array(256);
    for (var n = 0; n < 256; n++) {
      var c = n;
      for (var k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(b) {
    var c = 0xFFFFFFFF;
    for (var i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  function utf8(s) { return new TextEncoder().encode(s); }

  /* A zip of [{name, bytes}], stored without compression since the files
     are already compressed pictures. */
  function zipStore(files, when) {
    var d = when || new Date(2000, 0, 1);
    var time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
    var date = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
    var parts = [], central = [], at = 0;
    function le(n, size) { var a = new Uint8Array(size); for (var i = 0; i < size; i++) a[i] = (n >>> (8 * i)) & 0xFF; return a; }
    function cat(list) {
      var n = 0; list.forEach(function (x) { n += x.length; });
      var o = new Uint8Array(n), p = 0;
      list.forEach(function (x) { o.set(x, p); p += x.length; });
      return o;
    }
    files.forEach(function (f) {
      var name = utf8(f.name), data = f.bytes, crc = crc32(data);
      var common = [le(20, 2), le(0x800, 2), le(0, 2), le(time, 2), le(date, 2), le(crc, 4), le(data.length, 4), le(data.length, 4), le(name.length, 2), le(0, 2)];
      var local = cat([le(0x04034b50, 4)].concat(common, [name]));
      central.push(cat([le(0x02014b50, 4), le(20, 2)].concat(common, [le(0, 2), le(0, 2), le(0, 2), le(0, 4), le(at, 4), name])));
      parts.push(local, data);
      at += local.length + data.length;
    });
    var cd = cat(central);
    var end = cat([le(0x06054b50, 4), le(0, 2), le(0, 2), le(files.length, 2), le(files.length, 2), le(cd.length, 4), le(at, 4), le(0, 2)]);
    return cat(parts.concat([cd, end]));
  }

  /* ── GLB ──────────────────────────────────────────────────────────── */

  /* A glTF binary from groups of triangles: [{name, node, pos (Float32Array
     xyz in model space), uv, png (Uint8Array or null), color [r,g,b,a],
     alpha}]. opts.nodes is the piece tree [{name, parent, t: [x,y,z]}],
     each piece offset from its parent, so a 3DO keeps its pieces and their
     names. opts.scale turns model units into the file's. */
  function writeGlb(groups, opts) {
    opts = opts || {};
    var scale = opts.scale || 1;
    var nodes = opts.nodes && opts.nodes.length ? opts.nodes : [{ name: opts.name || 'model', parent: -1, t: [0, 0, 0] }];
    var bin = [], binLen = 0;
    function push(arr) {
      var bytes = new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength);
      var off = binLen;
      bin.push(bytes);
      binLen += bytes.length;
      var pad = (4 - (binLen % 4)) % 4;
      if (pad) { bin.push(new Uint8Array(pad)); binLen += pad; }
      return { off: off, len: bytes.length };
    }
    var abs = nodes.map(function () { return null; });
    function absOf(i) {
      if (abs[i]) return abs[i];
      var n = nodes[i], p = n.parent >= 0 && n.parent < i ? absOf(n.parent) : [0, 0, 0];
      return (abs[i] = [p[0] + n.t[0], p[1] + n.t[1], p[2] + n.t[2]]);
    }
    var j = { asset: { version: '2.0', generator: 'OpenKingdoms Asset Atlas' }, scene: 0, scenes: [{ nodes: [] }],
              nodes: [], meshes: [], materials: [], accessors: [], bufferViews: [], buffers: [] };
    nodes.forEach(function (n, i) {
      var node = { name: n.name || 'piece' + i };
      var t = [n.t[0] * scale, n.t[1] * scale, n.t[2] * scale];
      if (t[0] || t[1] || t[2]) node.translation = t;
      j.nodes.push(node);
      if (n.parent >= 0 && n.parent < i) (j.nodes[n.parent].children = j.nodes[n.parent].children || []).push(i);
      else j.scenes[0].nodes.push(i);
    });
    if (opts.extras) j.nodes[j.scenes[0].nodes[0]].extras = opts.extras;
    var images = [], meshOf = {};
    groups.forEach(function (g, gi) {
      var ni = g.node >= 0 && g.node < nodes.length ? g.node : 0, o = absOf(ni);
      var n = g.pos.length / 3, local = new Float32Array(g.pos.length);
      var min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
      for (var i = 0; i < g.pos.length; i += 3)
        for (var a = 0; a < 3; a++) {
          var v = (g.pos[i + a] - o[a]) * scale;
          local[i + a] = v;
          if (v < min[a]) min[a] = v;
          if (v > max[a]) max[a] = v;
        }
      var pv = push(local);
      j.bufferViews.push({ buffer: 0, byteOffset: pv.off, byteLength: pv.len, target: 34962 });
      j.accessors.push({ bufferView: j.bufferViews.length - 1, componentType: 5126, count: n, type: 'VEC3', min: min, max: max });
      var attrs = { POSITION: j.accessors.length - 1 };
      if (g.normal) {
        var nv = push(g.normal);
        j.bufferViews.push({ buffer: 0, byteOffset: nv.off, byteLength: nv.len, target: 34962 });
        j.accessors.push({ bufferView: j.bufferViews.length - 1, componentType: 5126, count: n, type: 'VEC3' });
        attrs.NORMAL = j.accessors.length - 1;
      }
      var mat = { name: g.name || 'material' + gi, pbrMetallicRoughness: { metallicFactor: 0, roughnessFactor: 0.9 },
                  doubleSided: true };
      if (g.png && g.uv) {
        var uv = push(g.uv);
        j.bufferViews.push({ buffer: 0, byteOffset: uv.off, byteLength: uv.len, target: 34962 });
        j.accessors.push({ bufferView: j.bufferViews.length - 1, componentType: 5126, count: n, type: 'VEC2' });
        attrs.TEXCOORD_0 = j.accessors.length - 1;
        var im = images.indexOf(g.png);
        if (im < 0) { images.push(g.png); im = images.length - 1; }
        mat.pbrMetallicRoughness.baseColorTexture = { index: im };
        if (g.alpha) mat.alphaMode = 'MASK';
      } else if (g.color) {
        mat.pbrMetallicRoughness.baseColorFactor = g.color;
      }
      var mi = j.materials.findIndex ? j.materials.findIndex(function (m) { return m.name === mat.name; }) : -1;
      if (mi < 0) { j.materials.push(mat); mi = j.materials.length - 1; }
      if (meshOf[ni] === undefined) {
        j.meshes.push({ name: j.nodes[ni].name, primitives: [] });
        meshOf[ni] = j.meshes.length - 1;
        j.nodes[ni].mesh = meshOf[ni];
      }
      j.meshes[meshOf[ni]].primitives.push({ attributes: attrs, material: mi, mode: 4 });
    });
    if (images.length) {
      j.images = [];
      j.textures = [];
      j.samplers = [{ magFilter: 9728, minFilter: 9728 }];
      images.forEach(function (png, i) {
        var v = push(png);
        j.bufferViews.push({ buffer: 0, byteOffset: v.off, byteLength: v.len });
        j.images.push({ bufferView: j.bufferViews.length - 1, mimeType: 'image/png' });
        j.textures.push({ source: i, sampler: 0 });
      });
    }
    if (!j.meshes.length) delete j.meshes;
    if (!j.materials.length) delete j.materials;
    j.buffers.push({ byteLength: binLen });
    var json = utf8(JSON.stringify(j));
    var jpad = (4 - (json.length % 4)) % 4;
    var total = 12 + 8 + json.length + jpad + 8 + binLen;
    var out = new Uint8Array(total), dv = new DataView(out.buffer), p = 0;
    dv.setUint32(0, 0x46546C67, true); dv.setUint32(4, 2, true); dv.setUint32(8, total, true);
    dv.setUint32(12, json.length + jpad, true); dv.setUint32(16, 0x4E4F534A, true);
    out.set(json, 20);
    for (p = 20 + json.length; p < 20 + json.length + jpad; p++) out[p] = 0x20;
    dv.setUint32(p, binLen, true); dv.setUint32(p + 4, 0x004E4942, true);
    p += 8;
    bin.forEach(function (b) { out.set(b, p); p += b.length; });
    return out;
  }

  /* {json, bin} of a glTF binary, or null. */
  function readGlb(bytes) {
    var b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (b.length < 20 || u32le(b, 0) !== 0x46546C67) return null;
    var len = u32le(b, 12);
    if (u32le(b, 16) !== 0x4E4F534A || 20 + len > b.length) return null;
    var json;
    try { json = JSON.parse(new TextDecoder().decode(b.subarray(20, 20 + len))); } catch (e) { return null; }
    var at = 20 + len, bin = null;
    if (at + 8 <= b.length && u32le(b, at + 4) === 0x004E4942) bin = b.subarray(at + 8, Math.min(b.length, at + 8 + u32le(b, at)));
    return { json: json, bin: bin };
  }

  /* Triangle count of a glTF's meshes. */
  function glbTriangles(json) {
    var n = 0, acc = json.accessors || [];
    (json.meshes || []).forEach(function (m) {
      (m.primitives || []).forEach(function (p) {
        if ((p.mode === undefined ? 4 : p.mode) !== 4) return;
        var a = p.indices !== undefined ? p.indices : (p.attributes || {}).POSITION;
        if (acc[a]) n += Math.floor(acc[a].count / 3);
      });
    });
    return n;
  }

  /* ── the grid ─────────────────────────────────────────────────────── */

  /* Which cells of a grid of count cells, cols across and rows rowH tall,
     a view from scrollTop height high shows, with spare rows either side. */
  function gridWindow(scrollTop, height, rowH, cols, count, spare) {
    cols = Math.max(1, cols | 0);
    var rows = Math.ceil(count / cols);
    var first = Math.max(0, Math.floor(scrollTop / rowH) - (spare || 0));
    var last = Math.min(rows - 1, Math.floor((scrollTop + height) / rowH) + (spare || 0));
    if (rows === 0 || last < first) return { first: 0, last: -1, rows: rows, total: rows * rowH };
    return { first: first * cols, last: Math.min(count - 1, (last + 1) * cols - 1), rows: rows, total: rows * rowH };
  }

  function sizeText(n) {
    n = +n || 0;
    if (n < 1024) return n + ' B';
    if (n < 1048576) return Math.round(n / 1024) + ' KB';
    return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
  }
  function count(n, one, many) { return n.toLocaleString('en-US') + ' ' + (n === 1 ? one : many || one + 's'); }

  return {
    INDEX_VERSION: INDEX_VERSION,
    SOD: SOD,
    SOD_REPO: SOD_REPO,
    ENGINE_REPO: ENGINE_REPO,
    KIND_GROUP: KIND_GROUP,
    FMT_CODE: FMT_CODE,
    SORTS: SORTS,
    lower: lower,
    baseName: baseName,
    stem: stem,
    ext: ext,
    packageInfo: packageInfo,
    mountOrder: mountOrder,
    displayOrder: displayOrder,
    isArchive: isArchive,
    classify: classify,
    noteKey: noteKey,
    uid: uid,
    splitKey: splitKey,
    parseTdf: parseTdf,
    unitFromFbi: unitFromFbi,
    featuresFromTdf: featuresFromTdf,
    imageSize: imageSize,
    parseGafInfo: parseGafInfo,
    gafEntries: gafEntries,
    fileEntry: fileEntry,
    isTexture: isTexture,
    isModel: isModel,
    parseRemaster: parseRemaster,
    nameBook: nameBook,
    namesOf: namesOf,
    linkRemasters: linkRemasters,
    KINGDOMS: KINGDOMS,
    kingdomOf: kingdomOf,
    paletteChoice: paletteChoice,
    flatPalette: flatPalette,
    paintSource: paintSource,
    tokens: tokens,
    haystack: haystack,
    search: search,
    cleanNote: cleanNote,
    cleanTags: cleanTags,
    parseNotes: parseNotes,
    exportNotes: exportNotes,
    mergeNotes: mergeNotes,
    contribute: contribute,
    issueUrl: issueUrl,
    crc32: crc32,
    zipStore: zipStore,
    writeGlb: writeGlb,
    readGlb: readGlb,
    glbTriangles: glbTriangles,
    gridWindow: gridWindow,
    sizeText: sizeText,
    count: count
  };
});
