/*
 * leaderboard.js -- what the leaderboard page works out, apart from
 * drawing it: routes and their filters, the query the relay is asked,
 * names that need telling apart, and the live games.
 *
 * No DOM in here, so web/test_leaderboard.js runs it in node.
 */
(function (root, factory) {
  'use strict';
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OKBoard = api;
})(this, function () {
  'use strict';

  var FILTER_KEYS = ['q', 'map', 'from', 'to', 'table'];
  var DAY = /^(\d{4})-(\d{2})-(\d{2})$/;
  /* A table id as the relay writes one: the mod's name folded, a dash
     and its fingerprint, or "earlier". */
  var TABLE = /^[a-z0-9-]{1,64}$/;

  /* The relay's https address from the relay.txt beside the page, or
     the page's own origin when there is none. */
  function relayApi(text, origin) {
    var url = (text || '').split(/[\r\n]/)[0].trim();
    url = url.replace(/^wss:\/\//i, 'https://').replace(/^ws:\/\//i, 'http://');
    url = url.replace(/\/relay\/?$/, '').replace(/\/+$/, '');
    if (!/^https?:\/\/[^\s]+$/.test(url)) url = origin;
    return url + '/api';
  }

  /* Only the filters a view takes, trimmed, dates only when they are
     dates, so a hand typed hash cannot put anything odd in a query. */
  function cleanFilters(f) {
    var out = {};
    FILTER_KEYS.forEach(function (k) {
      var v = f && f[k] != null ? String(f[k]).trim() : '';
      if (!v) return;
      if ((k === 'from' || k === 'to') && !DAY.test(v)) return;
      if (k === 'table' && !TABLE.test(v)) return;
      if (k === 'q') v = v.slice(0, 15);
      if (k === 'map') v = v.slice(0, 63);
      out[k] = v;
    });
    return out;
  }

  /* Keyed by whatever the address holds, so no prototype to write onto. */
  function parseQuery(text) {
    var out = Object.create(null);
    (text || '').split('&').forEach(function (pair) {
      if (!pair) return;
      var i = pair.indexOf('=');
      var k = i < 0 ? pair : pair.slice(0, i);
      var v = i < 0 ? '' : pair.slice(i + 1);
      try { out[decodeURIComponent(k)] = decodeURIComponent(v.replace(/\+/g, ' ')); } catch (e) {}
    });
    return out;
  }

  function encodeQuery(f) {
    return FILTER_KEYS.filter(function (k) { return f[k]; })
      .map(function (k) { return k + '=' + encodeURIComponent(f[k]); }).join('&');
  }

  /* #/, #/games, #/player/<id> and #/game/<n>, each with its filters
     after a ?. */
  function parseRoute(hash) {
    var h = hash || '#/';
    var q = h.indexOf('?');
    var path = q < 0 ? h : h.slice(0, q);
    var filters = cleanFilters(parseQuery(q < 0 ? '' : h.slice(q + 1)));
    var m;
    if ((m = /^#\/player\/([0-9a-f]{16})$/i.exec(path)))
      return { view: 'player', id: m[1].toLowerCase(), filters: filters };
    if ((m = /^#\/game\/(\d+)$/.exec(path))) return { view: 'game', id: m[1], filters: {} };
    if (path === '#/games') return { view: 'games', filters: filters };
    var board = {};
    if (filters.q) board.q = filters.q;
    if (filters.table) board.table = filters.table;
    return { view: 'board', filters: board };
  }

  function routeHash(route) {
    var base = route.view === 'player' ? '#/player/' + route.id
             : route.view === 'game' ? '#/game/' + route.id
             : route.view === 'games' ? '#/games' : '#/';
    var q = encodeQuery(cleanFilters(route.filters || {}));
    return q ? base + '?' + q : base;
  }

  /* A day typed in a date box is the reader's own day, from its first
     millisecond to its last. */
  function dayStart(day) {
    var m = DAY.exec(day || '');
    return m ? new Date(+m[1], +m[2] - 1, +m[3]).getTime() : 0;
  }
  function dayEnd(day) {
    var m = DAY.exec(day || '');
    return m ? new Date(+m[1], +m[2] - 1, +m[3] + 1).getTime() - 1 : 0;
  }

  /* What the relay is asked for a view's filters, to put after a ? or
     an &. Empty when nothing is filtered. */
  function apiQuery(filters) {
    var f = cleanFilters(filters);
    var parts = [];
    if (f.q) parts.push('q=' + encodeURIComponent(f.q));
    if (f.map) parts.push('map=' + encodeURIComponent(f.map));
    if (f.from) parts.push('from=' + dayStart(f.from));
    if (f.to) parts.push('to=' + dayEnd(f.to));
    if (f.table) parts.push('table=' + f.table);
    return parts.join('&');
  }

  /* Whether a search or filter narrows the list. The table is which
     list it is, not a narrowing of it. */
  function filtered(filters) {
    return Object.keys(cleanFilters(filters)).some(function (k) { return k !== 'table'; });
  }

  /* ---- tables ---------------------------------------------------------- */

  /* What a table is called on the page: the mod set and its version,
     Vanilla for the game itself, and the games from before tables by
     what they are. */
  function tableLabel(t) {
    if (!t) return '';
    if (t.earlier) return 'Earlier games';
    var name = String(t.name || '').trim();
    if (!name) return 'Unnamed data ' + String(t.fingerprint || '').slice(0, 8);
    if (t.vanilla) return 'Vanilla';
    var v = String(t.version || '').trim();
    return v ? name + ' ' + v : name;
  }

  /* /api/tables as the picker's choices, in the relay's order. Two
     tables that would read alike, vanilla on two releases say, are told
     apart by the start of their fingerprints. */
  function tableChoices(d) {
    var tables = (d && d.tables) || [];
    var counts = new Map();
    tables.forEach(function (t) {
      var l = tableLabel(t).toLowerCase();
      counts.set(l, (counts.get(l) || 0) + 1);
    });
    return tables.map(function (t) {
      var label = tableLabel(t);
      if (counts.get(label.toLowerCase()) > 1 && t.fingerprint)
        label += ' \u00b7' + String(t.fingerprint).slice(0, 8);
      return { id: t.id, label: label, games: t.games || 0 };
    });
  }

  /* The table a view shows: the one the address names when the relay
     has it, else the relay's default. '' when the relay keeps no
     tables, and then every game is one list, as before tables. */
  function pickTable(d, wanted) {
    var tables = (d && d.tables) || [];
    if (!tables.length) return '';
    for (var i = 0; i < tables.length; i++) if (tables[i].id === wanted) return wanted;
    return (d && d.default) || tables[0].id;
  }

  /* The mod set a live game plays, for its row. */
  function modLabel(name, version) {
    var n = String(name || '').trim();
    if (!n) return '';
    var v = String(version || '').trim();
    return v ? n + ' ' + v : n;
  }

  /* A player's name, and when two players on the page go by the same
     one, the end of their id after it, so the reader can tell them
     apart. `people` is anything with id and name. */
  function nameLabels(people) {
    /* Names are whatever a player typed, __proto__ included, so they only
       ever key a Map, and the answer is an object with no prototype. */
    var ids = new Map(), byName = new Map();
    people.forEach(function (p) {
      if (!p || !p.id || ids.has(p.id)) return;
      var name = String(p.name);
      ids.set(p.id, name);
      var k = name.toLowerCase();
      if (!byName.has(k)) byName.set(k, new Set());
      byName.get(k).add(p.id);
    });
    var out = Object.create(null);
    ids.forEach(function (name, id) {
      var shared = byName.get(name.toLowerCase()).size > 1;
      out[id] = shared ? name + ' \u00b7' + String(id).slice(-4) : name;
    });
    return out;
  }

  /* A seat as the page shows it: the name it played under, and the
     name its player goes by now when that is different. */
  function seatName(seat) {
    return seat.current ? seat.name + ' (now ' + seat.current + ')' : seat.name;
  }

  /* Every seat of one game, labelled, with two players who go by one
     name told apart the way the table tells them apart. */
  function seatLabels(seats) {
    var labels = nameLabels(seats.filter(function (s) { return s.player; })
      .map(function (s) { return { id: s.player, name: s.current || s.name }; }));
    return seats.map(function (s) {
      var text = seatName(s);
      if (!s.player) return text;
      var shown = String(s.current || s.name);
      var label = labels[s.player];
      return typeof label === 'string' ? text + label.slice(shown.length) : text;
    });
  }

  function plural(n, one, many) { return n + ' ' + (n === 1 ? one : many); }

  function playingFor(secs) {
    var m = Math.floor((secs || 0) / 60);
    if (m < 1) return 'just started';
    if (m < 60) return m + ' min in';
    return Math.floor(m / 60) + ' h ' + (m % 60) + ' min in';
  }

  /* /api/rooms as two lists, the games that can be joined or are
     filling and the games being played, with the words each row shows. */
  function liveGames(d) {
    var rooms = (d && d.rooms) || [];
    var open = [], playing = [];
    rooms.forEach(function (r) {
      var row = {
        code: r.code,
        title: r.name || ((r.host || 'Someone') + '’s game'),
        map: r.map || '',
        host: r.host || '',
        seats: r.players + ' of ' + r.max + ' seats',
        mod: modLabel(r.mod, r.mod_version),
        rules: r.remastered === true ? 'remastered battlefield' : '',
        watchers: r.watchers || 0,
        password: !!r.password,
        watchable: !!r.watchable
      };
      if (r.status === 'playing') {
        row.state = playingFor(r.playing_secs);
        row.can_watch = row.watchable && row.watchers < 8;
        playing.push(row);
      } else {
        row.joinable = r.status === 'open' && r.players < r.max;
        row.state = r.status === 'open' ? (row.joinable ? 'open' : 'full') : 'starting';
        open.push(row);
      }
    });
    var summary = plural(d && d.online || 0, 'player', 'players') + ' online, ' +
      plural(open.length, 'game', 'games') + ' open, ' +
      plural(playing.length, 'game', 'games') + ' being played';
    return { open: open, playing: playing, summary: summary };
  }

  /* The game page joins a room from a ?join= link, and watches one under
     way from a ?watch= link. */
  function joinHref(code) {
    return /^[A-Za-z0-9]{3,12}$/.test(code || '') ? './?join=' + code : '';
  }
  function watchHref(code) {
    return /^[A-Za-z0-9]{3,12}$/.test(code || '') ? './?watch=' + code : '';
  }

  return {
    relayApi: relayApi,
    parseRoute: parseRoute,
    routeHash: routeHash,
    cleanFilters: cleanFilters,
    apiQuery: apiQuery,
    filtered: filtered,
    tableLabel: tableLabel,
    tableChoices: tableChoices,
    pickTable: pickTable,
    modLabel: modLabel,
    dayStart: dayStart,
    dayEnd: dayEnd,
    nameLabels: nameLabels,
    seatName: seatName,
    seatLabels: seatLabels,
    liveGames: liveGames,
    playingFor: playingFor,
    joinHref: joinHref,
    watchHref: watchHref
  };
});
