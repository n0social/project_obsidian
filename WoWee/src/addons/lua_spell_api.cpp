// lua_spell_api.cpp — Spell info, casting, auras, and targeting Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "addons/lua_api_helpers.hpp"

namespace wowee::addons {

static int lua_IsSpellInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* spellNameOrId = luaL_checkstring(L, 1);
    const char* uid = luaL_optstring(L, 2, "target");

    // Resolve spell ID
    uint32_t spellId = 0;
    if (spellNameOrId[0] >= '0' && spellNameOrId[0] <= '9') {
        spellId = static_cast<uint32_t>(strtoul(spellNameOrId, nullptr, 10));
    } else {
        std::string nameLow(spellNameOrId);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    if (spellId == 0) { return luaReturnNil(L); }

    // Get spell max range from DBC
    auto data = gh->getSpellData(spellId);
    if (data.maxRange <= 0.0f) { return luaReturnNil(L); }

    // Resolve target position
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { return luaReturnNil(L); }

    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    lua_pushnumber(L, dist <= data.maxRange ? 1 : 0);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)

static int lua_UnitAura(lua_State* L, bool wantBuff) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    int index = static_cast<int>(luaL_optnumber(L, 2, 1));
    if (index < 1) { return luaReturnNil(L); }

    std::string uidStr(uid);
    toLowerInPlace(uidStr);

