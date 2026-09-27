// lua_unit_api.cpp — Unit query, stats, party/raid, and player state Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "addons/lua_api_helpers.hpp"
#include "pipeline/asset_manager.hpp"

#include <string>
#include <unordered_map>
#include <utility>

namespace wowee::addons {

static int lua_UnitName(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit && !unit->getName().empty()) {
        lua_pushstring(L, unit->getName().c_str());
    } else {
        // Fallback: party member name for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        if (pm && !pm->name.empty()) {
            lua_pushstring(L, pm->name.c_str());
        } else if (gh && guid != 0) {
            // Try player name cache
            const std::string& cached = gh->lookupName(guid);
            lua_pushstring(L, cached.empty() ? "Unknown" : cached.c_str());
        } else {
            lua_pushstring(L, "Unknown");
        }
    }
    return 1;
}


static int lua_UnitHealth(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getHealth());
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->curHealth : 0);
    }
    return 1;
}

static int lua_UnitHealthMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getMaxHealth());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->maxHealth : 0);
    }
    return 1;
}

static int lua_UnitPower(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getPower());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->curPower : 0);
    }
    return 1;
}

static int lua_UnitPowerMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getMaxPower());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->maxPower : 0);
    }
    return 1;
}

static int lua_UnitLevel(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getLevel());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->level : 0);
    }
    return 1;
}

static int lua_UnitExists(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushboolean(L, 1);
    } else {
        // Party members in other zones don't have entities but still "exist"
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        lua_pushboolean(L, guid != 0 && findPartyMember(gh, guid) != nullptr);
    }
    return 1;
}

static int lua_UnitIsDead(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushboolean(L, unit->getHealth() == 0);
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushboolean(L, pm ? (pm->curHealth == 0 && pm->maxHealth > 0) : 0);
    }
    return 1;
}

static int lua_UnitClass(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, uid);
    if (unit && gh) {

        uint8_t classId = 0;
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        if (uidStr == "player") {
            classId = gh->getPlayerClass();
        } else {
            // Read class from UNIT_FIELD_BYTES_0 (class is byte 1)
            uint64_t guid = resolveUnitGuid(gh, uidStr);
            if (guid != 0) {
                auto entity = gh->getEntityManager().getEntity(guid);
                if (entity) {
                    uint32_t bytes0 = entity->getField(
                        game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                    classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
                }
            }
            // Fallback: check name query class/race cache
            if (classId == 0 && guid != 0) {
                classId = gh->lookupPlayerClass(guid);
            }
        }
        const char* name = (classId > 0 && classId < 12) ? kLuaClasses[classId] : "Unknown";
        lua_pushstring(L, name);
        lua_pushstring(L, name);  // WoW returns localized + English
        lua_pushnumber(L, classId);
        return 3;
    }
    lua_pushstring(L, "Unknown");
    lua_pushstring(L, "Unknown");
    lua_pushnumber(L, 0);
    return 3;
}

// UnitIsGhost(unit) — true if unit is in ghost form
static int lua_UnitIsGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isPlayerGhost());
    } else {
        // Check UNIT_FIELD_FLAGS for UNIT_FLAG_GHOST (0x00000100) — best approximation
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool ghost = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                // Ghost is PLAYER_FLAGS bit 0x10, NOT UNIT_FIELD_FLAGS bit 0x100
                // (which is UNIT_FLAG_IMMUNE_TO_PC — would flag immune NPCs as ghosts).
                uint32_t pf = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
                ghost = (pf & 0x00000010) != 0;
            }
        }
        lua_pushboolean(L, ghost);
    }
    return 1;
}

// UnitIsDeadOrGhost(unit)
static int lua_UnitIsDeadOrGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    auto* gh = getGameHandler(L);
    bool dead = (unit && unit->getHealth() == 0);
    if (!dead && gh) {
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        if (uidStr == "player") dead = gh->isPlayerGhost() || gh->isPlayerDead();
    }
    lua_pushboolean(L, dead);
    return 1;
}

// UnitIsAFK(unit), UnitIsDND(unit)
static int lua_UnitIsAFK(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // AFK is PLAYER_FLAGS bit 0x01, NOT UNIT_FIELD_FLAGS (where 0x01
            // is UNIT_FLAG_SERVER_CONTROLLED — completely unrelated).
            uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            lua_pushboolean(L, (playerFlags & 0x01) != 0);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int lua_UnitIsDND(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // DND is PLAYER_FLAGS bit 0x02, NOT UNIT_FIELD_FLAGS.
            uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            lua_pushboolean(L, (playerFlags & 0x02) != 0);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// UnitPlayerControlled(unit) — true for players and player-controlled pets
static int lua_UnitPlayerControlled(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { return luaReturnFalse(L); }
    // Players are always player-controlled; pets check UNIT_FLAG_PLAYER_CONTROLLED (0x01000000)
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushboolean(L, 1);
    } else {
        uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
        lua_pushboolean(L, (flags & 0x01000000) != 0);
    }
    return 1;
}

// UnitIsTapped(unit) — true if mob is tapped (tagged by any player)
static int lua_UnitIsTapped(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0004) != 0); // UNIT_DYNFLAG_TAPPED_BY_PLAYER
    return 1;
}

// UnitIsTappedByPlayer(unit) — true if tapped by the local player (can loot)
static int lua_UnitIsTappedByPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    uint32_t df = unit->getDynamicFlags();
    // Tapped by player: has TAPPED flag but also LOOTABLE or TAPPED_BY_ALL
    bool tapped = (df & 0x0004) != 0;
    bool lootable = (df & 0x0001) != 0;
    bool sharedTag = (df & 0x0008) != 0;
    lua_pushboolean(L, tapped && (lootable || sharedTag));
    return 1;
}

