/*
 * test_leaderboard.js -- the leaderboard page's own logic, in node.
 *
 * No browser, no relay, no game data: node web/test_leaderboard.js.
 */
'use strict';

var assert = require('assert');
var path = require('path');
var B = require(path.join(__dirname, 'leaderboard.js'));

var passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log('  ' + name + ' PASS'); }
  catch (e) { failed++; console.log('  ' + name + ' FAIL\n    ' + (e && e.message)); }
}

console.log('The leaderboard page');

test('the relay address comes from relay.txt or the page itself', function () {
  assert.strictEqual(B.relayApi('wss://relay.example:8443/relay\n', 'https://site'), 'https://relay.example:8443/api');
  assert.strictEqual(B.relayApi('ws://10.0.0.2:9000/', 'https://site'), 'http://10.0.0.2:9000/api');
  assert.strictEqual(B.relayApi('', 'https://site'), 'https://site/api');
  assert.strictEqual(B.relayApi('<html>not found</html>', 'https://site'), 'https://site/api');
});

test('routes read their view, id and filters', function () {
  assert.deepStrictEqual(B.parseRoute(''), { view: 'board', filters: {} });
  assert.deepStrictEqual(B.parseRoute('#/?q=Zach'), { view: 'board', filters: { q: 'Zach' } });
  /* The table is searched by name only. */
  assert.deepStrictEqual(B.parseRoute('#/?q=a&map=b'), { view: 'board', filters: { q: 'a' } });
  assert.deepStrictEqual(B.parseRoute('#/games?q=Zed&map=Two%20Castles&from=2026-09-01&to=2026-09-30'),
    { view: 'games', filters: { q: 'Zed', map: 'Two Castles', from: '2026-09-01', to: '2026-09-30' } });
  assert.deepStrictEqual(B.parseRoute('#/player/00000000000000D1?map=x'),
    { view: 'player', id: '00000000000000d1', filters: { map: 'x' } });
  assert.deepStrictEqual(B.parseRoute('#/game/12'), { view: 'game', id: '12', filters: {} });
  assert.deepStrictEqual(B.parseRoute('#/player/zach'), { view: 'board', filters: {} });
});

test('a filter that is not what it says is dropped', function () {
  assert.deepStrictEqual(B.cleanFilters({ q: '  ', map: ' two ', from: 'yesterday', to: '2026-9-1', other: 'x' }),
    { map: 'two' });
  assert.strictEqual(B.cleanFilters({ q: 'a name far longer than fifteen' }).q.length, 15);
  assert.deepStrictEqual(B.parseRoute('#/games?q=%E0%A4%A'), { view: 'games', filters: {} });
});

test('a route and its hash go round', function () {
  var r = { view: 'games', filters: { q: 'Zed & co', map: 'Vain Blessings', from: '2026-09-01' } };
  var h = B.routeHash(r);
  assert.strictEqual(h, '#/games?q=Zed%20%26%20co&map=Vain%20Blessings&from=2026-09-01');
  assert.deepStrictEqual(B.parseRoute(h), r);
  assert.strictEqual(B.routeHash({ view: 'board', filters: {} }), '#/');
  assert.strictEqual(B.routeHash({ view: 'player', id: 'abcdabcdabcdabcd', filters: { to: '2026-01-02' } }),
    '#/player/abcdabcdabcdabcd?to=2026-01-02');
});

test('a day is the reader’s own, first millisecond to last', function () {
  var start = new Date(2026, 8, 1).getTime();
  var next = new Date(2026, 8, 2).getTime();
  assert.strictEqual(B.dayStart('2026-09-01'), start);
  assert.strictEqual(B.dayEnd('2026-09-01'), next - 1);
  assert.strictEqual(B.dayStart('nope'), 0);
  /* The last day of a month rolls into the next. */
  assert.strictEqual(B.dayEnd('2026-09-30'), new Date(2026, 9, 1).getTime() - 1);
});

