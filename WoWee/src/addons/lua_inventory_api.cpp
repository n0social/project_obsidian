// lua_inventory_api.cpp — Items, containers, merchant, loot, equipment, trading, auction, and mail Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "addons/lua_api_helpers.hpp"
#include "core/logger.hpp"

namespace wowee::addons {

/// Money held on the cursor and staked in a trade. Both are genuinely zero
/// here — this client has neither a money cursor nor an open trade at load —
/// but they have to answer with a number rather than not exist. MoneyFrame's
/// very first update reads GetMoney() - GetCursorMoney() - GetPlayerTradeMoney(),
/// and a missing name comes back from the API fallback as a function whose
/// call yields nothing, so the subtraction hits nil and takes the whole file
/// down. Eleven of them, on this one line.
static int lua_GetZeroMoney(lua_State* L) {
    lua_pushnumber(L, 0.0);
    return 1;
}

/// Prices FrameXML reads straight into a money frame at load, before any
/// server has told us anything. Nil is not an option there: TabardFrame does
/// MoneyFrame_Update(frame, GetTabardCreationCost()) in its OnLoad, and the
/// update divides that by the copper-per-gold constants immediately.
static int lua_GetTabardCreationCost(lua_State* L) {
    lua_pushnumber(L, 100000.0);   // ten gold
    return 1;
}

static int lua_GetSendMailPrice(lua_State* L) {
    lua_pushnumber(L, 30.0);
    return 1;
}

/// Uncommon, which is the default a fresh group starts on. Concatenated
/// straight into a global name — "ITEM_QUALITY" .. threshold .. "_DESC" — so
/// it has to be a number rather than nothing.
static int lua_GetLootThreshold(lua_State* L) {
    lua_pushnumber(L, 2.0);
    return 1;
}

static int lua_GetMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? static_cast<double>(gh->getMoneyCopper()) : 0.0);
    return 1;
}

// --- Merchant/Vendor API ---

static int lua_GetMerchantNumItems(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    lua_pushnumber(L, gh->getVendorItems().items.size());
    return 1;
}

// GetMerchantItemInfo(index) → name, texture, price, stackCount, numAvailable, isUsable
static int lua_GetMerchantItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto& items = gh->getVendorItems().items;
    if (index > static_cast<int>(items.size())) { return luaReturnNil(L); }
    const auto& vi = items[index - 1];
    const auto* info = gh->getItemInfo(vi.itemId);
    std::string name = info ? info->name : ("Item #" + std::to_string(vi.itemId));
    lua_pushstring(L, name.c_str());                    // name
    // texture
    std::string iconPath;
    if (info && info->displayInfoId != 0)
        iconPath = gh->getItemIconPath(info->displayInfoId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);
    lua_pushnumber(L, vi.buyPrice);                     // price (copper)
    lua_pushnumber(L, vi.stackCount > 0 ? vi.stackCount : 1); // stackCount
    lua_pushnumber(L, vi.maxCount == -1 ? -1 : vi.maxCount);  // numAvailable (-1=unlimited)
    lua_pushboolean(L, 1);                              // isUsable
    return 6;
}

// GetMerchantItemLink(index) → item link
static int lua_GetMerchantItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto& items = gh->getVendorItems().items;
    if (index > static_cast<int>(items.size())) { return luaReturnNil(L); }
    const auto& vi = items[index - 1];
    const auto* info = gh->getItemInfo(vi.itemId);
    if (!info) { return luaReturnNil(L); }

    const char* ch = (info->quality < 8) ? kQualHexAlpha[info->quality] : "ffffffff";
    char link[256];
    snprintf(link, sizeof(link), "|c%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r", ch, vi.itemId, info->name.c_str());
    lua_pushstring(L, link);
    return 1;
}

static int lua_CanMerchantRepair(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->getVendorItems().canRepair ? 1 : 0);
    return 1;
}

// UnitStat(unit, statIndex) → base, effective, posBuff, negBuff