    const std::vector<game::AuraSlot>* auras = nullptr;
    if (uidStr == "player")      auras = &gh->getPlayerAuras();
    else if (uidStr == "target") auras = &gh->getTargetAuras();
    else {
        // Try party/raid/focus via GUID lookup in unitAurasCache
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid != 0) auras = gh->getUnitAuras(guid);
    }
    if (!auras) { return luaReturnNil(L); }

    // Filter to buffs or debuffs and find the Nth one
    int found = 0;
    for (const auto& aura : *auras) {
        if (aura.isEmpty() || aura.spellId == 0) continue;
        bool isDebuff = (aura.flags & 0x80) != 0;
        if (wantBuff ? isDebuff : !isDebuff) continue;
        found++;
        if (found == index) {
            // Return: name, rank, icon, count, debuffType, duration, expirationTime, ...spellId
            std::string name = gh->getSpellName(aura.spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
            lua_pushstring(L, "");           // rank
            std::string iconPath = gh->getSpellIconPath(aura.spellId);
            if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
            else lua_pushnil(L);             // icon texture path
            lua_pushnumber(L, aura.charges); // count
            // debuffType: resolve from Spell.dbc dispel type
            {
                uint8_t dt = gh->getSpellDispelType(aura.spellId);
                switch (dt) {
                    case 1:  lua_pushstring(L, "Magic");  break;
                    case 2:  lua_pushstring(L, "Curse");  break;
                    case 3:  lua_pushstring(L, "Disease"); break;
                    case 4:  lua_pushstring(L, "Poison"); break;
                    default: lua_pushnil(L);              break;
                }
            }
            lua_pushnumber(L, aura.maxDurationMs > 0 ? aura.maxDurationMs / 1000.0 : 0); // duration
            // expirationTime: GetTime() + remaining seconds (so addons can compute countdown)
            if (aura.durationMs > 0) {
                uint64_t auraNowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t remMs = aura.getRemainingMs(auraNowMs);
                lua_pushnumber(L, luaGetTimeNow() + remMs / 1000.0);
            } else {
                lua_pushnumber(L, 0);  // permanent aura
            }
            // caster: return unit ID string if caster is known
            if (aura.casterGuid != 0) {
                if (aura.casterGuid == gh->getPlayerGuid())
                    lua_pushstring(L, "player");
                else if (aura.casterGuid == gh->getTargetGuid())
                    lua_pushstring(L, "target");
                else if (aura.casterGuid == gh->getFocusGuid())
                    lua_pushstring(L, "focus");
                else if (aura.casterGuid == gh->getPetGuid())
                    lua_pushstring(L, "pet");
                else {
                    char cBuf[32];
                    snprintf(cBuf, sizeof(cBuf), "0x%016llX", (unsigned long long)aura.casterGuid);
                    lua_pushstring(L, cBuf);
                }
            } else {
                lua_pushnil(L);
            }
            lua_pushboolean(L, 0);           // isStealable
            lua_pushboolean(L, 0);           // shouldConsolidate
            lua_pushnumber(L, aura.spellId); // spellId
            return 11;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int lua_UnitBuff(lua_State* L) { return lua_UnitAura(L, true); }
static int lua_UnitDebuff(lua_State* L) { return lua_UnitAura(L, false); }

// UnitAura(unit, index, filter) — generic aura query with filter string
// filter: "HELPFUL" = buffs, "HARMFUL" = debuffs, "PLAYER" = cast by player,
//         "HELPFUL|PLAYER" = buffs cast by player, etc.
static int lua_UnitAuraGeneric(lua_State* L) {
    const char* filter = luaL_optstring(L, 3, "HELPFUL");
    std::string f(filter ? filter : "HELPFUL");
    for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    bool wantBuff = (f.find("HARMFUL") == std::string::npos);
    return lua_UnitAura(L, wantBuff);
}

// ---------- UnitCastingInfo / UnitChannelInfo ----------
// Internal helper: pushes cast/channel info for a unit.
// Returns number of Lua return values (0 if not casting/channeling the requested type).
static int lua_UnitCastInfo(lua_State* L, bool wantChannel) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid ? uid : "player");

    // Use shared GetTime() epoch for consistent timestamps
    double nowSec = luaGetTimeNow();

    // Resolve cast state for the unit
    bool isCasting = false;
    bool isChannel = false;
    uint32_t spellId = 0;
    float timeTotal = 0.0f;
    float timeRemaining = 0.0f;
    bool interruptible = true;

    if (uidStr == "player") {
        isCasting = gh->isCasting();
        isChannel = gh->isChanneling();
        spellId = gh->getCurrentCastSpellId();
        timeTotal = gh->getCastTimeTotal();
        timeRemaining = gh->getCastTimeRemaining();
        // Player interruptibility: always true for own casts (server controls actual interrupt)
        interruptible = true;
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnNil(L); }
        const auto* state = gh->getUnitCastState(guid);
        if (!state) { return luaReturnNil(L); }
        isCasting = state->casting;
        isChannel = state->isChannel;
        spellId = state->spellId;
        timeTotal = state->timeTotal;
        timeRemaining = state->timeRemaining;
        interruptible = state->interruptible;
    }

    if (!isCasting) { return luaReturnNil(L); }

    // UnitCastingInfo: only returns for non-channel casts
    // UnitChannelInfo: only returns for channels
    if (wantChannel != isChannel) { return luaReturnNil(L); }

    // Spell name + icon
    const std::string& name = gh->getSpellName(spellId);
    std::string iconPath = gh->getSpellIconPath(spellId);

    // Time values in milliseconds (WoW API convention)
    double startTimeMs = (nowSec - (timeTotal - timeRemaining)) * 1000.0;
    double endTimeMs   = (nowSec + timeRemaining) * 1000.0;

    // Return values match WoW API:
    // UnitCastingInfo: name, text, texture, startTime, endTime, isTradeSkill, castID, notInterruptible
    // UnitChannelInfo: name, text, texture, startTime, endTime, isTradeSkill, notInterruptible
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
    lua_pushstring(L, "");                                       // text (sub-text, usually empty)
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushstring(L, "Interface\\Icons\\INV_Misc_QuestionMark");  // texture
    lua_pushnumber(L, startTimeMs);                              // startTime (ms)
    lua_pushnumber(L, endTimeMs);                                // endTime (ms)
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0); // isTradeSkill
    if (!wantChannel) {
        lua_pushnumber(L, spellId);                              // castID (UnitCastingInfo only)
    }
    lua_pushboolean(L, interruptible ? 0 : 1);                  // notInterruptible
    return wantChannel ? 7 : 8;
}

