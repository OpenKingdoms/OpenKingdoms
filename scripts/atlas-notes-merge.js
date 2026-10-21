#!/usr/bin/env node
/* Merges Asset Atlas notes files into the community list the site serves,
   by the page's own rule (web/atlas.js mergeNotes): a note it lacks is
   added, the later edit of a note both have wins, and tags are joined.

     node scripts/atlas-notes-merge.js web/atlas/community.json notes.json...

   The first file is rewritten in place. */
'use strict';
const fs = require('fs');
const path = require('path');
const A = require(path.join(__dirname, '..', 'web', 'atlas.js'));

const files = process.argv.slice(2);
if (files.length < 2) {
  console.error('usage: node scripts/atlas-notes-merge.js <community.json> <notes.json>...');
  process.exit(2);
}
function read(f) {
  const got = A.parseNotes(fs.readFileSync(f, 'utf8'));
  if (got.error) { console.error(f + ': ' + got.error); process.exit(1); }
  if (got.skipped) console.error(f + ': ' + got.skipped + ' notes were not valid and were left out');
  return got.notes;
}
let notes = read(files[0]);
for (const f of files.slice(1)) {
  const m = A.mergeNotes(notes, read(f));
  notes = m.notes;
  console.log(f + ': ' + m.added + ' added, ' + m.changed + ' changed');
}
const out = JSON.parse(A.exportNotes(notes));
delete out.exported;
fs.writeFileSync(files[0], JSON.stringify(out, null, 1) + '\n');
console.log(files[0] + ': ' + Object.keys(notes).length + ' notes');
