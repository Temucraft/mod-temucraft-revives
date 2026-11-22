/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

// Revives for everyone:
// - Every class: a slow single-target revive, out of combat (1 min cast, 20% health, 10% mana).
// - First Aid: a revive from skill 150 that grows with the skill (Resuscitate).
// - Healer specs (Discipline/Holy priest, Holy paladin, Restoration shaman and druid): a mass revive, out of combat.
// - Tank and healer specs: a combat revive. Warlocks: a soulstone revive for a big share of their mana, short cooldown.
// - Combat revives in dungeons and raids share the group's battle resurrection charges (like Mythic+): a number at the
//   start, then N more every M minutes, per instance type with per-map overrides. The soulstone revive only uses a
//   charge when the warlock is in combat.
// Spells the 3.3.5 client already has are used (their client tooltips may not match the values set here).

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemucraftModuleText.h"
#include <algorithm>
#include <list>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-revives";
    constexpr uint32 SPELL_REVIVE = 145728;       // Resurrection (generic, out of combat)
    constexpr uint32 SPELL_FIRST_AID = 115178;    // Resuscitate
    constexpr uint32 SPELL_MASS = 25841;          // Prayer of Elune: area resurrection
    constexpr uint32 SPELL_COMBAT = 51918;        // Revive (2 sec, usable in combat)
    constexpr uint32 SPELL_SOULSTONE = 24341;     // Revive (instant; warlocks' soulstone revive)
    constexpr uint32 SKILL_FIRST_AID_ID = 129;

    enum Text : uint32
    {
        T_NO_CHARGES = 1, T_USED, T_STATUS, T_NO_POOL, T_COMBAT
    };
    char const* Say(Player* p, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, p, id, fallback); }

    struct PoolRule { uint32 start = 1; uint32 charges = 1; uint32 minutes = 10; uint32 max = 0; };

    struct Settings
    {
        bool enable = true;
        bool revive = true; uint32 reviveCast = 60; float reviveHealth = 20, reviveMana = 10;
        bool firstAid = true; uint32 firstAidSkill = 150; uint32 firstAidCast = 30;
        std::vector<std::tuple<uint32, float, float>> firstAidSteps;   // skill -> health %, mana %
        bool mass = true; uint32 massCast = 10; float massHealth = 20, massMana = 10; uint32 massRadius = 30;
        bool combat = true; float combatHealth = 30, combatMana = 20; uint32 combatCooldown = 600;
        bool soulstone = true; float soulstoneHealth = 30, soulstoneMana = 20; uint32 soulstoneCooldown = 30; float soulstoneCost = 40;
        float firstAidLowHealth = 10, firstAidLowMana = 5; float combatRange = 40.0f;
        std::unordered_set<uint32> tankSpecs, healerSpecs;   // class * 10 + talent tab page
        bool pool = true;
        PoolRule base, raid, dungeon, other;
    } settings;

    std::unordered_map<uint32, PoolRule> mapRules;   // per map overrides

    struct Pool { uint32 charges = 0; time_t last = 0; bool started = false; };
    std::unordered_map<uint32, Pool> pools;          // instance id -> charges

    // "150:15:8 300:25:12" style lists
    std::vector<std::tuple<uint32, float, float>> ParseSteps(std::string text)
    {
        std::vector<std::tuple<uint32, float, float>> out;
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream in(text);
        std::string item;
        while (in >> item)
        {
            std::replace(item.begin(), item.end(), ':', ' ');
            std::istringstream part(item);
            uint32 skill; float h, m;
            if (part >> skill >> h >> m)
                out.emplace_back(skill, h, m);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::unordered_set<uint32> ParseSpecs(std::string text)
    {
        std::unordered_set<uint32> out;
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream in(text);
        std::string item;
        while (in >> item)
            if (std::size_t colon = item.find(':'); colon != std::string::npos)
                out.insert(uint32(std::strtoul(item.substr(0, colon).c_str(), nullptr, 10)) * 10 + uint32(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 10)));
        return out;
    }

    PoolRule ReadRule(std::string const& name, PoolRule const& fallback)
    {
        PoolRule r;
        r.start = sConfigMgr->GetOption<uint32>("Temucraft.Revives.BattleRes." + name + ".Start", fallback.start);
        r.charges = sConfigMgr->GetOption<uint32>("Temucraft.Revives.BattleRes." + name + ".Charges", fallback.charges);
        r.minutes = sConfigMgr->GetOption<uint32>("Temucraft.Revives.BattleRes." + name + ".Minutes", fallback.minutes);
        r.max = sConfigMgr->GetOption<uint32>("Temucraft.Revives.BattleRes." + name + ".Max", fallback.max);
        return r;
    }

    void LoadSettings()
    {
        auto b = [](char const* k, bool d) { return sConfigMgr->GetOption<bool>(std::string("Temucraft.Revives.") + k, d); };
        auto u = [](char const* k, uint32 d) { return sConfigMgr->GetOption<uint32>(std::string("Temucraft.Revives.") + k, d); };
        auto f = [](char const* k, float d) { return sConfigMgr->GetOption<float>(std::string("Temucraft.Revives.") + k, d); };
        settings.enable = b("Enable", true);
        settings.revive = b("Revive", true);
        settings.reviveCast = u("Revive.CastSeconds", 60);
        settings.reviveHealth = f("Revive.Health", 20); settings.reviveMana = f("Revive.Mana", 10);
        settings.firstAid = b("FirstAid", true);
        settings.firstAidSkill = u("FirstAid.MinSkill", 150);
        settings.firstAidCast = u("FirstAid.CastSeconds", 30);
        settings.firstAidLowHealth = f("FirstAid.Health", 10); settings.firstAidLowMana = f("FirstAid.Mana", 5);
        settings.combatRange = f("Combat.Range", 40.0f);
        settings.firstAidSteps = ParseSteps(sConfigMgr->GetOption<std::string>("Temucraft.Revives.FirstAid.Steps", "150:15:8 225:20:10 300:25:12 375:30:15 450:35:18"));
        settings.mass = b("Mass", true);
        settings.massCast = u("Mass.CastSeconds", 10);
        settings.massHealth = f("Mass.Health", 20); settings.massMana = f("Mass.Mana", 10);
        settings.massRadius = u("Mass.Radius", 30);
        settings.combat = b("Combat", true);
        settings.combatHealth = f("Combat.Health", 30); settings.combatMana = f("Combat.Mana", 20);
        settings.combatCooldown = u("Combat.CooldownSeconds", 600);
        settings.soulstone = b("Soulstone", true);
        settings.soulstoneHealth = f("Soulstone.Health", 30); settings.soulstoneMana = f("Soulstone.Mana", 20);
        settings.soulstoneCooldown = u("Soulstone.CooldownSeconds", 30);
        settings.soulstoneCost = f("Soulstone.ManaCost", 40);
        settings.tankSpecs = ParseSpecs(sConfigMgr->GetOption<std::string>("Temucraft.Revives.TankSpecs", "1:2 2:1 11:1 6:0 6:1 6:2 9:1 7:1 3:2 4:1"));
        settings.healerSpecs = ParseSpecs(sConfigMgr->GetOption<std::string>("Temucraft.Revives.HealerSpecs", "5:0 5:1 2:0 7:2 11:2"));
        settings.pool = b("BattleRes", true);
        PoolRule def;
        settings.base = ReadRule("Base", def);
        settings.raid = ReadRule("Raid", settings.base);
        settings.dungeon = ReadRule("Dungeon", settings.base);
        settings.other = ReadRule("Other", settings.base);
    }

    void LoadMapRules()
    {
        mapRules.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `map`, `start`, `charges`, `minutes`, `max` FROM `mod_temucraft_battle_res`"))
            do
            {
                Field* f = r->Fetch();
                mapRules[f[0].Get<uint32>()] = { f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>(), f[4].Get<uint32>() };
            } while (r->NextRow());
    }

    // spec of the player: class*10 + the talent tree with most points (none without talents)
    int32 Spec(Player* p)
    {
        uint8 points[3] = { 0, 0, 0 };
        p->GetTalentTreePoints(points);
        if (!points[0] && !points[1] && !points[2])
            return -1;
        return int32(p->getClass()) * 10 + p->GetMostPointsTalentTree();
    }
    bool IsHealer(Player* p) { int32 s = Spec(p); return s >= 0 && settings.healerSpecs.count(uint32(s)); }
    bool IsTank(Player* p) { int32 s = Spec(p); return s >= 0 && settings.tankSpecs.count(uint32(s)); }

    bool UsesPool(Map* map) { return map && map->IsDungeon() && !map->IsBattlegroundOrArena(); }

    PoolRule RuleFor(Map* map)
    {
        if (auto itr = mapRules.find(map->GetId()); itr != mapRules.end())
            return itr->second;
        if (map->IsRaid())
            return settings.raid;
        if (map->IsNonRaidDungeon())
            return settings.dungeon;
        return settings.other;
    }

    Pool& PoolOf(Map* map)
    {
        Pool& pool = pools[map->GetInstanceId()];
        PoolRule const rule = RuleFor(map);
        time_t const now = GameTime::GetGameTime().count();
        if (!pool.started)
        {
            pool.started = true;
            pool.charges = rule.start;
            pool.last = now;
        }
        else if (rule.minutes && rule.charges)
        {
            time_t const step = time_t(rule.minutes) * MINUTE;
            uint32 const gained = uint32((now - pool.last) / step);
            if (gained)
            {
                pool.charges += gained * rule.charges;
                pool.last += gained * step;
            }
        }
        if (rule.max && pool.charges > rule.max)
            pool.charges = rule.max;
        return pool;
    }

    uint32 NextChargeSeconds(Map* map)
    {
        PoolRule const rule = RuleFor(map);
        if (!rule.minutes || !rule.charges)
            return 0;
        Pool& pool = PoolOf(map);
        time_t const next = pool.last + time_t(rule.minutes) * MINUTE;
        time_t const now = GameTime::GetGameTime().count();
        return next > now ? uint32(next - now) : 0;
    }

    void Announce(Player* caster, Map* map)
    {
        Pool& pool = PoolOf(map);
        uint32 const next = NextChargeSeconds(map);
        std::string text = Acore::StringFormat(Say(caster, T_USED, "|cff80c0ffBattle resurrection|r used by {}: {} left (next in {} min)."),
            caster->GetName(), pool.charges, (next + MINUTE - 1) / MINUTE);
        if (Group* group = caster->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member->GetMap() == map)
                        ChatHandler(member->GetSession()).SendSysMessage(text);
        }
        else
            ChatHandler(caster->GetSession()).SendSysMessage(text);
    }

    // what each revive gives: health %, mana %
    void Amounts(Player* caster, uint32 spell, float& health, float& mana)
    {
        switch (spell)
        {
            case SPELL_REVIVE:    health = settings.reviveHealth; mana = settings.reviveMana; break;
            case SPELL_MASS:      health = settings.massHealth; mana = settings.massMana; break;
            case SPELL_COMBAT:    health = settings.combatHealth; mana = settings.combatMana; break;
            case SPELL_SOULSTONE: health = settings.soulstoneHealth; mana = settings.soulstoneMana; break;
            case SPELL_FIRST_AID:
            {
                health = settings.firstAidLowHealth; mana = settings.firstAidLowMana;
                uint32 const skill = caster->GetSkillValue(SKILL_FIRST_AID_ID);
                for (auto const& [from, h, m] : settings.firstAidSteps)
                    if (skill >= from)
                    {
                        health = h;
                        mana = m;
                    }
                break;
            }
            default: health = 20; mana = 10; break;
        }
    }

    // which revives a player should know
    void Sync(Player* p)
    {
        if (!p->IsInWorld())
            return;
        auto want = [p](uint32 spell, bool yes)
        {
            // a spell missing from the server's Spell.dbc / spell_dbc can't be learned: trying again every update floods
            // the character database with cleanup deletes (and stalls logins)
            if (yes && !p->HasSpell(spell) && sSpellMgr->GetSpellInfo(spell))
                p->learnSpell(spell);
            else if (!yes && p->HasSpell(spell))
                p->removeSpell(spell, SPEC_MASK_ALL, false);
        };
        bool const on = settings.enable;
        want(SPELL_REVIVE, on && settings.revive);
        want(SPELL_FIRST_AID, on && settings.firstAid && p->GetSkillValue(SKILL_FIRST_AID_ID) >= settings.firstAidSkill);
        want(SPELL_MASS, on && settings.mass && IsHealer(p));
        want(SPELL_COMBAT, on && settings.combat && (IsHealer(p) || IsTank(p)));
        want(SPELL_SOULSTONE, on && settings.soulstone && p->getClass() == CLASS_WARLOCK);
    }

    SpellCastTimesEntry const* CastTime(uint32 seconds)
    {
        // stock rows with a flat cast time first (some rows with the same base shrink per level)
        static std::unordered_map<uint32, uint32> const known = { { 1, 4 }, { 2, 5 }, { 10, 7 }, { 15, 192 }, { 20, 8 }, { 30, 9 }, { 60, 208 } };
        if (auto itr = known.find(seconds); itr != known.end())
            if (SpellCastTimesEntry const* e = sSpellCastTimesStore.LookupEntry(itr->second))
                if (e->CastTime == int32(seconds * IN_MILLISECONDS))
                    return e;
        int32 const ms = int32(seconds * IN_MILLISECONDS);
        for (uint32 i = 0; i < sSpellCastTimesStore.GetNumRows(); ++i)
            if (SpellCastTimesEntry const* e = sSpellCastTimesStore.LookupEntry(i))
                if (e->CastTime == ms)
                    return e;
        return nullptr;
    }

    SpellRadiusEntry const* Radius(float yards)
    {
        SpellRadiusEntry const* best = nullptr;
        for (uint32 i = 0; i < sSpellRadiusStore.GetNumRows(); ++i)
        {
            SpellRadiusEntry const* e = sSpellRadiusStore.LookupEntry(i);
            if (!e)
                continue;
            if (e->RadiusMin == yards && e->RadiusMax == yards)
                return e;
            if (!best && e->RadiusMax >= yards && e->RadiusPerLevel == 0.0f)
                best = e;
        }
        return best;
    }

    void TuneSpell(SpellInfo* spell)
    {
        if (!settings.enable)
            return;
        auto setCast = [spell](uint32 seconds)
        {
            if (SpellCastTimesEntry const* e = CastTime(seconds))
                spell->CastTimeEntry = e;
        };
        switch (spell->Id)
        {
            case SPELL_REVIVE:
                setCast(settings.reviveCast);
                break;
            case SPELL_FIRST_AID:
                setCast(settings.firstAidCast);
                break;
            case SPELL_MASS:
                setCast(settings.massCast);
                spell->Attributes |= SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL;
                if (SpellRadiusEntry const* r = Radius(float(settings.massRadius)))
                    for (SpellEffectInfo& effect : spell->_GetEffects())
                        if (effect.Effect)
                            effect.RadiusEntry = r;
                break;
            case SPELL_COMBAT:
                spell->RecoveryTime = settings.combatCooldown * IN_MILLISECONDS;
                break;
            case SPELL_SOULSTONE:
                spell->RecoveryTime = settings.soulstoneCooldown * IN_MILLISECONDS;
                break;
            default:
                break;
        }
    }
}

