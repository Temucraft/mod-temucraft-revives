# mod-temucraft-revives

Everyone can bring a friend back, and combat revives in instances share the group's battle resurrection charges
(like Mythic+).

| Who | Spell | When | Gives |
|---|---|---|---|
| Every class | Resurrection | out of combat, 60 sec cast | 20% health, 10% mana |
| First Aid 150+ | Resuscitate | out of combat, 30 sec cast | grows with the skill: 15%/8% at 150 up to 35%/18% at 450 |
| Healer specs | Prayer of Elune (mass revive, 30 yards) | out of combat, 10 sec cast | 20% health, 10% mana |
| Tank and healer specs | Revive (combat revive) | in combat too, 2 sec, 40 yards, 10 min cooldown | 30% health, 20% mana |
| Warlocks | Revive (soulstone revive) | instant, 40 yards, 30 sec cooldown, costs 40% of maximum mana | 30% health, 20% mana |

Healer classes keep their own class revive in every spec. Healer specs: Discipline/Holy priest, Holy paladin,
Restoration shaman and druid. Tank specs: Protection warrior/paladin, Feral druid, every death knight tree, and the tank
reworks (Demonology warlock, Enhancement shaman, Survival hunter, Combat rogue). The spec is the talent tree with the
most points; the spells come and go with talent changes and dual spec. Revives only work on players.

## Battle resurrection charges

In dungeons and raids, every combat revive (and the soulstone revive cast in combat) uses one of the instance's
charges. An instance starts with some and gains more over time: raids 3, then 1 every 10 minutes; dungeons 1, then 1
every 15 minutes; any other instance (expeditions, scenarios) the base rule, 1 then 1 every 10 minutes. Each type has a
start count, a regeneration (N charges every M minutes) and an optional cap, falling back to the base rule. Per-map
rules go in `mod_temucraft_battle_res` (world database). The group sees how many are left when one is used;
`.temucraft battleres` shows them. Outside instances only the spells' cooldowns count.

## Notes

The spells are ones the 3.3.5 client already has (Resurrection 145728, Resuscitate 115178, Prayer of Elune 25841,
Revive 51918 and 24341), so their client tooltips show their own numbers; the values above are what the server uses.
Prayer of Elune and the Revives keep their names.

The client's cast times are matched in the Temucraft release (patch-T's Spell.dbc, 2026-11-24): Resurrection 60 sec,
Resuscitate 30 sec, Prayer of Elune 10 sec. `tools/client_cast_times.py <Spell.dbc> <SpellCastTimes.dbc>` sets them
again (for example after patch-T's Spell.dbc is replaced); pass your seconds if you changed `CastSeconds` in the config.

## Install

Copy the module into `modules/`, re-run CMake and build. The world SQL adds the per-map table, the spell scripts and
the texts.

Part of the Temucraft modules for AzerothCore 3.3.5a.