// UnitIsTappedByAllThreatList(unit) — true if shared-tag mob
static int lua_UnitIsTappedByAllThreatList(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0008) != 0);
    return 1;
}

// UnitThreatSituation(unit, mobUnit) → 0=not tanking, 1=not tanking but threat, 2=insecurely tanking, 3=securely tanking
static int lua_UnitThreatSituation(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t playerUnitGuid = resolveUnitGuid(gh, uidStr);
    if (playerUnitGuid == 0) { return luaReturnZero(L); }
    // If no mob specified, check general combat threat against current target
    uint64_t mobGuid = 0;
    if (mobUid && *mobUid) {
        std::string mStr(mobUid);
        toLowerInPlace(mStr);
        mobGuid = resolveUnitGuid(gh, mStr);
    }
    // Approximate threat: check if the mob is targeting this unit
    if (mobGuid != 0) {
        auto mobEntity = gh->getEntityManager().getEntity(mobGuid);
        if (mobEntity) {
            const auto& fields = mobEntity->getFields();
            auto loIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
            if (loIt != fields.end()) {
                uint64_t mobTarget = loIt->second;
                auto hiIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                if (hiIt != fields.end())
                    mobTarget |= (static_cast<uint64_t>(hiIt->second) << 32);
                if (mobTarget == playerUnitGuid) {
                    lua_pushnumber(L, 3); // securely tanking
                    return 1;
                }
            }
        }
    }
    // Check if player is in combat (basic threat indicator)
    if (playerUnitGuid == gh->getPlayerGuid() && gh->isInCombat()) {
        lua_pushnumber(L, 1); // in combat but not tanking
        return 1;
    }
    lua_pushnumber(L, 0);
    return 1;
}

// UnitDetailedThreatSituation(unit, mobUnit) → isTanking, status, threatPct, rawThreatPct, threatValue
static int lua_UnitDetailedThreatSituation(lua_State* L) {
    // Use UnitThreatSituation logic for the basics
    auto* gh = getGameHandler(L);
    if (!gh) {
        lua_pushboolean(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 5;
    }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t unitGuid = resolveUnitGuid(gh, uidStr);
    bool isTanking = false;
    int status = 0;
    if (unitGuid != 0 && mobUid && *mobUid) {
        std::string mStr(mobUid);
        toLowerInPlace(mStr);
        uint64_t mobGuid = resolveUnitGuid(gh, mStr);
        if (mobGuid != 0) {
            auto mobEnt = gh->getEntityManager().getEntity(mobGuid);
            if (mobEnt) {
                const auto& f = mobEnt->getFields();
                auto lo = f.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
                if (lo != f.end()) {
                    uint64_t mt = lo->second;
                    auto hi = f.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                    if (hi != f.end()) mt |= (static_cast<uint64_t>(hi->second) << 32);
                    if (mt == unitGuid) { isTanking = true; status = 3; }
                }
            }
        }
    }
    lua_pushboolean(L, isTanking);
    lua_pushnumber(L, status);
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // threatPct
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // rawThreatPct
    lua_pushnumber(L, 0); // threatValue (not available without server threat data)
    return 5;
}

// UnitDistanceSquared(unit) → distSq, canCalculate
static int lua_UnitDistanceSquared(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0 || guid == gh->getPlayerGuid()) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    lua_pushnumber(L, dx*dx + dy*dy + dz*dz);
    lua_pushboolean(L, 1);
    return 2;
}

// CheckInteractDistance(unit, distIndex) → boolean
// distIndex: 1=inspect(28yd), 2=trade(11yd), 3=duel(10yd), 4=follow(28yd)
static int lua_CheckInteractDistance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid = luaL_checkstring(L, 1);
    int distIdx = static_cast<int>(luaL_optnumber(L, 2, 4));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { return luaReturnFalse(L); }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    float maxDist = 28.0f; // default: follow/inspect range
    switch (distIdx) {
        case 1: maxDist = 28.0f; break; // inspect
        case 2: maxDist = 11.11f; break; // trade
        case 3: maxDist = 9.9f; break; // duel
        case 4: maxDist = 28.0f; break; // follow
    }
    lua_pushboolean(L, dist <= maxDist);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)
static int lua_UnitIsVisible(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit != nullptr);
    return 1;
}

// UnitGroupRolesAssigned(unit) → "TANK", "HEALER", "DAMAGER", or "NONE"
static int lua_UnitGroupRolesAssigned(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "NONE"); return 1; }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "NONE"); return 1; }
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            // WotLK LFG roles bitmask (from SMSG_GROUP_LIST / SMSG_LFG_ROLE_CHECK_UPDATE).
            // Bit 0x01 = Leader (not a combat role), 0x02 = Tank, 0x04 = Healer, 0x08 = DPS.
            constexpr uint8_t kRoleTank    = 0x02;
            constexpr uint8_t kRoleHealer  = 0x04;
            constexpr uint8_t kRoleDamager = 0x08;
            if (m.roles & kRoleTank)    { lua_pushstring(L, "TANK"); return 1; }
            if (m.roles & kRoleHealer)  { lua_pushstring(L, "HEALER"); return 1; }
            if (m.roles & kRoleDamager) { lua_pushstring(L, "DAMAGER"); return 1; }
            break;
        }
    }
    lua_pushstring(L, "NONE");
    return 1;
}

// UnitCanAttack(unit, otherUnit) → boolean
static int lua_UnitCanAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == 0 || g2 == 0 || g1 == g2) { return luaReturnFalse(L); }
    // Check if unit2 is hostile to unit1
    auto* unit2 = resolveUnit(L, uid2);
    if (unit2 && unit2->isHostile()) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// UnitCanCooperate(unit, otherUnit) → boolean