test('the relay is asked for exactly the filters set', function () {
  assert.strictEqual(B.apiQuery({}), '');
  assert.strictEqual(B.filtered({}), false);
  assert.strictEqual(B.apiQuery({ q: 'Zé d', map: 'two+three' }), 'q=Z%C3%A9%20d&map=two%2Bthree');
  assert.strictEqual(B.apiQuery({ from: '2026-09-01', to: '2026-09-01' }),
    'from=' + new Date(2026, 8, 1).getTime() + '&to=' + (new Date(2026, 8, 2).getTime() - 1));
  assert.strictEqual(B.filtered({ map: 'x' }), true);
});

test('two players who go by one name are told apart', function () {
  var labels = B.nameLabels([
    { id: 'aaaaaaaaaaaa1111', name: 'Zach' },
    { id: 'bbbbbbbbbbbb2222', name: 'zach' },
    { id: 'cccccccccccc3333', name: 'Lokken' },
    { id: 'aaaaaaaaaaaa1111', name: 'Zach' },
    null
  ]);
  assert.strictEqual(labels.aaaaaaaaaaaa1111, 'Zach ·1111');
  assert.strictEqual(labels.bbbbbbbbbbbb2222, 'zach ·2222');
  assert.strictEqual(labels.cccccccccccc3333, 'Lokken');
});

test('a seat says the name its player goes by now', function () {
  assert.strictEqual(B.seatName({ name: 'Zach' }), 'Zach');
  assert.strictEqual(B.seatName({ name: 'Zach', current: 'Zed' }), 'Zach (now Zed)');
});

test('two seats in one game who go by one name are told apart', function () {
  assert.deepStrictEqual(B.seatLabels([
    { name: 'Zed', player: '000000000000aaaa' },
    { name: 'Zach', current: 'Zed', player: '000000000000bbbb' },
    { name: 'Lokken', player: '000000000000cccc' },
    { name: 'Computer', player: null }
  ]), ['Zed \u00b7aaaa', 'Zach (now Zed) \u00b7bbbb', 'Lokken', 'Computer']);
});

test('the live games split into open and being played', function () {
  var d = {
    online: 5, in_lobby: 2, rooms: [
      { code: 'ABC123', name: '', host: 'Zach', map: 'Two Castles', players: 1, max: 4, watchers: 0, status: 'open', password: false },
      { code: 'DEF456', name: 'Full up', host: 'Elsin', map: 'x', players: 2, max: 2, status: 'open', password: true },
      { code: 'GHI789', name: 'Big one', host: 'Lokken', map: 'Vain Blessings', players: 4, max: 4, watchers: 1, status: 'playing', playing_secs: 3725, watchable: true },
      { code: 'JKL012', name: '', host: '', map: '', players: 2, max: 2, status: 'starting' }
    ]
  };
  var g = B.liveGames(d);
  assert.strictEqual(g.summary, '5 players online, 3 games open, 1 game being played');
  assert.strictEqual(g.open.length, 3);
  assert.strictEqual(g.open[0].title, 'Zach’s game');
  assert.strictEqual(g.open[0].joinable, true);
  assert.strictEqual(g.open[0].seats, '1 of 4 seats');
  assert.strictEqual(g.open[1].state, 'full');
  assert.strictEqual(g.open[1].password, true);
  assert.strictEqual(g.open[2].state, 'starting');
  assert.strictEqual(g.open[2].title, 'Someone’s game');
  assert.strictEqual(g.playing[0].state, '1 h 2 min in');
  assert.strictEqual(g.playing[0].watchable, true);
  assert.strictEqual(B.liveGames({ online: 1, rooms: [] }).summary, '1 player online, 0 games open, 0 games being played');
  assert.strictEqual(B.liveGames(null).open.length, 0);
});

test('time in a game reads in minutes, then hours', function () {
  assert.strictEqual(B.playingFor(0), 'just started');
  assert.strictEqual(B.playingFor(59), 'just started');
  assert.strictEqual(B.playingFor(600), '10 min in');
  assert.strictEqual(B.playingFor(7200), '2 h 0 min in');
});

test('a join link is only ever a room code', function () {
  assert.strictEqual(B.joinHref('ABC123'), './?join=ABC123');
  assert.strictEqual(B.joinHref('a"b'), '');
  assert.strictEqual(B.joinHref(''), '');
  assert.strictEqual(B.joinHref(undefined), '');
});

console.log('Results: ' + passed + ' passed, ' + failed + ' failed, ' + (passed + failed) + ' total');
process.exit(failed ? 1 : 0);