static int lua_UnitCastingInfo(lua_State* L) { return lua_UnitCastInfo(L, false); }
static int lua_UnitChannelInfo(lua_State* L) { return lua_UnitCastInfo(L, true); }

static int lua_CastSpellByName(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* name = luaL_checkstring(L, 1);
    if (!name || !*name) return 0;

    // Find highest rank of spell by name (same logic as /cast)
    std::string nameLow(name);
    toLowerInPlace(nameLow);

    uint32_t bestId = 0;
    int bestRank = -1;
    for (uint32_t sid : gh->getKnownSpells()) {
        std::string sn = gh->getSpellName(sid);
        toLowerInPlace(sn);
        if (sn != nameLow) continue;
        int rank = 0;
        const std::string& rk = gh->getSpellRank(sid);
        if (!rk.empty()) {
            std::string rkl = rk;
            toLowerInPlace(rkl);
            if (rkl.rfind("rank ", 0) == 0) {
                try { rank = std::stoi(rkl.substr(5)); } catch (...) {}
            }
        }
        if (rank > bestRank) { bestRank = rank; bestId = sid; }
    }
    if (bestId != 0) {
        uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
        gh->castSpell(bestId, target);
    }
    return 0;
}

static int lua_IsSpellKnown(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId));
    return 1;
}

// --- Spell Book Tab API ---

// GetNumSpellTabs() → count
static int lua_GetNumSpellTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    lua_pushnumber(L, gh->getSpellBookTabs().size());
    return 1;
}

// GetSpellTabInfo(tabIndex) → name, texture, offset, numSpells
// tabIndex is 1-based; offset is 1-based global spell book slot
static int lua_GetSpellTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIdx = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || tabIdx < 1) {
        return luaReturnNil(L);
    }
    const auto& tabs = gh->getSpellBookTabs();
    if (tabIdx > static_cast<int>(tabs.size())) {
        return luaReturnNil(L);
    }
    // Compute offset: sum of spells in all preceding tabs (1-based)
    int offset = 0;
    for (int i = 0; i < tabIdx - 1; ++i)
        offset += static_cast<int>(tabs[i].spellIds.size());
    const auto& tab = tabs[tabIdx - 1];
    lua_pushstring(L, tab.name.c_str());           // name
    lua_pushstring(L, tab.texture.c_str());        // texture
    lua_pushnumber(L, offset);                     // offset (0-based for WoW compat)
    lua_pushnumber(L, tab.spellIds.size());        // numSpells
    // The highest-rank pair, which is what FrameXML actually reads: with
    // ShowAllSpellRanks off — the default — SpellBook_GetTabInfo throws away
    // the first offset and count and keeps these. Returning four values left
    // numSpells nil and the page count divided by nothing. This client does
    // not track ranks separately, so the whole tab is the highest rank.
    lua_pushnumber(L, offset);                     // highestRankOffset
    lua_pushnumber(L, tab.spellIds.size());        // highestRankNumSpells
    return 6;
}

// GetSpellBookItemInfo(slot, bookType) → "SPELL", spellId
// slot is 1-based global spell book index
static int lua_GetSpellBookItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) {
        lua_pushstring(L, "SPELL");
        lua_pushnumber(L, 0);
        return 2;
    }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot; // 1-based
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            lua_pushstring(L, "SPELL");
            lua_pushnumber(L, tab.spellIds[idx - 1]);
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushstring(L, "SPELL");
    lua_pushnumber(L, 0);
    return 2;
}

// GetSpellBookItemName(slot, bookType) → name, subName
static int lua_GetSpellBookItemName(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) { return luaReturnNil(L); }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot;
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            uint32_t spellId = tab.spellIds[idx - 1];
            const std::string& name = gh->getSpellName(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, ""); // subName/rank
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushnil(L);
    return 1;
}