static int lua_UnitCanCooperate(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    (void)luaL_checkstring(L, 1); // unit1 (unused — cooperation is based on unit2's hostility)
    const char* uid2 = luaL_checkstring(L, 2);
    auto* unit2 = resolveUnit(L, uid2);
    if (!unit2) { return luaReturnFalse(L); }
    lua_pushboolean(L, !unit2->isHostile());
    return 1;
}

// UnitCreatureFamily(unit) → familyName or nil
static int lua_UnitCreatureFamily(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) { return luaReturnNil(L); }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { return luaReturnNil(L); }
    uint32_t family = gh->getCreatureFamily(unit->getEntry());
    if (family == 0) { return luaReturnNil(L); }
    static constexpr const char* kFamilies[] = {
        "", "Wolf", "Cat", "Spider", "Bear", "Boar", "Crocolisk", "Carrion Bird",
        "Crab", "Gorilla", "Raptor", "", "Tallstrider", "", "", "Felhunter",
        "Voidwalker", "Succubus", "", "Doomguard", "Scorpid", "Turtle", "",
        "Imp", "Bat", "Hyena", "Bird of Prey", "Wind Serpent", "", "Dragonhawk",
        "Ravager", "Warp Stalker", "Sporebat", "Nether Ray", "Serpent", "Moth",
        "Chimaera", "Devilsaur", "Ghoul", "Silithid", "Worm", "Rhino", "Wasp",
        "Core Hound", "Spirit Beast"
    };
    lua_pushstring(L, (family < sizeof(kFamilies)/sizeof(kFamilies[0]) && kFamilies[family][0])
        ? kFamilies[family] : "Beast");
    return 1;
}

// UnitOnTaxi(unit) → boolean (true if on a flight path)
static int lua_UnitOnTaxi(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isOnTaxiFlight());
    } else {
        lua_pushboolean(L, 0); // Can't determine for other units
    }
    return 1;
}

// Race portrait for a creature display. Humanoid NPCs carry an extra-display
// row (race and sex); everything else keeps the caller's fallback.
static std::unordered_map<uint32_t, std::string> s_displayPortrait;
static bool s_displayPortraitLoaded = false;

static void loadDisplayPortraits(game::GameHandler* gh) {
    if (s_displayPortraitLoaded || !gh) return;
    auto* am = gh->services().assetManager;
    if (!am || !am->isInitialized()) return;
    s_displayPortraitLoaded = true;

    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> extra;
    if (auto dbc = am->loadDBC("CreatureDisplayInfoExtra.dbc");
        dbc && dbc->isLoaded() && dbc->getFieldCount() > 2) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            extra.emplace(dbc->getUInt32(i, 0),
                          std::make_pair(dbc->getUInt32(i, 1), dbc->getUInt32(i, 2)));
        }
    }
    static const char* kRaces[] = {
        "", "Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome", "Troll"
    };
    if (auto dbc = am->loadDBC("CreatureDisplayInfo.dbc");
        dbc && dbc->isLoaded() && dbc->getFieldCount() > 3) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const auto it = extra.find(dbc->getUInt32(i, 3));
            if (it == extra.end()) continue;
            const uint32_t race = it->second.first;
            const uint32_t sex = it->second.second;
            if (race < 1 || race > 8) continue;
            std::string path = "Interface\\CharacterFrame\\TemporaryPortrait-";
            path += (sex == 1) ? "Female-" : "Male-";
            path += kRaces[race];
            s_displayPortrait.emplace(dbc->getUInt32(i, 0), std::move(path));
        }
    }
}

static int lua_WoweeUnitPortrait(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    std::string uid(luaL_optstring(L, 1, "target"));
    toLowerInPlace(uid);
    if (uid == "npc") uid = "target";
    loadDisplayPortraits(gh);
    const uint64_t guid = resolveUnitGuid(gh, uid);
    if (guid == 0) return luaReturnNil(L);
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return luaReturnNil(L);
    auto* unit = static_cast<game::Unit*>(entity.get());
    uint32_t display = unit->getDisplayId();
    if (display == 0)
        display = unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID));
    auto it = s_displayPortrait.find(display);
    if (it == s_displayPortrait.end()) return luaReturnNil(L);
    lua_pushstring(L, it->second.c_str());
    return 1;
}

// UnitSex(unit) → 1=unknown, 2=male, 3=female
static int lua_UnitSex(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // Gender is byte 2 of UNIT_FIELD_BYTES_0 (0=male, 1=female)
            uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
            uint8_t gender = static_cast<uint8_t>((bytes0 >> 16) & 0xFF);
            lua_pushnumber(L, gender == 0 ? 2 : (gender == 1 ? 3 : 1)); // WoW: 2=male, 3=female
            return 1;
        }
    }
    lua_pushnumber(L, 1); // unknown
    return 1;
}

// --- Player/Game API ---

static int lua_UnitStat(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 4; }
    int statIdx = static_cast<int>(luaL_checknumber(L, 2)) - 1; // WoW API is 1-indexed
    int32_t val = gh->getPlayerStat(statIdx);
    if (val < 0) val = 0;
    // We only have the effective value from the server; report base=effective, no buffs
    lua_pushnumber(L, val); // base (approximate — server only sends effective)
    lua_pushnumber(L, val); // effective
    lua_pushnumber(L, 0);   // positive buff
    lua_pushnumber(L, 0);   // negative buff
    return 4;
}

