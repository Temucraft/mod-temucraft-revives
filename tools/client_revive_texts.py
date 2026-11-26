#!/usr/bin/env python3
"""Puts this module's combat revives into a client Spell.dbc, so their tooltips say what the server does.

usage: client_revive_texts.py <Spell.dbc> [health mana cooldown_min  soul_health soul_mana soul_cost soul_cooldown_sec]

Spell.dbc is the client's (in the Temucraft release: patch-T.MPQ, PTR first); it is changed in place, pack it back
into the MPQ afterwards. Running it again is safe. The numbers default to the config defaults (Combat: 30% health,
20% mana, 10 min; Soulstone: 30%, 20%, 40% of maximum mana, 30 sec); pass yours if you changed them.

- Revive 51918 (tank and healer specs, in combat too): the stock row is the death knight's ("Resurrects a fallen
  death knight.", 100 yards, no cooldown). It gets a description for everyone, 40 yards and its cooldown.
- Revive 24341 (warlock soulstone revive): the stock row has no description; it gets one, 40 yards and its cooldown.
The server's own values come from the config (TemucraftRevives.cpp), not from these rows."""
import struct, sys

NAME, DESC, AURA = 136, 170, 187          # Spell.dbc enUS string columns
RECOVERY, RANGE = 29, 46                  # RecoveryTime (ms), RangeIndex
RANGE_40 = 5                              # SpellRange.dbc row 5: 40 yards


def texts(args):
    nums = [float(x) for x in args]
    h, m, cd, sh, sm, sc, scd = (nums + [30, 20, 10, 30, 20, 40, 30][len(nums):])[:7]
    fmt = lambda v: ('%d' % v) if v == int(v) else ('%g' % v)
    combat = ('Brings a dead friendly player back to life with %s%% health and %s%% mana. Usable in combat. In a dungeon '
              'or raid it uses one of the group\'s battle resurrection charges.' % (fmt(h), fmt(m)))
    soul = ('Brings a dead friendly player back to life with %s%% health and %s%% mana, for %s%% of your maximum mana. '
            'Cast in combat in a dungeon or raid, it uses one of the group\'s battle resurrection charges.'
            % (fmt(sh), fmt(sm), fmt(sc)))
    return {51918: (combat, int(cd * 60000)), 24341: (soul, int(scd * 1000))}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    want = texts(sys.argv[2:])
    d = bytearray(open(path, 'rb').read())
    assert d[:4] == b'WDBC'
    n, f, rs, ss = struct.unpack_from('<4I', d, 4)
    strings = bytearray(d[20 + n * rs:])
    known = {}

    def add(text):
        raw = text.encode('utf-8') + b'\0'
        at = strings.find(b'\0' + raw)       # reuse an equal string already in the block
        if at >= 0:
            return at + 1
        if text in known:
            return known[text]
        known[text] = len(strings)
        strings.extend(raw)
        return known[text]

    done = []
    for i in range(n):
        o = 20 + i * rs
        sid = struct.unpack_from('<I', d, o)[0]
        if sid in want:
            desc, cooldown = want[sid]
            struct.pack_into('<I', d, o + DESC * 4, add(desc))
            struct.pack_into('<I', d, o + RECOVERY * 4, cooldown)
            struct.pack_into('<I', d, o + RANGE * 4, RANGE_40)
            done.append(sid)
    out = d[:20 + n * rs] + strings
    struct.pack_into('<I', out, 16, len(strings))
    open(path, 'wb').write(out)
    print('set', done)
    missing = set(want) - set(done)
    if missing:
        print('not in this Spell.dbc:', sorted(missing))


if __name__ == '__main__':
    main()