// GetSpellDescription(spellId) → description string
// Clean spell description template variables for display
static std::string cleanSpellDescription(const std::string& raw, const int32_t effectBase[3] = nullptr, float durationSec = 0.0f) {
    if (raw.empty() || raw.find('$') == std::string::npos) return raw;
    std::string result;
    result.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '$' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == 's' || next == 'S') {
                // $s1, $s2, $s3 — substitute with effect base points + 1
                i += 1; // skip 's'
                int idx = 0;
                if (i + 1 < raw.size() && raw[i + 1] >= '1' && raw[i + 1] <= '3') {
                    idx = raw[i + 1] - '1';
                    ++i;
                }
                if (effectBase && effectBase[idx] != 0) {
                    int32_t val = std::abs(effectBase[idx]) + 1;
                    result += std::to_string(val);
                } else {
                    result += 'X';
                }
                while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') ++i;
            } else if (next == 'o' || next == 'O') {
                // $o1 = periodic total (base * ticks). Ticks = duration / 3sec for most spells
                i += 1;
                int idx = 0;
                if (i + 1 < raw.size() && raw[i + 1] >= '1' && raw[i + 1] <= '3') {
                    idx = raw[i + 1] - '1';
                    ++i;
                }
                if (effectBase && effectBase[idx] != 0 && durationSec > 0.0f) {
                    int32_t perTick = std::abs(effectBase[idx]) + 1;
                    int ticks = static_cast<int>(durationSec / 3.0f);
                    if (ticks < 1) ticks = 1;
                    result += std::to_string(perTick * ticks);
                } else {
                    result += 'X';
                }
                while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') ++i;
            } else if (next == 'e' || next == 'E' || next == 't' || next == 'T' ||
                next == 'h' || next == 'H' || next == 'u' || next == 'U') {
                // Other variables — insert "X" placeholder
                result += 'X';
                i += 1;
                while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') ++i;
            } else if (next == 'd' || next == 'D') {
                // $d = duration
                if (durationSec > 0.0f) {
                    if (durationSec >= 60.0f)
                        result += std::to_string(static_cast<int>(durationSec / 60.0f)) + " min";
                    else
                        result += std::to_string(static_cast<int>(durationSec)) + " sec";
                } else {
                    result += "X sec";
                }
                ++i;
                while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') ++i;
            } else if (next == 'a' || next == 'A') {
                // $a1 = radius
                result += "X";
                ++i;
                while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') ++i;
            } else if (next == 'b' || next == 'B' || next == 'n' || next == 'N' ||
                       next == 'i' || next == 'I' || next == 'x' || next == 'X') {
                // misc variables
                result += "X";
                ++i;
                while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '9') ++i;
            } else if (next == '$') {
                // $$ = literal $
                result += '$';
                ++i;
            } else if (next == '{' || next == '<') {
                // ${...} or $<...> — skip entire block
                char close = (next == '{') ? '}' : '>';
                size_t end = raw.find(close, i + 2);
                if (end != std::string::npos) i = end;
                else result += raw[i]; // no closing — keep $
            } else {
                result += raw[i]; // unknown $ pattern — keep
            }
        } else {
            result += raw[i];
        }
    }
    return result;
}

static int lua_GetSpellDescription(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, ""); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    const std::string& desc = gh->getSpellDescription(spellId);
    const int32_t* ebp = gh->getSpellEffectBasePoints(spellId);
    float dur = gh->getSpellDuration(spellId);
    std::string cleaned = cleanSpellDescription(desc, ebp, dur);
    lua_pushstring(L, cleaned.c_str());
    return 1;
}


static int lua_GetEnchantInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t enchantId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    std::string name = gh->getEnchantName(enchantId);
    if (name.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, name.c_str());
    return 1;
}

static bool spellIdForBookSlot(game::GameHandler* gh, int slot, uint32_t& spellId);