// GetDodgeChance() → percent
static int lua_GetDodgeChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getDodgePct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetParryChance() → percent
static int lua_GetParryChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getParryPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetBlockChance() → percent
static int lua_GetBlockChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getBlockPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCritChance() → percent (melee crit)
static int lua_GetCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetRangedCritChance() → percent
static int lua_GetRangedCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getRangedCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetSpellCritChance(school) → percent  (1=Holy,2=Fire,3=Nature,4=Frost,5=Shadow,6=Arcane)
static int lua_GetSpellCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    int school = static_cast<int>(luaL_checknumber(L, 1));
    float v = gh ? gh->getSpellCritPct(school) : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCombatRating(ratingIndex) → value
static int lua_GetCombatRating(lua_State* L) {
    auto* gh = getGameHandler(L);
    int cr = static_cast<int>(luaL_checknumber(L, 1));
    int32_t v = gh ? gh->getCombatRating(cr) : 0;
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetSpellBonusDamage(school) → value  (1-6 magic schools)
static int lua_GetSpellBonusDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int32_t sp = gh->getSpellPower();
    lua_pushnumber(L, sp >= 0 ? sp : 0);
    return 1;
}

// GetSpellBonusHealing() → value
static int lua_GetSpellBonusHealing(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int32_t v = gh->getHealingPower();
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetMeleeHaste / GetAttackPowerForStat stubs for addon compat
static int lua_GetAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getMeleeAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);  // base
    lua_pushnumber(L, 0);   // posBuff
    lua_pushnumber(L, 0);   // negBuff
    return 3;
}

static int lua_GetRangedAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getRangedAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}


static int lua_IsInGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGroup());
    return 1;
}

static int lua_IsInRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGroup() && gh->getPartyData().groupType == 1);
    return 1;
}

// PlaySound(soundId) — play a WoW UI sound by ID or name

static int lua_UnitRace(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); lua_pushstring(L, "Unknown"); lua_pushnumber(L, 0); return 3; }
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);

    uint8_t raceId = 0;
    if (uid == "player") {
        raceId = gh->getPlayerRace();
    } else {
        // Read race from UNIT_FIELD_BYTES_0 (race is byte 0)
        uint64_t guid = resolveUnitGuid(gh, uid);
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                uint32_t bytes0 = entity->getField(
                    game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                raceId = static_cast<uint8_t>(bytes0 & 0xFF);
            }
            // Fallback: name query class/race cache
            if (raceId == 0) raceId = gh->lookupPlayerRace(guid);
        }
    }
    const char* name = (raceId > 0 && raceId < 12) ? kLuaRaces[raceId] : "Unknown";
    lua_pushstring(L, name);      // 1: localized race
    lua_pushstring(L, name);      // 2: English race
    lua_pushnumber(L, raceId);    // 3: raceId (WoW returns 3 values)
    return 3;
}

static int lua_UnitPowerType(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);

    if (unit) {
        uint8_t pt = unit->getPowerType();
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    // Fallback: party member stats for out-of-range members
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    const auto* pm = findPartyMember(gh, guid);
    if (pm) {
        uint8_t pt = pm->powerType;
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    lua_pushnumber(L, 0);
    lua_pushstring(L, "MANA");
    return 2;
}

static int lua_GetNumGroupMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getPartyData().memberCount : 0);
    return 1;
}

static int lua_UnitGUID(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)guid);
    lua_pushstring(L, buf);
    return 1;
}

static int lua_UnitIsPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto entity = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    lua_pushboolean(L, entity && entity->getType() == game::ObjectType::PLAYER);
    return 1;
}

static int lua_InCombatLockdown(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInCombat());
    return 1;
}

static int lua_IsMounted(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isMounted());
    return 1;
}

static int lua_IsFlying(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerFlying());
    return 1;
}

static int lua_IsSwimming(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isSwimming());
    return 1;
}

static int lua_IsResting(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerResting());
    return 1;
}

static int lua_IsFalling(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Check FALLING movement flag
    if (!gh) { return luaReturnFalse(L); }
    const auto& mi = gh->getMovementInfo();
    lua_pushboolean(L, (mi.flags & 0x2000) != 0); // MOVEFLAG_FALLING = 0x2000
    return 1;
}

static int lua_IsStealthed(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    // Check for stealth auras (aura flags bit 0x40 = is harmful, stealth is a buff)
    // WoW detects stealth via unit flags: UNIT_FLAG_IMMUNE (0x02) or specific aura IDs
    // Simplified: check player auras for known stealth spell IDs
    bool stealthed = false;
    for (const auto& a : gh->getPlayerAuras()) {
        if (a.isEmpty() || a.spellId == 0) continue;
        // Common stealth IDs: 1784 (Stealth), 5215 (Prowl), 66 (Invisibility)
        if (a.spellId == 1784 || a.spellId == 5215 || a.spellId == 66 ||
            a.spellId == 1785 || a.spellId == 1786 || a.spellId == 1787 ||
            a.spellId == 11305 || a.spellId == 11306) {
            stealthed = true;
            break;
        }
    }
    lua_pushboolean(L, stealthed);
    return 1;
}

static int lua_GetUnitSpeed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (!gh || std::string(uid) != "player") {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getServerRunSpeed());
    return 1;
}

// --- Container/Bag API ---
// WoW bags: container 0 = backpack (16 slots), containers 1-4 = equipped bags


static int lua_UnitXP(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    std::string u(uid);
    toLowerInPlace(u);
    if (u == "player") lua_pushnumber(L, gh->getPlayerXp());
    else lua_pushnumber(L, 0);
    return 1;
}

static int lua_UnitXPMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string u(uid);
    toLowerInPlace(u);
    if (u == "player") {
        uint32_t nlxp = gh->getPlayerNextLevelXp();
        lua_pushnumber(L, nlxp > 0 ? nlxp : 1);
    } else {
        lua_pushnumber(L, 1);
    }
    return 1;
}