static int lua_GetItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t itemId = 0;
    if (lua_isnumber(L, 1)) {
        itemId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        // Try to parse "item:12345" link format
        const char* s = lua_tostring(L, 1);
        std::string str(s ? s : "");
        auto pos = str.find("item:");
        if (pos != std::string::npos) {
            try { itemId = static_cast<uint32_t>(std::stoul(str.substr(pos + 5))); } catch (...) {}
        }
    }
    if (itemId == 0) { return luaReturnNil(L); }

    const auto* info = gh->getItemInfo(itemId);
    if (!info) { return luaReturnNil(L); }

    lua_pushstring(L, info->name.c_str());          // 1: name
    // Build item link with quality-colored text
    const char* colorHex = (info->quality < 8) ? kQualHexAlpha[info->quality] : "ffffffff";
    char link[256];
    snprintf(link, sizeof(link), "|c%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             colorHex, itemId, info->name.c_str());
    lua_pushstring(L, link);                         // 2: link
    lua_pushnumber(L, info->quality);                // 3: quality
    lua_pushnumber(L, info->itemLevel);              // 4: iLevel
    lua_pushnumber(L, info->requiredLevel);          // 5: requiredLevel
    // 6: class (type string) — map itemClass to display name
    {
        static constexpr const char* kItemClasses[] = {
            "Consumable", "Bag", "Weapon", "Gem", "Armor", "Reagent", "Projectile",
            "Trade Goods", "Generic", "Recipe", "Money", "Quiver", "Quest", "Key",
            "Permanent", "Miscellaneous", "Glyph"
        };
        if (info->itemClass < 17)
            lua_pushstring(L, kItemClasses[info->itemClass]);
        else
            lua_pushstring(L, "Miscellaneous");
    }
    // 7: subclass — use subclassName from ItemDef if available, else generic
    lua_pushstring(L, info->subclassName.empty() ? "" : info->subclassName.c_str());
    lua_pushnumber(L, info->maxStack > 0 ? info->maxStack : 1); // 8: maxStack
    // 9: equipSlot — WoW inventoryType to INVTYPE string
    {
        static constexpr const char* kInvTypes[] = {
            "", "INVTYPE_HEAD", "INVTYPE_NECK", "INVTYPE_SHOULDER",
            "INVTYPE_BODY", "INVTYPE_CHEST", "INVTYPE_WAIST", "INVTYPE_LEGS",
            "INVTYPE_FEET", "INVTYPE_WRIST", "INVTYPE_HAND", "INVTYPE_FINGER",
            "INVTYPE_TRINKET", "INVTYPE_WEAPON", "INVTYPE_SHIELD",
            "INVTYPE_RANGED", "INVTYPE_CLOAK", "INVTYPE_2HWEAPON",
            "INVTYPE_BAG", "INVTYPE_TABARD", "INVTYPE_ROBE",
            "INVTYPE_WEAPONMAINHAND", "INVTYPE_WEAPONOFFHAND", "INVTYPE_HOLDABLE",
            "INVTYPE_AMMO", "INVTYPE_THROWN", "INVTYPE_RANGEDRIGHT",
            "INVTYPE_QUIVER", "INVTYPE_RELIC"
        };
        uint32_t invType = info->inventoryType;
        lua_pushstring(L, invType < 29 ? kInvTypes[invType] : "");
    }
    // 10: texture (icon path from ItemDisplayInfo.dbc)
    if (info->displayInfoId != 0) {
        std::string iconPath = gh->getItemIconPath(info->displayInfoId);
        if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
        else lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
    lua_pushnumber(L, info->sellPrice);              // 11: vendorPrice
    return 11;
}

// GetItemQualityColor(quality) → r, g, b, hex
// Quality: 0=Poor(gray), 1=Common(white), 2=Uncommon(green), 3=Rare(blue),
//          4=Epic(purple), 5=Legendary(orange), 6=Artifact(gold), 7=Heirloom(gold)
static int lua_GetItemQualityColor(lua_State* L) {
    int q = static_cast<int>(luaL_checknumber(L, 1));
    struct QC { float r, g, b; const char* hex; };
    static const QC colors[] = {
        {0.62f, 0.62f, 0.62f, "ff9d9d9d"}, // 0 Poor
        {1.00f, 1.00f, 1.00f, "ffffffff"}, // 1 Common
        {0.12f, 1.00f, 0.00f, "ff1eff00"}, // 2 Uncommon
        {0.00f, 0.44f, 0.87f, "ff0070dd"}, // 3 Rare
        {0.64f, 0.21f, 0.93f, "ffa335ee"}, // 4 Epic
        {1.00f, 0.50f, 0.00f, "ffff8000"}, // 5 Legendary
        {0.90f, 0.80f, 0.50f, "ffe6cc80"}, // 6 Artifact
        {0.00f, 0.80f, 1.00f, "ff00ccff"}, // 7 Heirloom
    };
    if (q < 0 || q > 7) q = 1;
    lua_pushnumber(L, colors[q].r);
    lua_pushnumber(L, colors[q].g);
    lua_pushnumber(L, colors[q].b);
    lua_pushstring(L, colors[q].hex);
    return 4;
}

// GetItemCount(itemId [, includeBank]) → count
static int lua_GetItemCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    uint32_t itemId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    const auto& inv = gh->getInventory();
    uint32_t count = 0;
    // Backpack
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        const auto& s = inv.getBackpackSlot(i);
        if (!s.empty() && s.item.itemId == itemId)
            count += (s.item.stackCount > 0 ? s.item.stackCount : 1);
    }
    // Bags 1-4
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
        int sz = inv.getBagSize(b);
        for (int i = 0; i < sz; ++i) {
            const auto& s = inv.getBagSlot(b, i);
            if (!s.empty() && s.item.itemId == itemId)
                count += (s.item.stackCount > 0 ? s.item.stackCount : 1);
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

// UseContainerItem(bag, slot) — use/equip an item from a bag
static int lua_UseContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int bag = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;
    if (bag == 0 && slot >= 1 && slot <= inv.getBackpackSize())
        itemSlot = &inv.getBackpackSlot(slot - 1);
    else if (bag >= 1 && bag <= 4) {
        int sz = inv.getBagSize(bag - 1);
        if (slot >= 1 && slot <= sz)
            itemSlot = &inv.getBagSlot(bag - 1, slot - 1);
    }
    if (itemSlot && !itemSlot->empty())
        gh->useItemById(itemSlot->item.itemId);
    return 0;
}