// The revive spells: players only, battle resurrection charges, health and mana as configured.
class spell_temucraft_revive : public SpellScript
{
    PrepareSpellScript(spell_temucraft_revive);

    bool pooled = false;

    SpellCastResult Check()
    {
        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!caster || !settings.enable)
            return SPELL_CAST_OK;
        uint32 const id = GetSpellInfo()->Id;
        if (id == SPELL_MASS)
            return caster->IsInCombat() ? SPELL_FAILED_AFFECTING_COMBAT : SPELL_CAST_OK;
        if (Unit* target = GetExplTargetUnit())
        {
            if (!target->IsPlayer())
                return SPELL_FAILED_BAD_TARGETS;
            // the client allows 100 yards for these two; players revive from 40
            if ((id == SPELL_COMBAT || id == SPELL_SOULSTONE) && !caster->IsWithinDistInMap(target, settings.combatRange))
                return SPELL_FAILED_OUT_OF_RANGE;
        }
        if (id == SPELL_SOULSTONE)
        {
            uint32 const cost = uint32(caster->GetMaxPower(POWER_MANA) * settings.soulstoneCost / 100.0f);
            if (caster->GetPower(POWER_MANA) < cost)
                return SPELL_FAILED_NO_POWER;
        }
        bool const combatRevive = id == SPELL_COMBAT || (id == SPELL_SOULSTONE && caster->IsInCombat());
        if (combatRevive && settings.pool && UsesPool(caster->GetMap()) && !PoolOf(caster->GetMap()).charges)
        {
            ChatHandler(caster->GetSession()).SendSysMessage(Say(caster, T_NO_CHARGES, "No battle resurrection charges left."));
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void OnCastDone()
    {
        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!caster || !settings.enable)
            return;
        uint32 const id = GetSpellInfo()->Id;
        if (id == SPELL_SOULSTONE)
            caster->ModifyPower(POWER_MANA, -int32(caster->GetMaxPower(POWER_MANA) * settings.soulstoneCost / 100.0f));
        bool const combatRevive = id == SPELL_COMBAT || (id == SPELL_SOULSTONE && caster->IsInCombat());
        if (combatRevive && settings.pool && UsesPool(caster->GetMap()))
        {
            Pool& pool = PoolOf(caster->GetMap());
            if (pool.charges)
                --pool.charges;
            Announce(caster, caster->GetMap());
        }
    }

