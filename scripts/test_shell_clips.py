#!/usr/bin/env python3
"""Which clips web/shell.html carries into the engine.

The page decides with isClip, a handful of regular expressions. This
reads them out of the page and holds them to the names a shipped install
has, so a clip the engine asks for is one the page will have handed it.
No browser and no game data needed.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHELL = os.path.join(ROOT, 'web', 'shell.html')


def function_body(text, name):
    start = text.index('function %s(' % name)
    depth = 0
    i = text.index('{', start)
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    raise ValueError('no end to ' + name)


def patterns(body):
    # /.../.test(r), with the slashes inside written as \/
    return [re.compile(p.replace('\\/', '/'))
            for p in re.findall(r'/((?:\\.|[^/\\\n])+)/\.test\(', body)]


def main():
    text = open(SHELL, encoding='utf-8').read()
    clip = patterns(function_body(text, 'isClip'))
    try:
        clip += patterns(function_body(text, 'isMissionClip'))
    except ValueError:
        pass

    def carried(rel):
        r = rel.lower()
        return any(p.search(r) for p in clip)

    # Names as a shipped install spells them, under Movies/.
    want = [
        'Movies/logo.bik', 'Movies/Intro.bik', 'Movies/Credits.bik',
        'Movies/Gui/Aramon.bik',
        # Before a mission (legacy:168662), both campaigns, mixed case.
        'Movies/takmission01_mt.bik', 'Movies/TakMission02_mt.bik',
        'Movies/takmission39a_mt.bik', 'Movies/takmission48_dh.bik',
        'Movies/Takx03_ph.bik', 'Movies/Takx20_mt.bik',
        # After one (legacy:168719).
        'Movies/posttakmission24_mt.bik',
    ]
    leave = [
        'Movies/readme.txt', 'Movies/Gui/notes.txt',
        'Movies/Gui/deeper/Aramon.bik', 'Music/takmission01_mt.bik',
        'takmission01_mt.bik',
    ]
    failed = 0
    for rel in want:
        if not carried(rel):
            print('FAIL  not carried: ' + rel)
            failed += 1
    for rel in leave:
        if carried(rel):
            print('FAIL  carried and should not be: ' + rel)
            failed += 1
    print('ran %d checks' % (len(want) + len(leave)))
    if failed:
        print('%d failed' % failed)
        return 1
    print('the page carries the clips the engine asks for')
    return 0


if __name__ == '__main__':
    sys.exit(main())