// _GetItemTooltipData(itemId) → table with armor, bind, stats, damage, description
// Returns a Lua table with detailed item info for tooltip building
static int lua_GetItemTooltipData(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t itemId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (!gh || itemId == 0) { return luaReturnNil(L); }
    const auto* info = gh->getItemInfo(itemId);
    if (!info) { return luaReturnNil(L); }

    lua_newtable(L);
    // Unique / Heroic flags
    if (info->maxCount == 1) { lua_pushboolean(L, 1); lua_setfield(L, -2, "isUnique"); }
    if (info->itemFlags & 0x8) { lua_pushboolean(L, 1); lua_setfield(L, -2, "isHeroic"); }
    if (info->itemFlags & 0x1000000) { lua_pushboolean(L, 1); lua_setfield(L, -2, "isUniqueEquipped"); }
    // Bind type
    lua_pushnumber(L, info->bindType);
    lua_setfield(L, -2, "bindType");
    // Armor
    lua_pushnumber(L, info->armor);
    lua_setfield(L, -2, "armor");
    // Damage
    lua_pushnumber(L, info->damageMin);
    lua_setfield(L, -2, "damageMin");
    lua_pushnumber(L, info->damageMax);
    lua_setfield(L, -2, "damageMax");
    lua_pushnumber(L, info->delayMs);
    lua_setfield(L, -2, "speed");
    // Primary stats
    if (info->stamina != 0) { lua_pushnumber(L, info->stamina); lua_setfield(L, -2, "stamina"); }
    if (info->strength != 0) { lua_pushnumber(L, info->strength); lua_setfield(L, -2, "strength"); }
    if (info->agility != 0) { lua_pushnumber(L, info->agility); lua_setfield(L, -2, "agility"); }
    if (info->intellect != 0) { lua_pushnumber(L, info->intellect); lua_setfield(L, -2, "intellect"); }
    if (info->spirit != 0) { lua_pushnumber(L, info->spirit); lua_setfield(L, -2, "spirit"); }
    // Description
    if (!info->description.empty()) {
        lua_pushstring(L, info->description.c_str());
        lua_setfield(L, -2, "description");
    }
    // Required level
    lua_pushnumber(L, info->requiredLevel);
    lua_setfield(L, -2, "requiredLevel");
    // Extra stats (hit, crit, haste, AP, SP, etc.) as array of {type, value} pairs
    if (!info->extraStats.empty()) {
        lua_newtable(L);
        for (size_t i = 0; i < info->extraStats.size(); ++i) {
            lua_newtable(L);
            lua_pushnumber(L, info->extraStats[i].statType);
            lua_setfield(L, -2, "type");
            lua_pushnumber(L, info->extraStats[i].statValue);
            lua_setfield(L, -2, "value");
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        lua_setfield(L, -2, "extraStats");
    }
    // Resistances
    if (info->fireRes != 0) { lua_pushnumber(L, info->fireRes); lua_setfield(L, -2, "fireRes"); }
    if (info->natureRes != 0) { lua_pushnumber(L, info->natureRes); lua_setfield(L, -2, "natureRes"); }
    if (info->frostRes != 0) { lua_pushnumber(L, info->frostRes); lua_setfield(L, -2, "frostRes"); }
    if (info->shadowRes != 0) { lua_pushnumber(L, info->shadowRes); lua_setfield(L, -2, "shadowRes"); }
    if (info->arcaneRes != 0) { lua_pushnumber(L, info->arcaneRes); lua_setfield(L, -2, "arcaneRes"); }
    // Item spell effects (Use: / Equip: / Chance on Hit:)
    {
        lua_newtable(L);
        int spellCount = 0;
        for (int i = 0; i < 5; ++i) {
            if (info->spells[i].spellId == 0) continue;
            ++spellCount;
            lua_newtable(L);
            lua_pushnumber(L, info->spells[i].spellId);
            lua_setfield(L, -2, "spellId");
            lua_pushnumber(L, info->spells[i].spellTrigger);
            lua_setfield(L, -2, "trigger");
            // Get spell name for display
            const std::string& sName = gh->getSpellName(info->spells[i].spellId);
            if (!sName.empty()) { lua_pushstring(L, sName.c_str()); lua_setfield(L, -2, "name"); }
            // Get description
            const std::string& sDesc = gh->getSpellDescription(info->spells[i].spellId);
            if (!sDesc.empty()) { lua_pushstring(L, sDesc.c_str()); lua_setfield(L, -2, "description"); }
            lua_rawseti(L, -2, spellCount);
        }
        if (spellCount > 0) lua_setfield(L, -2, "itemSpells");
        else lua_pop(L, 1);
    }
    // Gem sockets (WotLK/TBC)
    int numSockets = 0;
    for (int i = 0; i < 3; ++i) {
        if (info->socketColor[i] != 0) ++numSockets;
    }
    if (numSockets > 0) {
        lua_newtable(L);
        for (int i = 0; i < 3; ++i) {
            if (info->socketColor[i] != 0) {
                lua_newtable(L);
                lua_pushnumber(L, info->socketColor[i]);
                lua_setfield(L, -2, "color");
                lua_rawseti(L, -2, i + 1);
            }
        }
        lua_setfield(L, -2, "sockets");
    }
    // Item set
    if (info->itemSetId != 0) {
        lua_pushnumber(L, info->itemSetId);
        lua_setfield(L, -2, "itemSetId");
    }
    // Quest-starting item
    if (info->startQuestId != 0) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "startsQuest");
    }
    return 1;
}