    void PlayersOnly(std::list<WorldObject*>& targets)
    {
        targets.remove_if([](WorldObject* o) { Player* p = o->ToPlayer(); return !p || p->IsAlive(); });
    }

    void SetHealth(SpellEffIndex /*effIndex*/)
    {
        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!caster || !settings.enable)
            return;
        float health, mana;
        Amounts(caster, GetSpellInfo()->Id, health, mana);
        SetEffectValue(int32(health));
    }

    void SetMana()
    {
        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Player* target = GetHitPlayer();
        if (!caster || !target || !settings.enable || !target->isResurrectRequestedBy(caster->GetGUID()))
            return;
        float health, mana;
        Amounts(caster, GetSpellInfo()->Id, health, mana);
        target->setResurrectRequestData(caster->GetGUID(), caster->GetMapId(), caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ(),
            target->CountPctFromMaxHealth(int32(health)), uint32(CalculatePct(target->GetMaxPower(POWER_MANA), mana)));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_temucraft_revive::Check);
        AfterCast += SpellCastFn(spell_temucraft_revive::OnCastDone);
        if (m_scriptSpellId == SPELL_MASS)
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_temucraft_revive::PlayersOnly, EFFECT_0, TARGET_UNIT_SRC_AREA_ALLY);
        OnEffectHitTarget += SpellEffectFn(spell_temucraft_revive::SetHealth, EFFECT_0, SPELL_EFFECT_RESURRECT);
        AfterHit += SpellHitFn(spell_temucraft_revive::SetMana);
    }
};