static int lua_GetSpellCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    // Accept spell name or ID
    uint32_t spellId = 0;
    if (lua_isstring(L, 2)) {
        spellIdForBookSlot(gh, static_cast<int>(luaL_optnumber(L, 1, 0)), spellId);
    } else if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else {
        const char* name = luaL_checkstring(L, 1);
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    float cd = gh->getSpellCooldown(spellId);
    // Also check GCD — if spell has no individual cooldown but GCD is active,
    // return the GCD timing (this is how WoW handles it)
    float gcdRem = gh->getGCDRemaining();
    float gcdTotal = gh->getGCDTotal();

    // WoW returns (start, duration, enabled) where remaining = start + duration - GetTime()
    double nowSec = luaGetTimeNow();

    if (cd > 0.01f) {
        // Spell-specific cooldown (longer than GCD)
        double start = nowSec - 0.01; // approximate start as "just now" minus epsilon
        lua_pushnumber(L, start);
        lua_pushnumber(L, cd);
    } else if (gcdRem > 0.01f) {
        // GCD is active — return GCD timing
        double elapsed = gcdTotal - gcdRem;
        double start = nowSec - elapsed;
        lua_pushnumber(L, start);
        lua_pushnumber(L, gcdTotal);
    } else {
        lua_pushnumber(L, 0);       // not on cooldown
        lua_pushnumber(L, 0);
    }
    lua_pushnumber(L, 1);           // enabled
    return 3;
}

static int lua_HasTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasTarget());
    return 1;
}

// TargetUnit(unitId) — set current target
static int lua_TargetUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) gh->setTarget(guid);
    return 0;
}

// ClearTarget() — clear current target
static int lua_ClearTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->clearTarget();
    return 0;
}

// FocusUnit(unitId) — set focus target
static int lua_FocusUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, nullptr);
    if (!uid || !*uid) return 0;
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) gh->setFocus(guid);
    return 0;
}

// ClearFocus() — clear focus target
static int lua_ClearFocus(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->clearFocus();
    return 0;
}

// AssistUnit(unitId) — target whatever the given unit is targeting
static int lua_AssistUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    uint64_t theirTarget = getEntityTargetGuid(gh, guid);
    if (theirTarget != 0) gh->setTarget(theirTarget);
    return 0;
}

// TargetLastTarget() — re-target previous target
static int lua_TargetLastTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetLastTarget();
    return 0;
}

// TargetNearestEnemy() — tab-target nearest enemy
static int lua_TargetNearestEnemy(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetEnemy(false);
    return 0;
}

// TargetNearestFriend() — target nearest friendly unit
static int lua_TargetNearestFriend(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetFriend(false);
    return 0;
}

// GetRaidTargetIndex(unit) → icon index (1-8) or nil
static int lua_GetRaidTargetIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    uint8_t mark = gh->getEntityRaidMark(guid);
    if (mark == 0xFF) { return luaReturnNil(L); }
    lua_pushnumber(L, mark + 1); // WoW uses 1-indexed (1=Star, 2=Circle, ... 8=Skull)
    return 1;
}

// SetRaidTarget(unit, index) — set raid marker (1-8, or 0 to clear)
static int lua_SetRaidTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    int index = static_cast<int>(luaL_checknumber(L, 2));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    if (index >= 1 && index <= 8)
        gh->setRaidMark(guid, static_cast<uint8_t>(index - 1));
    else if (index == 0)
        gh->setRaidMark(guid, 0xFF); // clear
    return 0;
}

// GetSpellPowerCost(spellId) → {{ type=powerType, cost=manaCost, name=powerName }}

static int lua_GetSpellPowerCost(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_newtable(L); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    auto data = gh->getSpellData(spellId);
    lua_newtable(L); // outer table (array of cost entries)
    if (data.manaCost > 0) {
        lua_newtable(L); // cost entry
        lua_pushnumber(L, data.powerType);
        lua_setfield(L, -2, "type");
        lua_pushnumber(L, data.manaCost);
        lua_setfield(L, -2, "cost");
    
        lua_pushstring(L, data.powerType < 7 ? kLuaPowerNames[data.powerType] : "MANA");
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, 1); // outer[1] = entry
    }
    return 1;
}