// --- Locale/Build/Realm info ---


static int lua_GetContainerNumSlots(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) { return luaReturnZero(L); }
    const auto& inv = gh->getInventory();
    if (container == 0) {
        lua_pushnumber(L, inv.getBackpackSize());
    } else if (container >= 1 && container <= 4) {
        lua_pushnumber(L, inv.getBagSize(container - 1));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetContainerItemCooldown(container, slot) → start, duration, enable
// Bags call this for every filled slot. A missing function returns nothing,
// and CooldownFrame_SetTimer then compares nil and the bag errors out.
static int lua_GetContainerItemCooldown(lua_State* L) {
    (void)L;
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 1);
    return 3;
}

static int lua_GetBagName(lua_State* L) {
    const int id = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (id == 0) { lua_pushstring(L, "Backpack"); return 1; }
    if (id == -2) { lua_pushstring(L, "Keyring"); return 1; }
    auto* gh = getGameHandler(L);
    if (gh && id >= 1 && id <= 4) {
        const auto slotKind = static_cast<game::EquipSlot>(18 + id);
        const auto& slot = gh->getInventory().getEquipSlot(slotKind);
        if (!slot.empty() && !slot.item.name.empty()) {
            lua_pushstring(L, slot.item.name.c_str());
            return 1;
        }
    }
    lua_pushstring(L, "");
    return 1;
}

// GetContainerItemInfo(container, slot) → texture, count, locked, quality, readable, lootable, link
static void pushItemIcon(lua_State* L, game::GameHandler* gh, const game::ItemSlot& slot);

static int lua_GetContainerItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh) { return luaReturnNil(L); }

    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;

    if (container == 0 && slot >= 1 && slot <= inv.getBackpackSize()) {
        itemSlot = &inv.getBackpackSlot(slot - 1);  // WoW uses 1-based
    } else if (container >= 1 && container <= 4) {
        int bagIdx = container - 1;
        int bagSize = inv.getBagSize(bagIdx);
        if (slot >= 1 && slot <= bagSize)
            itemSlot = &inv.getBagSlot(bagIdx, slot - 1);
    }

    if (!itemSlot || itemSlot->empty()) { return luaReturnNil(L); }

    // Get item info for quality/icon
    const auto* info = gh->getItemInfo(itemSlot->item.itemId);

    pushItemIcon(L, gh, *itemSlot);
    lua_pushnumber(L, itemSlot->item.stackCount);  // count
    lua_pushboolean(L, 0);  // locked
    lua_pushnumber(L, info ? info->quality : 0);  // quality
    lua_pushboolean(L, 0);  // readable
    lua_pushboolean(L, 0);  // lootable
    // Build item link with quality color
    std::string name = info ? info->name : ("Item #" + std::to_string(itemSlot->item.itemId));
    uint32_t q = info ? info->quality : 0;

    uint32_t qi = q < 8 ? q : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQualHexNoAlpha[qi], itemSlot->item.itemId, name.c_str());
    lua_pushstring(L, link);  // link
    return 7;
}

// GetContainerItemLink(container, slot) → item link string
static int lua_GetContainerItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh) { return luaReturnNil(L); }

    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;

    if (container == 0 && slot >= 1 && slot <= inv.getBackpackSize()) {
        itemSlot = &inv.getBackpackSlot(slot - 1);
    } else if (container >= 1 && container <= 4) {
        int bagIdx = container - 1;
        int bagSize = inv.getBagSize(bagIdx);
        if (slot >= 1 && slot <= bagSize)
            itemSlot = &inv.getBagSlot(bagIdx, slot - 1);
    }

    if (!itemSlot || itemSlot->empty()) { return luaReturnNil(L); }
    const auto* info = gh->getItemInfo(itemSlot->item.itemId);
    std::string name = info ? info->name : ("Item #" + std::to_string(itemSlot->item.itemId));
    uint32_t q = info ? info->quality : 0;
    char link[256];

    uint32_t qi = q < 8 ? q : 1u;
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQualHexNoAlpha[qi], itemSlot->item.itemId, name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// GetContainerNumFreeSlots(container) → numFreeSlots, bagType
static int lua_GetContainerNumFreeSlots(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }

    const auto& inv = gh->getInventory();
    int freeSlots = 0;
    int totalSlots = 0;

    if (container == 0) {
        totalSlots = inv.getBackpackSize();
        for (int i = 0; i < totalSlots; ++i)
            if (inv.getBackpackSlot(i).empty()) ++freeSlots;
    } else if (container >= 1 && container <= 4) {
        totalSlots = inv.getBagSize(container - 1);
        for (int i = 0; i < totalSlots; ++i)
            if (inv.getBagSlot(container - 1, i).empty()) ++freeSlots;
    }

    lua_pushnumber(L, freeSlots);
    lua_pushnumber(L, 0);  // bagType (0 = normal)
    return 2;
}