// GetXPExhaustion() → rested XP pool remaining (nil if none)
static int lua_GetXPExhaustion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t rested = gh->getPlayerRestedXp();
    if (rested > 0) lua_pushnumber(L, rested);
    else lua_pushnil(L);
    return 1;
}

// GetRestState() → 1 = normal, 2 = rested
/// GetRestState() → stateID, stateName, xpMultiplier.
///
/// One is rested and two is normal, which is the way round WoW numbers them —
/// this answered the opposite. MainMenuBar multiplies by the third return the
/// line after reading it, so returning only the id was arithmetic on nil every
/// time the cursor crossed the experience bar.
static int lua_GetRestState(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool rested = gh && gh->isPlayerResting();
    lua_pushnumber(L, rested ? 1 : 2);
    lua_pushstring(L, rested ? "Rested" : "Normal");
    lua_pushnumber(L, rested ? 1.5 : 1.0);
    return 3;
}

// --- Quest Log API ---

static int lua_UnitAffectingCombat(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInCombat());
    } else {
        // Check UNIT_FLAG_IN_COMBAT (0x00080000) in UNIT_FIELD_FLAGS
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool inCombat = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                uint32_t flags = entity->getField(
                    game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
                inCombat = (flags & 0x00080000) != 0; // UNIT_FLAG_IN_COMBAT
            }
        }
        lua_pushboolean(L, inCombat);
    }
    return 1;
}

static int lua_GetNumRaidMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { return luaReturnZero(L); }
    const auto& pd = gh->getPartyData();
    lua_pushnumber(L, (pd.groupType == 1) ? pd.memberCount : 0);
    return 1;
}

static int lua_GetNumPartyMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { return luaReturnZero(L); }
    const auto& pd = gh->getPartyData();
    // In party (not raid), count excludes self
    int count = (pd.groupType == 0) ? static_cast<int>(pd.memberCount) : 0;
    // memberCount includes self on some servers, subtract 1 if needed
    if (count > 0) count = std::max(0, count - 1);
    lua_pushnumber(L, count);
    return 1;
}

static int lua_UnitInParty(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInGroup());
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnFalse(L); }
        const auto& pd = gh->getPartyData();
        bool found = false;
        for (const auto& m : pd.members) {
            if (m.guid == guid) { found = true; break; }
        }
        lua_pushboolean(L, found);
    }
    return 1;
}

static int lua_UnitInRaid(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const auto& pd = gh->getPartyData();
    if (pd.groupType != 1) { return luaReturnFalse(L); }
    if (uidStr == "player") {
        lua_pushboolean(L, 1);
        return 1;
    }
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    bool found = false;
    for (const auto& m : pd.members) {
        if (m.guid == guid) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

// GetRaidRosterInfo(index) → name, rank, subgroup, level, class, fileName, zone, online, isDead, role, isML
static int lua_GetRaidRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto& pd = gh->getPartyData();
    if (index > static_cast<int>(pd.members.size())) { return luaReturnNil(L); }
    const auto& m = pd.members[index - 1];
    lua_pushstring(L, m.name.c_str());       // name
    lua_pushnumber(L, m.guid == pd.leaderGuid ? 2 : (m.flags & 0x01 ? 1 : 0)); // rank (0=member, 1=assist, 2=leader)
    lua_pushnumber(L, m.subGroup + 1);       // subgroup (1-indexed)
    lua_pushnumber(L, m.level);              // level
    // Class: resolve from entity if available
    std::string className = "Unknown";
    auto entity = gh->getEntityManager().getEntity(m.guid);
    if (entity) {
        uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        uint8_t classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
        if (classId > 0 && classId < 12) className = kLuaClasses[classId];
    }
    lua_pushstring(L, className.c_str());    // class (localized)
    lua_pushstring(L, className.c_str());    // fileName
    lua_pushstring(L, "");                   // zone
    lua_pushboolean(L, m.isOnline);          // online
    lua_pushboolean(L, m.curHealth == 0);    // isDead
    lua_pushstring(L, "NONE");               // role
    lua_pushboolean(L, pd.looterGuid == m.guid ? 1 : 0); // isML
    return 11;
}

// GetThreatStatusColor(statusIndex) → r, g, b
static int lua_GetThreatStatusColor(lua_State* L) {
    int status = static_cast<int>(luaL_optnumber(L, 1, 0));
    switch (status) {
        case 0: lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); break; // gray (no threat)
        case 1: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.47f); break; // yellow (threat)
        case 2: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.6f);  lua_pushnumber(L, 0.0f);  break; // orange (high threat)
        case 3: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.0f);  lua_pushnumber(L, 0.0f);  break; // red (tanking)
        default: lua_pushnumber(L, 1.0f); lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  break;
    }
    return 3;
}

// GetReadyCheckStatus(unit) → status string
static int lua_GetReadyCheckStatus(lua_State* L) {
    (void)L;
    lua_pushnil(L); // No ready check in progress
    return 1;
}

// RegisterUnitWatch / UnregisterUnitWatch — secure unit frame stubs
static int lua_RegisterUnitWatch(lua_State* L) { (void)L; return 0; }
static int lua_UnregisterUnitWatch(lua_State* L) { (void)L; return 0; }

static int lua_UnitIsUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    lua_pushboolean(L, g1 != 0 && g1 == g2);
    return 1;
}

static int lua_UnitIsFriend(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit && !unit->isHostile());
    return 1;
}

static int lua_UnitIsEnemy(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit && unit->isHostile());
    return 1;
}