class TemucraftRevivesWorld : public WorldScript
{
public:
    TemucraftRevivesWorld() : WorldScript("TemucraftRevivesWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }
    void OnAfterConfigLoad(bool reload) override { LoadSettings(); if (reload) LoadMapRules(); }
    void OnStartup() override { LoadMapRules(); }
};

class TemucraftRevivesSpells : public GlobalScript
{
public:
    TemucraftRevivesSpells() : GlobalScript("TemucraftRevivesSpells", { GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR }) { }
    void OnLoadSpellCustomAttr(SpellInfo* spell) override { TuneSpell(spell); }
};

class TemucraftRevivesPlayer : public PlayerScript
{
public:
    TemucraftRevivesPlayer() : PlayerScript("TemucraftRevivesPlayer",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_AFTER_SPEC_SLOT_CHANGED, PLAYERHOOK_ON_TALENTS_RESET,
          PLAYERHOOK_ON_PLAYER_LEARN_TALENTS }) { }

    void OnPlayerLogin(Player* p) override { p->m_Events.AddEventAtOffset([p]() { Sync(p); }, 1s); }
    void OnPlayerLogout(Player* p) override { timers.erase(p->GetGUID().GetCounter()); }
    void OnPlayerAfterSpecSlotChanged(Player* p, uint8 /*slot*/) override { Sync(p); }
    void OnPlayerTalentsReset(Player* p, bool /*noCost*/) override { Sync(p); }
    void OnPlayerLearnTalents(Player* p, uint32, uint32, uint32) override { p->m_Events.AddEventAtOffset([p]() { Sync(p); }, 100ms); }