// --- GetSpellInfo / GetSpellTexture ---
// GetSpellInfo(spellIdOrName) -> name, rank, icon, castTime, minRange, maxRange, spellId
static int lua_GetSpellInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { return luaReturnNil(L); }
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        int bestRank = -1;
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn != nameLow) continue;
            int rank = 0;
            const std::string& rk = gh->getSpellRank(sid);
            if (!rk.empty()) {
                std::string rkl = rk;
                toLowerInPlace(rkl);
                if (rkl.rfind("rank ", 0) == 0) {
                    try { rank = std::stoi(rkl.substr(5)); } catch (...) {}
                }
            }
            if (rank > bestRank) { bestRank = rank; spellId = sid; }
        }
    }

    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }

    lua_pushstring(L, name.c_str());                        // 1: name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                        // 2: rank
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);                                     // 3: icon texture path
    // Resolve cast time and range from Spell.dbc → SpellCastTimes.dbc / SpellRange.dbc
    auto spellData = gh->getSpellData(spellId);
    lua_pushnumber(L, spellData.castTimeMs);                 // 4: castTime (ms)
    lua_pushnumber(L, spellData.minRange);                   // 5: minRange (yards)
    lua_pushnumber(L, spellData.maxRange);                   // 6: maxRange (yards)
    lua_pushnumber(L, spellId);                              // 7: spellId
    return 7;
}

// 1.12 addresses the spellbook by slot: GetSpellName(id, "spell"), where id
// is the book position from SpellBook_GetSpellID, not the spell's id.
static bool spellIdForBookSlot(game::GameHandler* gh, int slot, uint32_t& spellId) {
    if (!gh || slot < 1) return false;
    int idx = slot;
    for (const auto& tab : gh->getSpellBookTabs()) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            spellId = tab.spellIds[idx - 1];
            return spellId != 0;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    return false;
}

static int lua_GetSpellName(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = 0;
    if (!spellIdForBookSlot(gh, static_cast<int>(luaL_optnumber(L, 1, 0)), spellId))
        return luaReturnNil(L);
    const std::string& name = gh->getSpellName(spellId);
    if (name.empty()) return luaReturnNil(L);
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, gh->getSpellRank(spellId).c_str());
    return 2;
}

static int lua_IsSpellPassive(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = 0;
    if (lua_isstring(L, 2)) {
        if (!spellIdForBookSlot(gh, static_cast<int>(luaL_optnumber(L, 1, 0)), spellId)) {
            lua_pushboolean(L, 0);
            return 1;
        }
    } else if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    }
    constexpr uint32_t kPassive = 0x40u;
    lua_pushboolean(L, gh && spellId != 0 && (gh->getSpellAttributes(spellId) & kPassive) != 0);
    return 1;
}

// GetSpellTexture(spellIdOrName) -> icon texture path string
// GetSpellTexture(bookSlot, "spell") is the 1.12 spellbook form.
static int lua_GetSpellTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t spellId = 0;
    if (lua_isstring(L, 2)) {
        if (!spellIdForBookSlot(gh, static_cast<int>(luaL_optnumber(L, 1, 0)), spellId))
            return luaReturnNil(L);
    } else if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { return luaReturnNil(L); }
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    if (spellId == 0) { return luaReturnNil(L); }
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);
    return 1;
}

// GetItemInfo(itemId) -> name, link, quality, iLevel, reqLevel, class, subclass, maxStack, equipSlot, texture, vendorPrice

static int lua_GetSpellLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { return luaReturnNil(L); }
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }
    char link[256];
    snprintf(link, sizeof(link), "|cff71d5ff|Hspell:%u|h[%s]|h|r", spellId, name.c_str());
    lua_pushstring(L, link);
    return 1;
}

