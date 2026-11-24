#!/usr/bin/env python3
"""Puts the revive cast times of this module into a client Spell.dbc, so the tooltips show what the server uses.

usage: client_cast_times.py <Spell.dbc> <SpellCastTimes.dbc> [revive_sec firstaid_sec mass_sec]

Both DBCs are the client's (in the Temucraft release: patch-T.MPQ). Spell.dbc is changed in place; pack it back into
the MPQ afterwards. The seconds default to the config defaults (60, 30, 10); pass yours if you changed
Temucraft.Revives.*.CastSeconds. A cast time needs a SpellCastTimes row with that base time and no per-level change;
the script stops if there is none.

Release values (2026-11-24): Resurrection 145728 -> SpellCastTimes 208 (60 sec), Resuscitate 115178 -> 9 (30 sec),
Prayer of Elune 25841 -> 7 (10 sec)."""
import struct, sys

SPELLS = (145728, 115178, 25841)          # Resurrection, Resuscitate, Prayer of Elune
CAST_COL = 28                             # Spell.dbc CastingTimeIndex

def main():
    spell, sct = sys.argv[1], sys.argv[2]
    secs = [int(x) for x in sys.argv[3:6]] if len(sys.argv) >= 6 else [60, 30, 10]
    d = open(sct, 'rb').read(); n, f, rs, ss = struct.unpack_from('<4I', d, 4)
    by_ms = {}
    for i in range(n):
        rid, base, per = struct.unpack_from('<iii', d, 20 + i * rs)
        if per == 0: by_ms.setdefault(base, rid)
    want = {}
    for sid, s in zip(SPELLS, secs):
        if s * 1000 not in by_ms: raise SystemExit(f'no SpellCastTimes row for {s} sec')
        want[sid] = by_ms[s * 1000]
    d = bytearray(open(spell, 'rb').read()); n, f, rs, ss = struct.unpack_from('<4I', d, 4)
    done = []
    for i in range(n):
        o = 20 + i * rs; sid = struct.unpack_from('<I', d, o)[0]
        if sid in want:
            struct.pack_into('<I', d, o + CAST_COL * 4, want[sid]); done.append((sid, want[sid]))
    open(spell, 'wb').write(d)
    print('set', done)
    missing = set(want) - {s for s, _ in done}
    if missing: print('not in this Spell.dbc:', sorted(missing))

if __name__ == '__main__':
    main()