    // First Aid skill-ups and anything else: checked every few seconds
    void OnPlayerUpdate(Player* p, uint32 diff) override
    {
        uint32& t = timers[p->GetGUID().GetCounter()];
        t += diff;
        if (t < 10000)
            return;
        t = 0;
        Sync(p);
    }

private:
    std::unordered_map<uint32, uint32> timers;
};

class TemucraftRevivesCommands : public CommandScript
{
public:
    TemucraftRevivesCommands() : CommandScript("TemucraftRevivesCommands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table = { { "temucraft battleres", HandleBattleRes, 103050, Console::No } };
        return table;
    }

    static bool HandleBattleRes(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        Map* map = p->GetMap();
        if (!settings.pool || !UsesPool(map))
        {
            handler->SendSysMessage(Say(p, T_NO_POOL, "Battle resurrection charges only count in dungeons and raids."));
            return true;
        }
        Pool& pool = PoolOf(map);
        uint32 const next = NextChargeSeconds(map);
        handler->PSendSysMessage(Say(p, T_STATUS, "Battle resurrection charges here: {} (next in {} min)."), pool.charges, (next + MINUTE - 1) / MINUTE);
        return true;
    }
};

void AddTemucraftRevivesScripts()
{
    RegisterSpellScript(spell_temucraft_revive);
    new TemucraftRevivesWorld();
    new TemucraftRevivesSpells();
    new TemucraftRevivesPlayer();
    new TemucraftRevivesCommands();
}