// --- Equipment Slot API ---
// WoW inventory slot IDs: 1=Head,2=Neck,3=Shoulders,4=Shirt,5=Chest,
// 6=Waist,7=Legs,8=Feet,9=Wrists,10=Hands,11=Ring1,12=Ring2,
// 13=Trinket1,14=Trinket2,15=Back,16=MainHand,17=OffHand,18=Ranged,19=Tabard

// GetInventorySlotInfo("slotName") → slotId, textureName, checkRelic
// Maps WoW slot names (e.g. "HeadSlot", "HEADSLOT") to inventory slot IDs
static int lua_GetInventorySlotInfo(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::string slot(name);
    // Normalize: uppercase, strip trailing "SLOT" if present
    for (char& c : slot) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (slot.size() > 4 && slot.substr(slot.size() - 4) == "SLOT")
        slot = slot.substr(0, slot.size() - 4);

    // WoW inventory slots are 1-indexed
    struct SlotMap { const char* name; int id; const char* texture; };
    static const SlotMap mapping[] = {
        {"HEAD",          1,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Head"},
        {"NECK",          2,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Neck"},
        {"SHOULDER",      3,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Shoulder"},
        {"SHIRT",         4,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Shirt"},
        {"CHEST",         5,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Chest"},
        {"WAIST",         6,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Waist"},
        {"LEGS",          7,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Legs"},
        {"FEET",          8,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Feet"},
        {"WRIST",         9,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Wrists"},
        {"HANDS",        10,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Hands"},
        {"FINGER0",      11,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Finger"},
        {"FINGER1",      12,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Finger"},
        {"TRINKET0",     13,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Trinket"},
        {"TRINKET1",     14,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Trinket"},
        {"BACK",         15,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Chest"},
        {"MAINHAND",     16,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-MainHand"},
        {"SECONDARYHAND",17,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-SecondaryHand"},
        {"RANGED",       18,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Ranged"},
        {"TABARD",       19,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Tabard"},
        // The bag buttons along the main bar ask for these by name at load, and
        // paperdollframe.lua does it in an OnLoad — so a gap here does not just
        // lose the bags, it loses the file.
        {"BAG0",         20,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Bag"},
        {"BAG1",         21,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Bag"},
        {"BAG2",         22,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Bag"},
        {"BAG3",         23,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Bag"},
        {"AMMO",          0,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Ammo"},
    };
    for (const auto& m : mapping) {
        if (slot == m.name) {
            lua_pushnumber(L, m.id);
            lua_pushstring(L, m.texture);
            lua_pushboolean(L, m.id == 18 ? 1 : 0); // checkRelic: only ranged slot
            return 3;
        }
    }
    // nil rather than an error, which is what the real client returns. Raising
    // here takes down the whole file that asked, and a name we do not know is
    // a gap in the table above rather than a reason to lose an interface.
    LOG_WARNING("GetInventorySlotInfo: unknown slot ", name);
    lua_pushnil(L);
    return 1;
}

static int lua_GetInventoryItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    int slotId = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || slotId < 1 || slotId > 19) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr != "player") { return luaReturnNil(L); }

    const auto& inv = gh->getInventory();
    const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    if (slot.empty()) { return luaReturnNil(L); }

    const auto* info = gh->getItemInfo(slot.item.itemId);
    std::string name = info ? info->name : slot.item.name;
    uint32_t q = info ? info->quality : static_cast<uint32_t>(slot.item.quality);

    uint32_t qi = q < 8 ? q : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQualHexNoAlpha[qi], slot.item.itemId, name.c_str());
    lua_pushstring(L, link);
    return 1;
}

static int lua_GetInventoryItemID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    int slotId = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || slotId < 1 || slotId > 19) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr != "player") { return luaReturnNil(L); }

    const auto& inv = gh->getInventory();
    const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    if (slot.empty()) { return luaReturnNil(L); }
    lua_pushnumber(L, slot.item.itemId);
    return 1;
}

static void pushItemIcon(lua_State* L, game::GameHandler* gh, const game::ItemSlot& slot) {
    uint32_t displayId = slot.item.displayInfoId;
    if (displayId == 0) {
        if (const auto* info = gh->getItemInfo(slot.item.itemId))
            displayId = info->displayInfoId;
    }
    const std::string icon = displayId ? gh->getItemIconPath(displayId) : std::string{};
    if (!icon.empty()) lua_pushstring(L, icon.c_str());
    else lua_pushnil(L);
}

static int lua_GetInventoryItemTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    int slotId = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || slotId < 1 || slotId > 19) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr != "player") { return luaReturnNil(L); }

    const auto& inv = gh->getInventory();
    const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    if (slot.empty()) { return luaReturnNil(L); }
    pushItemIcon(L, gh, slot);
    return 1;
}

static int lua_GetInventoryItemCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slotId = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || slotId < 1 || slotId > 19) { lua_pushnumber(L, 0); return 1; }
    const auto& slot = gh->getInventory().getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    lua_pushnumber(L, slot.empty() ? 0 : std::max(1u, slot.item.stackCount));
    return 1;
}

static int lua_GetInventoryItemBroken(lua_State* L) {
    (void)L;
    lua_pushnil(L);
    return 1;
}

static int lua_GetNumLootItems(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isLootWindowOpen()) { return luaReturnZero(L); }
    lua_pushnumber(L, gh->getCurrentLoot().items.size());
    return 1;
}

// GetLootSlotInfo(slot) → texture, name, quantity, quality, locked
static int lua_GetLootSlotInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1)); // 1-indexed
    if (!gh || !gh->isLootWindowOpen()) {
        return luaReturnNil(L);
    }
    const auto& loot = gh->getCurrentLoot();
    if (slot < 1 || slot > static_cast<int>(loot.items.size())) {
        return luaReturnNil(L);
    }
    const auto& item = loot.items[slot - 1];
    const auto* info = gh->getItemInfo(item.itemId);

    // texture (icon path from ItemDisplayInfo.dbc)
    std::string icon;
    if (info && info->displayInfoId != 0) {
        icon = gh->getItemIconPath(info->displayInfoId);
    }
    if (!icon.empty()) lua_pushstring(L, icon.c_str());
    else lua_pushnil(L);

    // name
    if (info && !info->name.empty()) lua_pushstring(L, info->name.c_str());
    else lua_pushstring(L, ("Item #" + std::to_string(item.itemId)).c_str());

    lua_pushnumber(L, item.count);                           // quantity
    lua_pushnumber(L, info ? info->quality : 1);             // quality
    lua_pushboolean(L, 0);                                   // locked (not tracked)
    return 5;
}

// GetLootSlotLink(slot) → itemLink
static int lua_GetLootSlotLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || !gh->isLootWindowOpen()) { return luaReturnNil(L); }
    const auto& loot = gh->getCurrentLoot();
    if (slot < 1 || slot > static_cast<int>(loot.items.size())) {
        return luaReturnNil(L);
    }
    const auto& item = loot.items[slot - 1];
    const auto* info = gh->getItemInfo(item.itemId);
    if (!info || info->name.empty()) { return luaReturnNil(L); }

    uint32_t qi = info->quality < 8 ? info->quality : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQualHexNoAlpha[qi], item.itemId, info->name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// LootSlot(slot) — take item from loot
static int lua_LootSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || !gh->isLootWindowOpen()) return 0;
    const auto& loot = gh->getCurrentLoot();
    if (slot < 1 || slot > static_cast<int>(loot.items.size())) return 0;
    gh->lootItem(loot.items[slot - 1].slotIndex);
    return 0;
}

// CloseLoot() — close loot window
static int lua_CloseLoot(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->closeLoot();
    return 0;
}

// GetLootMethod() → "freeforall"|"roundrobin"|"master"|"group"|"needbeforegreed", partyLoot, raidLoot
static int lua_GetLootMethod(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "freeforall"); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    const auto& pd = gh->getPartyData();
    const char* method = "freeforall";
    switch (pd.lootMethod) {
        case 0: method = "freeforall"; break;
        case 1: method = "roundrobin"; break;
        case 2: method = "master"; break;
        case 3: method = "group"; break;
        case 4: method = "needbeforegreed"; break;
    }
    lua_pushstring(L, method);
    lua_pushnumber(L, 0); // partyLootMaster (index)
    lua_pushnumber(L, 0); // raidLootMaster (index)
    return 3;
}

// --- Additional WoW API ---

static int lua_GetItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t itemId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (itemId == 0) { return luaReturnNil(L); }
    const auto* info = gh->getItemInfo(itemId);
    if (!info || info->name.empty()) { return luaReturnNil(L); }

    uint32_t qi = info->quality < 8 ? info->quality : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQualHexNoAlpha[qi], itemId, info->name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// GetSpellLink(spellIdOrName) → "|cFFxxxxxx|Hspell:ID|h[Name]|h|r"

void registerInventoryLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"GetMoney",      lua_GetMoney},
                {"GetCursorMoney",      lua_GetZeroMoney},
                {"GetPlayerTradeMoney", lua_GetZeroMoney},
                {"GetTargetTradeMoney", lua_GetZeroMoney},
                {"GetMerchantNumItems",  lua_GetMerchantNumItems},
                {"GetMerchantItemInfo",  lua_GetMerchantItemInfo},
                {"GetMerchantItemLink",  lua_GetMerchantItemLink},
                {"CanMerchantRepair",    lua_CanMerchantRepair},
                {"GetItemInfo",       lua_GetItemInfo},
                {"GetItemQualityColor", lua_GetItemQualityColor},
                {"_GetItemTooltipData", lua_GetItemTooltipData},
                {"GetItemCount",      lua_GetItemCount},
                {"UseContainerItem",  lua_UseContainerItem},
                {"GetContainerNumSlots",    lua_GetContainerNumSlots},
                {"GetContainerItemCooldown", lua_GetContainerItemCooldown},
                {"GetBagName",              lua_GetBagName},
                {"GetContainerItemInfo",    lua_GetContainerItemInfo},
                {"GetContainerItemLink",    lua_GetContainerItemLink},
                {"GetContainerNumFreeSlots", lua_GetContainerNumFreeSlots},
                {"GetInventorySlotInfo",    lua_GetInventorySlotInfo},
                {"GetInventoryItemLink",    lua_GetInventoryItemLink},
                {"GetInventoryItemID",      lua_GetInventoryItemID},
                {"GetInventoryItemTexture", lua_GetInventoryItemTexture},
                {"GetInventoryItemCount",   lua_GetInventoryItemCount},
                {"GetInventoryItemBroken",  lua_GetInventoryItemBroken},
                {"GetItemLink",          lua_GetItemLink},
                {"GetNumLootItems",     lua_GetNumLootItems},
                {"GetLootSlotInfo",     lua_GetLootSlotInfo},
                {"GetLootSlotLink",     lua_GetLootSlotLink},
                {"LootSlot",            lua_LootSlot},
                {"CloseLoot",           lua_CloseLoot},
                {"GetLootMethod",       lua_GetLootMethod},
                {"GetLootThreshold",    lua_GetLootThreshold},
                {"GetTabardCreationCost", lua_GetTabardCreationCost},
                {"GetSendMailPrice",    lua_GetSendMailPrice},
                {"BuyMerchantItem", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            int count = static_cast<int>(luaL_optnumber(L, 2, 1));
            if (!gh || index < 1) return 0;
            const auto& items = gh->getVendorItems().items;
            if (index > static_cast<int>(items.size())) return 0;
            const auto& vi = items[index - 1];
            gh->buyItem(gh->getVendorGuid(), vi.itemId, vi.slot, count);
            return 0;
        }},
                {"SellContainerItem", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int bag = static_cast<int>(luaL_checknumber(L, 1));
            int slot = static_cast<int>(luaL_checknumber(L, 2));
            if (!gh) return 0;
            if (bag == 0) gh->sellItemBySlot(slot - 1);
            else if (bag >= 1 && bag <= 4) gh->sellItemInBag(bag - 1, slot - 1);
            return 0;
        }},
                {"RepairAllItems", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->getVendorItems().canRepair) {
                bool useGuildBank = lua_toboolean(L, 1) != 0;
                gh->repairAll(gh->getVendorGuid(), useGuildBank);
            }
            return 0;
        }},
                {"UnequipItemSlot", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int slot = static_cast<int>(luaL_checknumber(L, 1));
            if (gh && slot >= 1 && slot <= 19)
                gh->unequipToBackpack(static_cast<game::EquipSlot>(slot - 1));
            return 0;
        }},
                {"AcceptTrade", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->acceptTrade();
            return 0;
        }},
                {"CancelTrade", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->isTradeOpen()) gh->cancelTrade();
            return 0;
        }},
                {"InitiateTrade", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_checkstring(L, 1);
            if (gh) {
                uint64_t guid = resolveUnitGuid(gh, std::string(uid));
                if (guid != 0) gh->initiateTrade(guid);
            }
            return 0;
        }},
                {"GetNumAuctionItems", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* listType = luaL_optstring(L, 1, "list");
            if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
            std::string t(listType);
            const game::AuctionListResult* r = nullptr;
            if (t == "list" || t == "browse") r = &gh->getAuctionBrowseResults();
            else if (t == "owner") r = &gh->getAuctionOwnerResults();
            else if (t == "bidder") r = &gh->getAuctionBidderResults();
            lua_pushnumber(L, r ? r->auctions.size() : 0);
            lua_pushnumber(L, r ? r->totalCount : 0);
            return 2;
        }},
                {"GetAuctionItemInfo", [](lua_State* L) -> int {
            // GetAuctionItemInfo(type, index) → name, texture, count, quality,
            // canUse, level, minBid, minIncrement, buyoutPrice, bidAmount,
            // highBidder, owner, saleStatus
            //
            // 3.3.5's thirteen, in 3.3.5's order. This answered Cataclysm's
            // seventeen — levelColHeader after level, and bidderFullName and
            // ownerFullName around owner — while a 3.3.5 auction interface asks
            // for:
            //
            //   name, texture, count, quality, canUse, level, minBid,
            //   minIncrement, buyoutPrice, bidAmount, highBidder, owner
            //
            // so every value from the seventh on landed one place early. minBid
            // received the empty levelColHeader, minIncrement received minBid,
            // buyoutPrice received minIncrement — so no row showed a buyout —
            // bidAmount received the buyout, and owner received a boolean.
            // Bidding and buying read the prices they were given, so both were
            // computed from the wrong number and refused.
            auto* gh = getGameHandler(L);
            const char* listType = luaL_checkstring(L, 1);
            int index = static_cast<int>(luaL_checknumber(L, 2));
            if (!gh || index < 1) { return luaReturnNil(L); }
            std::string t(listType);
            const game::AuctionListResult* r = nullptr;
            if (t == "list") r = &gh->getAuctionBrowseResults();
            else if (t == "owner") r = &gh->getAuctionOwnerResults();
            else if (t == "bidder") r = &gh->getAuctionBidderResults();
            if (!r || index > static_cast<int>(r->auctions.size())) { return luaReturnNil(L); }
            const auto& a = r->auctions[index - 1];
            const auto* info = gh->getItemInfo(a.itemEntry);
            std::string name = info ? info->name : "Item #" + std::to_string(a.itemEntry);
            std::string icon = (info && info->displayInfoId != 0) ? gh->getItemIconPath(info->displayInfoId) : "";
            uint32_t quality = info ? info->quality : 1;
            lua_pushstring(L, name.c_str());        // name
            lua_pushstring(L, icon.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark" : icon.c_str()); // texture
            lua_pushnumber(L, a.stackCount);        // count
            lua_pushnumber(L, quality);             // quality
            lua_pushboolean(L, 1);                  // canUse
            lua_pushnumber(L, info ? info->requiredLevel : 0); // level
            lua_pushnumber(L, a.startBid);          // minBid
            lua_pushnumber(L, a.minBidIncrement);   // minIncrement
            lua_pushnumber(L, a.buyoutPrice);       // buyoutPrice
            lua_pushnumber(L, a.currentBid);        // bidAmount
            lua_pushboolean(L, a.bidderGuid != 0 ? 1 : 0); // highBidder
            lua_pushstring(L, "");                  // owner
            lua_pushnumber(L, 0);                   // saleStatus
            return 13;
        }},
                {"GetAuctionItemTimeLeft", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* listType = luaL_checkstring(L, 1);
            int index = static_cast<int>(luaL_checknumber(L, 2));
            if (!gh || index < 1) { lua_pushnumber(L, 4); return 1; }
            std::string t(listType);
            const game::AuctionListResult* r = nullptr;
            if (t == "list") r = &gh->getAuctionBrowseResults();
            else if (t == "owner") r = &gh->getAuctionOwnerResults();
            else if (t == "bidder") r = &gh->getAuctionBidderResults();
            if (!r || index > static_cast<int>(r->auctions.size())) { lua_pushnumber(L, 4); return 1; }
            // Return 1=short(<30m), 2=medium(<2h), 3=long(<12h), 4=very long(>12h)
            uint32_t ms = r->auctions[index - 1].timeLeftMs;
            int cat = (ms < 1800000) ? 1 : (ms < 7200000) ? 2 : (ms < 43200000) ? 3 : 4;
            lua_pushnumber(L, cat);
            return 1;
        }},
                {"GetAuctionItemLink", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* listType = luaL_checkstring(L, 1);
            int index = static_cast<int>(luaL_checknumber(L, 2));
            if (!gh || index < 1) { return luaReturnNil(L); }
            std::string t(listType);
            const game::AuctionListResult* r = nullptr;
            if (t == "list") r = &gh->getAuctionBrowseResults();
            else if (t == "owner") r = &gh->getAuctionOwnerResults();
            else if (t == "bidder") r = &gh->getAuctionBidderResults();
            if (!r || index > static_cast<int>(r->auctions.size())) { return luaReturnNil(L); }
            uint32_t itemId = r->auctions[index - 1].itemEntry;
            const auto* info = gh->getItemInfo(itemId);
            if (!info) { return luaReturnNil(L); }
        
            const char* ch = (info->quality < 8) ? kQualHexAlpha[info->quality] : "ffffffff";
            char link[256];
            snprintf(link, sizeof(link), "|c%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r", ch, itemId, info->name.c_str());
            lua_pushstring(L, link);
            return 1;
        }},
                {"GetInboxNumItems", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getMailInbox().size() : 0);
            return 1;
        }},
                {"GetInboxHeaderInfo", [](lua_State* L) -> int {
            // GetInboxHeaderInfo(index) → packageIcon, stationeryIcon, sender, subject, money, COD, daysLeft, hasItem, wasRead, wasReturned, textCreated, canReply, isGM
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) { return luaReturnNil(L); }
            const auto& inbox = gh->getMailInbox();
            if (index > static_cast<int>(inbox.size())) { return luaReturnNil(L); }
            const auto& mail = inbox[index - 1];
            lua_pushstring(L, "Interface\\Icons\\INV_Letter_15"); // packageIcon
            lua_pushstring(L, "Interface\\Icons\\INV_Letter_15"); // stationeryIcon
            lua_pushstring(L, mail.senderName.c_str());           // sender
            const std::string subject = gh->getMailDisplaySubject(mail);
            lua_pushstring(L, subject.c_str());                   // subject
            lua_pushnumber(L, mail.money);                        // money (copper)
            lua_pushnumber(L, mail.cod);                          // COD
            lua_pushnumber(L, mail.expirationTime);              // daysLeft (server sends days)
            lua_pushboolean(L, mail.attachments.empty() ? 0 : 1); // hasItem
            lua_pushboolean(L, mail.read ? 1 : 0);               // wasRead
            lua_pushboolean(L, 0);                                // wasReturned
            lua_pushboolean(L, !mail.body.empty() ? 1 : 0);      // textCreated
            lua_pushboolean(L, mail.messageType == 0 ? 1 : 0);   // canReply (player mail only)
            lua_pushboolean(L, 0);                                // isGM
            return 13;
        }},
                {"GetInboxText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) { return luaReturnNil(L); }
            const auto& inbox = gh->getMailInbox();
            if (index > static_cast<int>(inbox.size())) { return luaReturnNil(L); }
            lua_pushstring(L, inbox[index - 1].body.c_str());
            return 1;
        }},
                {"HasNewMail", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnFalse(L); }
            bool hasNew = false;
            for (const auto& m : gh->getMailInbox()) {
                if (!m.read) { hasNew = true; break; }
            }
            lua_pushboolean(L, hasNew ? 1 : 0);
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