static int lua_UnitCreatureType(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "Unknown"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { lua_pushstring(L, "Unknown"); return 1; }
    // Player units are always "Humanoid"
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "Humanoid");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "Unknown"); return 1; }
    uint32_t ct = gh->getCreatureType(unit->getEntry());
    static constexpr const char* kTypes[] = {
        "Unknown", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical",
        "Not specified", "Totem", "Non-combat Pet", "Gas Cloud"
    };
    lua_pushstring(L, (ct < 14) ? kTypes[ct] : "Unknown");
    return 1;
}

// GetPlayerInfoByGUID(guid) → localizedClass, englishClass, localizedRace, englishRace, sex, name, realm
static int lua_GetPlayerInfoByGUID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* guidStr = luaL_checkstring(L, 1);
    if (!gh || !guidStr) {
        for (int i = 0; i < 7; i++) lua_pushnil(L);
        return 7;
    }
    // Parse hex GUID string "0x0000000000000001"
    uint64_t guid = 0;
    if (guidStr[0] == '0' && (guidStr[1] == 'x' || guidStr[1] == 'X'))
        guid = strtoull(guidStr + 2, nullptr, 16);
    else
        guid = strtoull(guidStr, nullptr, 16);

    if (guid == 0) { for (int i = 0; i < 7; i++) lua_pushnil(L); return 7; }

    // Look up entity name
    std::string name = gh->lookupName(guid);
    if (name.empty() && guid == gh->getPlayerGuid()) {
        const auto& chars = gh->getCharacters();
        for (const auto& c : chars)
            if (c.guid == guid) { name = c.name; break; }
    }

    // For player GUID, return class/race if it's the local player
    const char* className = "Unknown";
    const char* raceName = "Unknown";
    if (guid == gh->getPlayerGuid()) {
        uint8_t cid = gh->getPlayerClass();
        uint8_t rid = gh->getPlayerRace();
        if (cid < 12) className = kLuaClasses[cid];
        if (rid < 12) raceName = kLuaRaces[rid];
    }

    lua_pushstring(L, className);  // 1: localizedClass
    lua_pushstring(L, className);  // 2: englishClass
    lua_pushstring(L, raceName);   // 3: localizedRace
    lua_pushstring(L, raceName);   // 4: englishRace
    lua_pushnumber(L, 0);          // 5: sex (0=unknown)
    lua_pushstring(L, name.c_str()); // 6: name
    lua_pushstring(L, "");         // 7: realm
    return 7;
}


static int lua_UnitClassification(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "normal"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "normal"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "normal");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "normal"); return 1; }
    int rank = gh->getCreatureRank(unit->getEntry());
    switch (rank) {
        case 1:  lua_pushstring(L, "elite"); break;
        case 2:  lua_pushstring(L, "rareelite"); break;
        case 3:  lua_pushstring(L, "worldboss"); break;
        case 4:  lua_pushstring(L, "rare"); break;
        default: lua_pushstring(L, "normal"); break;
    }
    return 1;
}

// GetComboPoints("player"|"vehicle", "target") → number
static int lua_GetComboPoints(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getComboPoints() : 0);
    return 1;
}

// UnitReaction(unit, otherUnit) → 1-8 (hostile to exalted)
// Simplified: hostile=2, neutral=4, friendly=5
static int lua_UnitReaction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    auto* unit2 = resolveUnit(L, uid2);
    if (!unit2) { return luaReturnNil(L); }
    // If unit2 is the player, always friendly to self
    std::string u1(uid1);
    toLowerInPlace(u1);
    std::string u2(uid2);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == g2) { lua_pushnumber(L, 5); return 1; } // same unit = friendly
    if (unit2->isHostile()) {
        lua_pushnumber(L, 2); // hostile
    } else {
        lua_pushnumber(L, 5); // friendly
    }
    return 1;
}

// UnitIsConnected(unit) → boolean
static int lua_UnitIsConnected(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    // Player is always connected
    if (guid == gh->getPlayerGuid()) { lua_pushboolean(L, 1); return 1; }
    // Check party/raid member online status
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            lua_pushboolean(L, m.isOnline ? 1 : 0);
            return 1;
        }
    }
    // Non-party entities that exist are considered connected
    auto entity = gh->getEntityManager().getEntity(guid);
    lua_pushboolean(L, entity ? 1 : 0);
    return 1;
}

// HasAction(slot) → boolean (1-indexed slot)

void registerUnitLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"UnitName",      lua_UnitName},
                {"UnitPVPName",   lua_UnitName},
                {"UnitFullName",  lua_UnitName},
                {"GetUnitName",   lua_UnitName},
                {"UnitHealth",    lua_UnitHealth},
                {"UnitHealthMax", lua_UnitHealthMax},
                {"UnitPower",     lua_UnitPower},
                {"UnitPowerMax",  lua_UnitPowerMax},
                {"UnitMana",      lua_UnitPower},
                {"UnitManaMax",   lua_UnitPowerMax},
                {"UnitRage",      lua_UnitPower},
                {"UnitEnergy",    lua_UnitPower},
                {"UnitFocus",     lua_UnitPower},
                {"UnitRunicPower", lua_UnitPower},
                {"UnitLevel",     lua_UnitLevel},
                {"UnitExists",    lua_UnitExists},
                {"UnitIsDead",    lua_UnitIsDead},
                {"UnitIsGhost",   lua_UnitIsGhost},
                {"UnitIsDeadOrGhost", lua_UnitIsDeadOrGhost},
                {"UnitIsAFK",     lua_UnitIsAFK},
                {"UnitIsDND",     lua_UnitIsDND},
                {"UnitPlayerControlled", lua_UnitPlayerControlled},
                {"UnitIsTapped",        lua_UnitIsTapped},
                {"UnitIsTappedByPlayer", lua_UnitIsTappedByPlayer},
                {"UnitIsTappedByAllThreatList", lua_UnitIsTappedByAllThreatList},
                {"UnitIsVisible",       lua_UnitIsVisible},
                {"UnitGroupRolesAssigned", lua_UnitGroupRolesAssigned},
                {"UnitCanAttack",       lua_UnitCanAttack},
                {"UnitCanCooperate",    lua_UnitCanCooperate},
                {"UnitCreatureFamily",  lua_UnitCreatureFamily},
                {"UnitOnTaxi",          lua_UnitOnTaxi},
                {"UnitThreatSituation", lua_UnitThreatSituation},
                {"UnitDetailedThreatSituation", lua_UnitDetailedThreatSituation},
                {"UnitSex",       lua_UnitSex},
                {"UnitClass",     lua_UnitClass},
                {"UnitArmor",     [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int32_t armor = gh ? gh->getArmorRating() : 0;
            if (armor < 0) armor = 0;
            lua_pushnumber(L, armor); // base
            lua_pushnumber(L, armor); // effective
            lua_pushnumber(L, armor); // armor (again for compat)
            lua_pushnumber(L, 0);     // posBuff
            lua_pushnumber(L, 0);     // negBuff
            return 5;
        }},
                {"UnitResistance", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int school = static_cast<int>(luaL_optnumber(L, 2, 0));
            int32_t val = 0;
            if (gh) {
                if (school == 0) val = gh->getArmorRating(); // physical = armor
                else if (school >= 1 && school <= 6) val = gh->getResistance(school);
            }
            if (val < 0) val = 0;
            lua_pushnumber(L, val); // base
            lua_pushnumber(L, val); // effective
            lua_pushnumber(L, 0);   // posBuff
            lua_pushnumber(L, 0);   // negBuff
            return 4;
        }},
                {"UnitStat",      lua_UnitStat},
                {"GetDodgeChance",    lua_GetDodgeChance},
                {"GetParryChance",    lua_GetParryChance},
                {"GetBlockChance",    lua_GetBlockChance},
                {"GetCritChance",     lua_GetCritChance},
                {"GetRangedCritChance", lua_GetRangedCritChance},
                {"GetSpellCritChance",  lua_GetSpellCritChance},
                {"GetCombatRating",     lua_GetCombatRating},
                {"GetSpellBonusDamage", lua_GetSpellBonusDamage},
                {"GetSpellBonusHealing", lua_GetSpellBonusHealing},
                {"GetAttackPowerForStat", lua_GetAttackPower},
                {"GetRangedAttackPower",  lua_GetRangedAttackPower},
                {"IsInGroup",     lua_IsInGroup},
                {"IsInRaid",      lua_IsInRaid},
                {"GetShapeshiftFormInfo", [](lua_State* L) -> int {
            // GetShapeshiftFormInfo(index) → icon, name, isActive, isCastable
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) { return luaReturnNil(L); }
            uint8_t classId = gh->getPlayerClass();
            uint8_t currentForm = gh->getShapeshiftFormId();

            // Form tables per class: {formId, spellId, name, icon}
            struct FormInfo { uint8_t formId; const char* name; const char* icon; };
            static const FormInfo warriorForms[] = {
                {17, "Battle Stance", "Interface\\Icons\\Ability_Warrior_OffensiveStance"},
                {18, "Defensive Stance", "Interface\\Icons\\Ability_Warrior_DefensiveStance"},
                {19, "Berserker Stance", "Interface\\Icons\\Ability_Racial_Avatar"},
            };
            static const FormInfo druidForms[] = {
                {1,  "Bear Form", "Interface\\Icons\\Ability_Racial_BearForm"},
                {4,  "Travel Form", "Interface\\Icons\\Ability_Druid_TravelForm"},
                {3,  "Cat Form", "Interface\\Icons\\Ability_Druid_CatForm"},
                {27, "Swift Flight Form", "Interface\\Icons\\Ability_Druid_FlightForm"},
                {31, "Moonkin Form", "Interface\\Icons\\Spell_Nature_ForceOfNature"},
                {36, "Tree of Life", "Interface\\Icons\\Ability_Druid_TreeofLife"},
            };
            static const FormInfo dkForms[] = {
                {32, "Blood Presence", "Interface\\Icons\\Spell_Deathknight_BloodPresence"},
                {33, "Frost Presence", "Interface\\Icons\\Spell_Deathknight_FrostPresence"},
                {34, "Unholy Presence", "Interface\\Icons\\Spell_Deathknight_UnholyPresence"},
            };
            static const FormInfo rogueForms[] = {
                {30, "Stealth", "Interface\\Icons\\Ability_Stealth"},
            };

            const FormInfo* forms = nullptr;
            int numForms = 0;
            switch (classId) {
                case 1: forms = warriorForms; numForms = 3; break;
                case 6: forms = dkForms; numForms = 3; break;
                case 4: forms = rogueForms; numForms = 1; break;
                case 11: forms = druidForms; numForms = 6; break;
                default: lua_pushnil(L); return 1;
            }
            if (index > numForms) { return luaReturnNil(L); }
            const auto& fi = forms[index - 1];
            lua_pushstring(L, fi.icon);                          // icon
            lua_pushstring(L, fi.name);                          // name
            lua_pushboolean(L, currentForm == fi.formId ? 1 : 0); // isActive
            lua_pushboolean(L, 1);                               // isCastable
            return 4;
        }},
                {"UnitIsPVP", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            if (!gh) { return luaReturnFalse(L); }
            uint64_t guid = resolveUnitGuid(gh, std::string(uid));
            if (guid == 0) { return luaReturnFalse(L); }
            auto entity = gh->getEntityManager().getEntity(guid);
            if (!entity) { return luaReturnFalse(L); }
            // UNIT_FLAG_PVP = 0x00001000
            uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
            lua_pushboolean(L, (flags & 0x00001000) ? 1 : 0);
            return 1;
        }},
                {"UnitIsPVPFreeForAll", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            if (!gh) { return luaReturnFalse(L); }
            uint64_t guid = resolveUnitGuid(gh, std::string(uid));
            if (guid == 0) { return luaReturnFalse(L); }
            auto entity = gh->getEntityManager().getEntity(guid);
            if (!entity) { return luaReturnFalse(L); }
            // FFA PvP is PLAYER_FLAGS bit 0x80, NOT UNIT_FIELD_FLAGS bit 0x00080000
            // (which is UNIT_FLAG_PACIFIED — would flag pacified mobs as FFA-PVP).
            uint32_t pf = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            lua_pushboolean(L, (pf & 0x00000080) ? 1 : 0);
            return 1;
        }},
                {"GetBattlefieldStatus", [](lua_State* L) -> int {
            lua_pushstring(L, "none");
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 3;
        }},
                {"GetNumBattlefieldScores", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            lua_pushnumber(L, sb ? sb->players.size() : 0);
            return 1;
        }},
                {"GetBattlefieldScore", [](lua_State* L) -> int {
            // GetBattlefieldScore(index) → name, killingBlows, honorableKills, deaths, honorGained, faction, rank, race, class, classToken, damageDone, healingDone
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (!sb || index < 1 || index > static_cast<int>(sb->players.size())) {
                return luaReturnNil(L);
            }
            const auto& p = sb->players[index - 1];
            lua_pushstring(L, p.name.c_str());     // name
            lua_pushnumber(L, p.killingBlows);      // killingBlows
            lua_pushnumber(L, p.honorableKills);    // honorableKills
            lua_pushnumber(L, p.deaths);            // deaths
            lua_pushnumber(L, p.bonusHonor);        // honorGained
            lua_pushnumber(L, p.team);              // faction (0=Horde,1=Alliance)
            lua_pushnumber(L, 0);                   // rank
            lua_pushstring(L, "");                  // race
            lua_pushstring(L, "");                  // class
            lua_pushstring(L, "WARRIOR");           // classToken
            lua_pushnumber(L, 0);                   // damageDone
            lua_pushnumber(L, 0);                   // healingDone
            return 12;
        }},
                {"GetBattlefieldWinner", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (sb && sb->hasWinner) lua_pushnumber(L, sb->winner);
            else lua_pushnil(L);
            return 1;
        }},
                {"RequestBattlefieldScoreData", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->requestPvpLog();
            return 0;
        }},
                {"AcceptBattlefieldPort", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int accept = lua_toboolean(L, 2);
            if (gh) {
                if (accept) gh->acceptBattlefield();
                else gh->declineBattlefield();
            }
            return 0;
        }},
                {"UnitRace",          lua_UnitRace},
                {"WoweeUnitPortrait", lua_WoweeUnitPortrait},
                {"UnitPowerType",     lua_UnitPowerType},
                {"GetNumGroupMembers", lua_GetNumGroupMembers},
                {"UnitGUID",          lua_UnitGUID},
                {"UnitIsPlayer",      lua_UnitIsPlayer},
                {"InCombatLockdown",  lua_InCombatLockdown},
                {"UnitDistanceSquared", lua_UnitDistanceSquared},
                {"CheckInteractDistance", lua_CheckInteractDistance},
                {"IsMounted",         lua_IsMounted},
                {"IsFlying",          lua_IsFlying},
                {"IsSwimming",        lua_IsSwimming},
                {"IsResting",         lua_IsResting},
                {"IsFalling",         lua_IsFalling},
                {"IsStealthed",       lua_IsStealthed},
                {"GetUnitSpeed",      lua_GetUnitSpeed},
                {"UnitAffectingCombat", lua_UnitAffectingCombat},
                {"GetNumRaidMembers",   lua_GetNumRaidMembers},
                {"GetNumPartyMembers",  lua_GetNumPartyMembers},
                {"UnitInParty",         lua_UnitInParty},
                {"UnitInRaid",          lua_UnitInRaid},
                {"GetRaidRosterInfo",   lua_GetRaidRosterInfo},
                {"GetThreatStatusColor", lua_GetThreatStatusColor},
                {"GetReadyCheckStatus", lua_GetReadyCheckStatus},
                {"RegisterUnitWatch",   lua_RegisterUnitWatch},
                {"UnregisterUnitWatch", lua_UnregisterUnitWatch},
                {"UnitIsUnit",          lua_UnitIsUnit},
                {"UnitIsFriend",        lua_UnitIsFriend},
                {"UnitIsEnemy",         lua_UnitIsEnemy},
                {"UnitCreatureType",    lua_UnitCreatureType},
                {"UnitClassification",  lua_UnitClassification},
                {"UnitReaction",        lua_UnitReaction},
                {"UnitIsConnected",     lua_UnitIsConnected},
                {"GetComboPoints",      lua_GetComboPoints},
                {"GetPlayerInfoByGUID",  lua_GetPlayerInfoByGUID},
                {"UnitXP",                  lua_UnitXP},
                {"UnitXPMax",               lua_UnitXPMax},
                {"GetXPExhaustion",         lua_GetXPExhaustion},
                {"GetRestState",            lua_GetRestState},
                {"HasFocus", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->hasFocus() ? 1 : 0);
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