static int lua_CancelUnitBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr != "player") return 0; // Can only cancel own buffs
    int index = static_cast<int>(luaL_checknumber(L, 2));
    const auto& auras = gh->getPlayerAuras();
    // Find the Nth buff (non-debuff)
    int buffCount = 0;
    for (const auto& a : auras) {
        if (a.isEmpty()) continue;
        if ((a.flags & 0x80) != 0) continue; // skip debuffs
        if (++buffCount == index) {
            gh->cancelAura(a.spellId);
            break;
        }
    }
    return 0;
}

// CastSpellByID(spellId) — cast spell by numeric ID
static int lua_CastSpellByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (spellId == 0) return 0;
    uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
    gh->castSpell(spellId, target);
    return 0;
}

static int lua_IsUsableSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }

    if (spellId == 0 || !gh->getKnownSpells().count(spellId)) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }

    float cd = gh->getSpellCooldown(spellId);
    bool onCooldown = (cd > 0.1f);
    bool noMana = false;
    if (!onCooldown) {
        auto spellData = gh->getSpellData(spellId);
        if (spellData.manaCost > 0) {
            auto playerEntity = gh->getEntityManager().getEntity(gh->getPlayerGuid());
            if (playerEntity) {
                auto* unit = dynamic_cast<game::Unit*>(playerEntity.get());
                if (unit && unit->getPower() < spellData.manaCost) {
                    noMana = true;
                }
            }
        }
    }
    lua_pushboolean(L, (onCooldown || noMana) ? 0 : 1);
    lua_pushboolean(L, noMana ? 1 : 0);
    return 2;
}


void registerSpellLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"SpellStopCasting", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->cancelCast();
            return 0;
        }},
                {"SpellStopTargeting", [](lua_State* L) -> int {
            (void)L; return 0; // No targeting reticle in this client
        }},
                {"SpellIsTargeting", [](lua_State* L) -> int {
            lua_pushboolean(L, 0); // No AoE targeting reticle
            return 1;
        }},
                {"IsSpellInRange",    lua_IsSpellInRange},
                {"UnitBuff",          lua_UnitBuff},
                {"UnitDebuff",        lua_UnitDebuff},
                {"UnitAura",          lua_UnitAuraGeneric},
                {"UnitCastingInfo",   lua_UnitCastingInfo},
                {"UnitChannelInfo",   lua_UnitChannelInfo},
                {"CastSpellByName",   lua_CastSpellByName},
                {"CastSpellByID",       lua_CastSpellByID},
                {"IsSpellKnown",      lua_IsSpellKnown},
                {"GetNumSpellTabs",   lua_GetNumSpellTabs},
                {"GetSpellTabInfo",   lua_GetSpellTabInfo},
                {"GetSpellBookItemInfo", lua_GetSpellBookItemInfo},
                {"GetSpellBookItemName", lua_GetSpellBookItemName},
                {"GetSpellCooldown",  lua_GetSpellCooldown},
                {"GetSpellPowerCost", lua_GetSpellPowerCost},
                {"GetSpellDescription", lua_GetSpellDescription},
                {"GetEnchantInfo",     lua_GetEnchantInfo},
                {"GetSpellInfo",      lua_GetSpellInfo},
                {"GetSpellName",      lua_GetSpellName},
                {"IsSpellPassive",    lua_IsSpellPassive},
                {"GetSpellTexture",   lua_GetSpellTexture},
                {"GetSpellLink",         lua_GetSpellLink},
                {"IsUsableSpell",        lua_IsUsableSpell},
                {"CancelUnitBuff",      lua_CancelUnitBuff},
                {"HasTarget",         lua_HasTarget},
                {"TargetUnit",        lua_TargetUnit},
                {"ClearTarget",       lua_ClearTarget},
                {"FocusUnit",         lua_FocusUnit},
                {"ClearFocus",        lua_ClearFocus},
                {"AssistUnit",        lua_AssistUnit},
                {"TargetLastTarget",  lua_TargetLastTarget},
                {"TargetNearestEnemy",  lua_TargetNearestEnemy},
                {"TargetNearestFriend", lua_TargetNearestFriend},
                {"GetRaidTargetIndex",  lua_GetRaidTargetIndex},
                {"SetRaidTarget",       lua_SetRaidTarget},
                {"IsPlayerSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId) ? 1 : 0);
            return 1;
        }},
                {"IsSpellOverlayed", [](lua_State* L) -> int {
            (void)L; lua_pushboolean(L, 0); return 1; // No proc overlay tracking
        }},
                {"IsCurrentSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, gh && gh->getCurrentCastSpellId() == spellId ? 1 : 0);
            return 1;
        }},
                {"IsAutoRepeatSpell", [](lua_State* L) -> int {
            (void)L; lua_pushboolean(L, 0); return 1; // Stub
        }},
                {"CastShapeshiftForm", [](lua_State* L) -> int {
            // CastShapeshiftForm(index) — cast the spell for the given form slot
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) return 0;
            uint8_t classId = gh->getPlayerClass();
            // Map class + index to spell IDs
            // Warrior stances
            static const uint32_t warriorSpells[] = {2457, 71, 2458}; // Battle, Defensive, Berserker
            // Druid forms
            static const uint32_t druidSpells[] = {5487, 783, 768, 40120, 24858, 33891}; // Bear, Travel, Cat, Swift Flight, Moonkin, Tree
            // DK presences
            static const uint32_t dkSpells[] = {48266, 48263, 48265}; // Blood, Frost, Unholy
            // Rogue
            static const uint32_t rogueSpells[] = {1784}; // Stealth

            const uint32_t* spells = nullptr;
            int numSpells = 0;
            switch (classId) {
                case 1: spells = warriorSpells; numSpells = 3; break;
                case 6: spells = dkSpells; numSpells = 3; break;
                case 4: spells = rogueSpells; numSpells = 1; break;
                case 11: spells = druidSpells; numSpells = 6; break;
                default: return 0;
            }
            if (index <= numSpells) {
                gh->castSpell(spells[index - 1], 0);
            }
            return 0;
        }},
                {"CancelShapeshiftForm", [](lua_State* L) -> int {
            // Cancel current form — cast spell 0 or cancel aura
            auto* gh = getGameHandler(L);
            if (gh && gh->getShapeshiftFormId() != 0) {
                // Cancelling a form is done by re-casting the same form spell
                // For simplicity, just note that the server will handle it
            }
            return 0;
        }},
                {"GetShapeshiftFormCooldown", [](lua_State* L) -> int {
            // No per-form cooldown tracking — return no cooldown
            lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
            return 3;
        }},
                {"GetShapeshiftForm", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getShapeshiftFormId() : 0);
            return 1;
        }},
                {"GetNumShapeshiftForms", [](lua_State* L) -> int {
            // Return count based on player class
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnZero(L); }
            uint8_t classId = gh->getPlayerClass();
            // Druid: Bear(1), Aquatic(2), Cat(3), Travel(4), Moonkin/Tree(5/6)
            // Warrior: Battle(1), Defensive(2), Berserker(3)
            // Rogue: Stealth(1)
            // Priest: Shadowform(1)
            // Paladin: varies by level/talents
            // DK: Blood Presence, Frost, Unholy (3)
            switch (classId) {
                case 1: lua_pushnumber(L, 3); break;  // Warrior
                case 2: lua_pushnumber(L, 3); break;  // Paladin (auras)
                case 4: lua_pushnumber(L, 1); break;  // Rogue
                case 5: lua_pushnumber(L, 1); break;  // Priest
                case 6: lua_pushnumber(L, 3); break;  // Death Knight
                case 11: lua_pushnumber(L, 6); break; // Druid
                default: lua_pushnumber(L, 0); break;
            }
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
