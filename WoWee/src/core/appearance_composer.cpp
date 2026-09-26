#include "core/appearance_composer.hpp"
#include "core/helm_visual.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/game_handler.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace wowee {
namespace core {

namespace {

constexpr uint32_t kAttachShield = 0;
// M2 attachment 11 is the helm; 0 is the shield mount, which is where head
// gear was going — attached successfully, on the forearm, invisible on the head.
constexpr uint32_t kAttachHelm = 11;
constexpr uint32_t kAttachRightHand = 1;
constexpr uint32_t kAttachLeftHand = 2;
constexpr uint32_t kAttachRightHip = 9;
constexpr uint32_t kAttachLeftHip = 10;
constexpr uint32_t kAttachBack = 12;

uint32_t weaponAttachment(bool sheathed, game::EquipSlot slot, uint8_t inventoryType) {
    if (!sheathed) {
        return slot == game::EquipSlot::OFF_HAND ? kAttachLeftHand : kAttachRightHand;
    }

    if (inventoryType == game::InvType::TWO_HAND) return kAttachBack;
    if (inventoryType == game::InvType::SHIELD) return kAttachShield;
    if (inventoryType == game::InvType::ONE_HAND ||
        inventoryType == game::InvType::MAIN_HAND) {
        return slot == game::EquipSlot::OFF_HAND ? kAttachLeftHip : kAttachRightHip;
    }

    // Holdables and other items with no sheath position are hidden, matching
    // the original client rather than pinning books/orbs to an arbitrary bone.
    return UINT32_MAX;
}

glm::mat4 weaponLocalTransform(bool sheathed, game::EquipSlot /*slot*/,
                               uint8_t inventoryType) {
    glm::mat4 transform(1.0f);
    if (!sheathed || inventoryType == game::InvType::SHIELD) return transform;

    if (inventoryType == game::InvType::TWO_HAND) {
        // Weapon models are authored for a hand with their long axis pointing
        // forward. Stand that axis up, cant it across the back, and move it off
        // the spine so the grip sits below the opposite shoulder.
        // Weapon models are authored along local X, which is also the character's
        // front/back axis. First rotate weapon X completely onto character Z, then
        // cant that vertical axis within the Y/Z back plane. This ordering is
        // important: rotating around X cannot change an X-aligned blade.
        // The final innermost roll spins the blade about its own long axis so
        // the flat rests against the back instead of the sharp edge.
        // Values tuned against the live attachment frame. Its Z axis moves the
        // weapon laterally rather than vertically.
        transform = glm::translate(transform, glm::vec3(-0.03f, -0.10f, 0.0f));
        transform = glm::rotate(transform, glm::radians(33.0f), glm::vec3(1, 0, 0));
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(0, 1, 0));
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(1, 0, 0));
    } else {
        // Hip-sheathed one-handers have the same X-aligned long axis. Rotate it
        // onto -Z so the blade points down alongside the leg.
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(0, 1, 0));
    }
    return transform;
}

} // namespace

AppearanceComposer::AppearanceComposer(rendering::Renderer* renderer,
                                       pipeline::AssetManager* assetManager,
                                       game::GameHandler* gameHandler,
                                       pipeline::DBCLayout* dbcLayout,
                                       EntitySpawner* entitySpawner)
    : renderer_(renderer)
    , assetManager_(assetManager)
    , gameHandler_(gameHandler)
    , dbcLayout_(dbcLayout)
    , entitySpawner_(entitySpawner)
{
}

std::string AppearanceComposer::getPlayerModelPath(game::Race race, game::Gender gender) const {
    return game::getPlayerModelPath(race, gender);
}

PlayerTextureInfo AppearanceComposer::resolvePlayerTextures(pipeline::M2Model& model,
                                                            game::Race race, game::Gender gender,
                                                            uint32_t appearanceBytes) {
    PlayerTextureInfo result;

    uint32_t targetRaceId = static_cast<uint32_t>(race);
    uint32_t targetSexId = (gender == game::Gender::FEMALE) ? 1u : 0u;

    // Race name for fallback texture paths
    const char* raceFolderName = "Human";
    switch (race) {
        case game::Race::HUMAN:    raceFolderName = "Human"; break;
        case game::Race::ORC:      raceFolderName = "Orc"; break;
        case game::Race::DWARF:    raceFolderName = "Dwarf"; break;
        case game::Race::NIGHT_ELF: raceFolderName = "NightElf"; break;
        case game::Race::UNDEAD:    raceFolderName = "Scourge"; break;
        case game::Race::TAUREN:    raceFolderName = "Tauren"; break;
        case game::Race::GNOME:     raceFolderName = "Gnome"; break;
        case game::Race::TROLL:     raceFolderName = "Troll"; break;
        case game::Race::BLOOD_ELF: raceFolderName = "BloodElf"; break;
        case game::Race::DRAENEI:   raceFolderName = "Draenei"; break;
        default: break;
    }
    const char* genderFolder = (gender == game::Gender::FEMALE) ? "Female" : "Male";
    std::string raceGender = std::string(raceFolderName) + genderFolder;
    result.bodySkinPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "Skin00_00.blp";
    std::string pelvisPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "NakedPelvisSkin00_00.blp";

    // Extract appearance bytes for texture lookups
    uint8_t charSkinId = appearanceBytes & 0xFF;
    uint8_t charFaceId = (appearanceBytes >> 8) & 0xFF;
    uint8_t charHairStyleId = (appearanceBytes >> 16) & 0xFF;
    uint8_t charHairColorId = (appearanceBytes >> 24) & 0xFF;
    LOG_INFO("Appearance: skin=", static_cast<int>(charSkinId), " face=", static_cast<int>(charFaceId),
             " hairStyle=", static_cast<int>(charHairStyleId), " hairColor=", static_cast<int>(charHairColorId));

    // Parse CharSections.dbc for skin/face/hair/underwear texture paths
    auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
    if (charSectionsDbc) {
        LOG_INFO("CharSections.dbc loaded: ", charSectionsDbc->getRecordCount(), " records");
        const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);
        bool foundSkin = false;
        bool foundUnderwear = false;
        bool foundFaceLower = false;
        bool foundHair = false;
        // Nearest usable face if the exact (variation, colour) pair is absent.
        // Character creation falls back to a synthetic 0..9 range whenever its
        // DBC scan comes up empty, so a character can be created carrying a face
        // number that has no row at all — and then has no face for good, because
        // this lookup used to just not match and say nothing.
        std::string faceAltLower, faceAltUpper;
        bool haveFaceAlt = false;
        for (uint32_t r = 0; r < charSectionsDbc->getRecordCount(); r++) {
            uint32_t raceId = charSectionsDbc->getUInt32(r, csF.raceId);
            uint32_t sexId = charSectionsDbc->getUInt32(r, csF.sexId);
            uint32_t baseSection = charSectionsDbc->getUInt32(r, csF.baseSection);
            uint32_t variationIndex = charSectionsDbc->getUInt32(r, csF.variationIndex);
            uint32_t colorIndex = charSectionsDbc->getUInt32(r, csF.colorIndex);

            if (raceId != targetRaceId || sexId != targetSexId) continue;

            // Section 0 = skin: match by colorIndex = skin byte
            if (baseSection == 0 && !foundSkin && colorIndex == charSkinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                if (!tex1.empty()) {
                    result.bodySkinPath = tex1;
                    foundSkin = true;
                    LOG_INFO("  DBC body skin: ", result.bodySkinPath, " (skin=", static_cast<int>(charSkinId), ")");
                }
            }
            // Section 3 = hair: match variation=hairStyle, color=hairColor
            // Texture1 = hair mesh (type-6). Texture2/3 = scalp lower/upper overlays
            // that must be composited onto the body skin or the head looks bald
            // under the hair cards.
            else if (baseSection == 3 && !foundHair &&
                     variationIndex == charHairStyleId && colorIndex == charHairColorId) {
                result.hairTexturePath = charSectionsDbc->getString(r, csF.texture1);
                result.scalpLowerPath = charSectionsDbc->getString(r, csF.texture2);
                result.scalpUpperPath = charSectionsDbc->getString(r, csF.texture3);
                if (!result.hairTexturePath.empty()) {
                    foundHair = true;
                    LOG_INFO("  DBC hair texture: ", result.hairTexturePath,
                             " (style=", static_cast<int>(charHairStyleId), " color=", static_cast<int>(charHairColorId), ")");
                    if (!result.scalpLowerPath.empty() || !result.scalpUpperPath.empty()) {
                        LOG_INFO("  DBC scalp overlays: lower=", result.scalpLowerPath.empty() ? "(none)" : result.scalpLowerPath,
                                 " upper=", result.scalpUpperPath.empty() ? "(none)" : result.scalpUpperPath);
                    }
                }
            }
            // Section 1 = face: match variation=faceId, colorIndex=skinId
            // Texture1 = face lower, Texture2 = face upper
            else if (baseSection == 1 && !foundFaceLower &&
                     variationIndex == charFaceId && colorIndex == charSkinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                if (!tex1.empty()) {
                    result.faceLowerPath = tex1;
                    LOG_INFO("  DBC face lower: ", result.faceLowerPath);
                }
                if (!tex2.empty()) {
                    result.faceUpperPath = tex2;
                    LOG_INFO("  DBC face upper: ", result.faceUpperPath);
                }
                foundFaceLower = true;
            }
            // Same face variation in another skin colour, or failing that any
            // face at all in the right colour: either beats a blank head.
            else if (baseSection == 1 && !foundFaceLower && !haveFaceAlt &&
                     (variationIndex == charFaceId || colorIndex == charSkinId)) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                if (!tex1.empty()) {
                    faceAltLower = tex1;
                    faceAltUpper = tex2;
                    haveFaceAlt = true;
                }
            }
            // Section 4 = underwear
            else if (baseSection == 4 && !foundUnderwear && colorIndex == charSkinId) {
                for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                    std::string tex = charSectionsDbc->getString(r, f);
                    if (!tex.empty() && assetManager_->fileExists(tex)) {
                        result.underwearPaths.push_back(tex);
                        LOG_INFO("  DBC underwear texture: ", tex);
                    }
                }
                foundUnderwear = !result.underwearPaths.empty();
            }

            if (foundSkin && foundHair && foundFaceLower && foundUnderwear) break;
        }

        if (!foundFaceLower) {
            LOG_WARNING("No DBC face match for face=", static_cast<int>(charFaceId),
                        " skin=", static_cast<int>(charSkinId),
                        " race=", targetRaceId, " sex=", targetSexId,
                        haveFaceAlt ? " — using the nearest face instead"
                                    : " — this character will render with no face");
            if (haveFaceAlt) {
                result.faceLowerPath = faceAltLower;
                result.faceUpperPath = faceAltUpper;
            }
        }

        if (!foundHair) {
            LOG_WARNING("No DBC hair match for style=", static_cast<int>(charHairStyleId),
                        " color=", static_cast<int>(charHairColorId),
                        " race=", targetRaceId, " sex=", targetSexId);
        }
    } else {
        LOG_WARNING("Failed to load CharSections.dbc, using hardcoded textures");
    }

    // Fill model texture slots with resolved paths
    for (auto& tex : model.textures) {
        if (tex.type == 1 && tex.filename.empty()) {
            tex.filename = result.bodySkinPath;
        } else if (tex.type == 6) {
            if (!result.hairTexturePath.empty()) {
                tex.filename = result.hairTexturePath;
            } else if (tex.filename.empty()) {
                tex.filename = std::string("Character\\") + raceFolderName + "\\Hair00_00.blp";
            }
        } else if (tex.type == 8 && tex.filename.empty()) {
            if (!result.underwearPaths.empty()) {
                tex.filename = result.underwearPaths[0];
            } else {
                tex.filename = pelvisPath;
            }
        }
    }

    return result;
}

void AppearanceComposer::compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo) {
    if (!renderer_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;

    // Save skin composite state for re-compositing on equipment changes
    // Include face + scalp textures so compositeWithRegions can rebuild the full base
    bodySkinPath_ = texInfo.bodySkinPath;
    underwearPaths_.clear();
    if (!texInfo.faceLowerPath.empty()) underwearPaths_.push_back(texInfo.faceLowerPath);
    if (!texInfo.faceUpperPath.empty()) underwearPaths_.push_back(texInfo.faceUpperPath);
    if (!texInfo.scalpLowerPath.empty()) underwearPaths_.push_back(texInfo.scalpLowerPath);
    if (!texInfo.scalpUpperPath.empty()) underwearPaths_.push_back(texInfo.scalpUpperPath);
    for (const auto& up : texInfo.underwearPaths) underwearPaths_.push_back(up);

    // Composite body skin + face + scalp hairline + underwear overlays
    {
        std::vector<std::string> layers;
        layers.push_back(texInfo.bodySkinPath);
        if (!texInfo.faceLowerPath.empty()) layers.push_back(texInfo.faceLowerPath);
        if (!texInfo.faceUpperPath.empty()) layers.push_back(texInfo.faceUpperPath);
        if (!texInfo.scalpLowerPath.empty()) layers.push_back(texInfo.scalpLowerPath);
        if (!texInfo.scalpUpperPath.empty()) layers.push_back(texInfo.scalpUpperPath);
        for (const auto& up : texInfo.underwearPaths) {
            layers.push_back(up);
        }
        if (layers.size() > 1) {
            rendering::VkTexture* compositeTex = charRenderer->compositeTextures(layers);
            if (compositeTex != 0) {
                // Find type-1 (skin) texture slot and replace with composite
                // We need model texture info — walk slots via charRenderer
                // Use the model slot ID to find the right texture index
                auto* modelData = charRenderer->getModelData(modelSlotId);
                if (modelData) {
                    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                        if (modelData->textures[ti].type == 1) {
                            charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), compositeTex);
                            skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
                            LOG_INFO("Replaced type-1 texture slot ", ti, " with composited body+face+scalp+underwear");
                            break;
                        }
                    }
                }
            }
        }
    }

    // Override hair texture on GPU (type-6 slot) after model load
    if (!texInfo.hairTexturePath.empty()) {
        rendering::VkTexture* hairTex = charRenderer->loadTexture(texInfo.hairTexturePath);
        if (hairTex) {
            auto* modelData = charRenderer->getModelData(modelSlotId);
            if (modelData) {
                for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                    if (modelData->textures[ti].type == 6) {
                        charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), hairTex);
                        LOG_INFO("Applied DBC hair texture to slot ", ti, ": ", texInfo.hairTexturePath);
                        break;
                    }
                }
            }
        }
    }

    // Find cloak (type-2, Object Skin) texture slot index
    {
        auto* modelData = charRenderer->getModelData(modelSlotId);
        if (modelData) {
            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                if (modelData->textures[ti].type == 2) {
                    cloakTextureSlotIndex_ = static_cast<uint32_t>(ti);
                    LOG_INFO("Cloak texture slot: ", ti);
                    break;
                }
            }
        }
    }
}

std::unordered_set<uint16_t> AppearanceComposer::buildDefaultPlayerGeosets(uint8_t raceId, uint8_t sexId,
                                                                           uint8_t hairStyleId, uint8_t facialId) {
    std::unordered_set<uint16_t> activeGeosets;

    // Look up the correct hair scalp geoset from CharHairGeosets.dbc
    uint16_t selectedHairScalp = 1; // default bald cap
    bool mapped = false;
    if (entitySpawner_) {
        const auto& hairMap = entitySpawner_->getHairGeosetMap();
        uint32_t hairKey = (static_cast<uint32_t>(raceId) << 16) |
                           (static_cast<uint32_t>(sexId) << 8) |
                           static_cast<uint32_t>(hairStyleId);
        auto it = hairMap.find(hairKey);
        if (it != hairMap.end() && it->second > 0) {
            selectedHairScalp = it->second;
            mapped = true;
        }
    }
    if (!mapped && hairStyleId > 0) {
        // Match CharacterPreview fallback so create/select and in-world agree
        // when the DBC map is incomplete.
        selectedHairScalp = static_cast<uint16_t>(std::max<uint8_t>(hairStyleId + 1, 1));
    }

    // Group 0: body base plus exactly one selected hair scalp. Do not enable
    // every non-mapped group-0 submesh as a fallback; if the hair DBC map is
    // unavailable or incomplete, that path activates multiple hair variants.
    activeGeosets.insert(0);  // body base
    activeGeosets.insert(selectedHairScalp);
    // Group 1 hair connector — NPC humanoid path always pairs scalp with
    // (100 + scalpId). Without it, many classic hair styles render as a bald
    // head with missing cards (Night Elf style meshes especially).
    if (selectedHairScalp > 1) {
        activeGeosets.insert(static_cast<uint16_t>(100 + selectedHairScalp));
    } else {
        activeGeosets.insert(kGeosetDefaultConnector);
    }

    LOG_INFO("Player hair geosets: race=", static_cast<int>(raceId),
             " sex=", static_cast<int>(sexId),
             " style=", static_cast<int>(hairStyleId),
             " scalp=", selectedHairScalp,
             " connector=", (selectedHairScalp > 1 ? 100 + selectedHairScalp : 101),
             mapped ? " (DBC)" : " (fallback)");

    // Groups 1xx, 2xx and 3xx are independent facial-feature channels from
    // CharacterFacialHairStyles. They must not be derived from the hairstyle.
    // Note: geoset100 may collide with the hair connector above; the hair
    // connector wins when both resolve to the same id.
    if (entitySpawner_) {
        const auto& facialMap = entitySpawner_->getFacialHairGeosetMap();
        uint32_t facialKey = (static_cast<uint32_t>(raceId) << 16) |
                             (static_cast<uint32_t>(sexId) << 8) |
                             static_cast<uint32_t>(facialId);
        auto it = facialMap.find(facialKey);
        if (it != facialMap.end()) {
            // A zero means this channel has no feature — a night elf female has
            // none on any of the three. Clamping to 1 handed every character the
            // first variant of all three channels instead.
            if (it->second.geoset100 > 0)
                activeGeosets.insert(static_cast<uint16_t>(100 + it->second.geoset100));
            if (it->second.geoset200 > 0)
                activeGeosets.insert(static_cast<uint16_t>(200 + it->second.geoset200));
            if (it->second.geoset300 > 0)
                activeGeosets.insert(static_cast<uint16_t>(300 + it->second.geoset300));
        }
    }

    activeGeosets.insert(kGeosetBareForearms);
    activeGeosets.insert(kGeosetBareShins);
    activeGeosets.insert(kGeosetDefaultEars);
    activeGeosets.insert(kGeosetBareSleeves);
    activeGeosets.insert(kGeosetDefaultKneepads);
    activeGeosets.insert(kGeosetBarePants);
    // Group 15 variant 01 is the body with no cloak. Variant 02 is the cloak
    // mesh, and it draws white until a cloak texture is bound. Other players
    // already switch to 1502 only when inventory type 16 is equipped.
    activeGeosets.insert(kGeosetNoCape);
    activeGeosets.insert(kGeosetBareFeet);
    return activeGeosets;
}

void AppearanceComposer::applyEnchantVisuals(uint32_t charInstanceId, int equipSlotIndex,
                                             uint32_t attachmentId) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || !gameHandler_ || !assetManager_ || !entitySpawner_) return;

    charRenderer->detachWeaponEffects(charInstanceId, attachmentId);

    uint64_t itemGuid = gameHandler_->getEquipSlotGuid(equipSlotIndex);
    if (itemGuid == 0) return;

    // A temporary enchant (sharpening stone, oil) masks the permanent one's visual.
    auto [permEnchantId, tempEnchantId] = gameHandler_->getItemEnchantIds(itemGuid);
    uint32_t enchantId = (tempEnchantId != 0) ? tempEnchantId : permEnchantId;
    if (enchantId == 0) return;

    auto sieDbc     = assetManager_->loadDBC("SpellItemEnchantment.dbc");
    auto visualsDbc = assetManager_->loadDBC("ItemVisuals.dbc");
    auto effectsDbc = assetManager_->loadDBC("ItemVisualEffects.dbc");
    if (!sieDbc || !sieDbc->isLoaded() || !visualsDbc || !visualsDbc->isLoaded() ||
        !effectsDbc || !effectsDbc->isLoaded()) {
        return;
    }

    const auto* sieL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
    auto effectModels = pipeline::resolveEnchantItemVisuals(enchantId, sieDbc.get(),
                                                            visualsDbc.get(), effectsDbc.get(), sieL);

    for (uint32_t visualSlot = 0; visualSlot < effectModels.size(); ++visualSlot) {
        const std::string& modelName = effectModels[visualSlot];
        if (modelName.empty()) continue;

        // DBC stores .mdx paths; the shipped assets are .m2.
        std::string m2Path = modelName;
        size_t dotPos = m2Path.rfind('.');
        m2Path = (dotPos != std::string::npos ? m2Path.substr(0, dotPos) : m2Path) + ".m2";

        pipeline::M2Model effectModel;
        if (!loadWeaponM2(m2Path, effectModel)) {
            LOG_WARNING("Enchant visual: failed to load ", m2Path);
            continue;
        }

        uint32_t effectModelId = entitySpawner_->allocateWeaponModelId();
        if (charRenderer->attachWeaponEffect(charInstanceId, attachmentId, visualSlot,
                                             effectModel, effectModelId)) {
            LOG_INFO("Enchant visual: ", m2Path, " on attachment ", attachmentId,
                     " (enchant ", enchantId, ", visual slot ", visualSlot, ")");
        }
    }
}

bool AppearanceComposer::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    auto m2Data = assetManager_->readFile(m2Path);
    if (m2Data.empty()) return false;
    outModel = pipeline::M2Loader::load(m2Data);
    if (outModel.name.empty()) outModel.name = m2Path;
    // Load skin (WotLK+ M2 format): strip .m2, append 00.skin
    std::string skinPath = m2Path;
    size_t dotPos = skinPath.rfind('.');
    if (dotPos != std::string::npos) skinPath = skinPath.substr(0, dotPos);
    skinPath += "00.skin";
    auto skinData = assetManager_->readFile(skinPath);
    if (!skinData.empty() && outModel.version >= 264)
        pipeline::M2Loader::loadSkin(skinData, outModel);
    return outModel.isValid();
}

// Head gear, which only other players used to get. The local character's
// appearance is assembled here while everyone else's goes through
// EntitySpawner, and the head slot was simply missing from this side: no
// helmet model, and hair left showing through where one should be.
void AppearanceComposer::loadEquippedHelm(game::Inventory& inventory) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t charInstanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    if (!charRenderer || charInstanceId == 0 || !assetManager_ || !gameHandler_) return;

    // Only the helm point. Detaching 0 as well would drop the shield.
    charRenderer->detachWeapon(charInstanceId, kAttachHelm);

    // Hiding the helm is a display choice, not an unequip: the item stays on,
    // the model comes off, and the hair comes back.
    if (!gameHandler_->isHelmVisible()) return;

    const auto& headSlot = inventory.getEquipSlot(game::EquipSlot::HEAD);
    if (headSlot.empty()) return;
    const auto* info = gameHandler_->getItemInfo(headSlot.item.itemId);
    const uint32_t displayId = info && info->valid ? info->displayInfoId
                                                   : headSlot.item.displayInfoId;
    if (displayId == 0) return;

    uint8_t raceId = 0;
    uint8_t genderId = 0;
    if (const auto* ch = gameHandler_->getActiveCharacter()) {
        raceId = static_cast<uint8_t>(ch->race);
        genderId = static_cast<uint8_t>(ch->gender);
    }

    const core::HelmVisual helm =
        core::resolveHelmVisual(*assetManager_, displayId, raceId, genderId);
    if (!helm.valid()) return;

    pipeline::M2Model helmModel;
    std::string helmPath;
    if (!helm.racialModelPath.empty()) {
        helmPath = helm.racialModelPath;
        if (!loadWeaponM2(helmPath, helmModel)) helmModel = {};
    }
    if (!helmModel.isValid()) {
        helmPath = helm.baseModelPath;
        if (!loadWeaponM2(helmPath, helmModel)) return;
    }

    const uint32_t helmModelId = entitySpawner_ ? entitySpawner_->allocateWeaponModelId() : 0;
    const bool attached = charRenderer->attachWeapon(charInstanceId, kAttachHelm, helmModel,
                                                     helmModelId, helm.texturePath);
    if (attached) {
        LOG_INFO("Equipped helm: ", helmPath, " tex: ", helm.texturePath);
    }
}

void AppearanceComposer::loadEquippedWeapons() {
    // Equipment refreshes can arrive during a gather cast. Keep the temporary
    // tool authoritative until the cast-end callback restores real equipment.
    const uint32_t currentInstanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    if (showingMiningPick_ && currentInstanceId == miningPickInstanceId_) return;
    if (showingMiningPick_) {
        showingMiningPick_ = false;
        miningPickInstanceId_ = 0;
    }
    showingRanged_ = false;
    if (renderer_ && renderer_->getAnimationController())
        renderer_->getAnimationController()->setRangedWeaponActive(false);
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized())
        return;
    if (!gameHandler_) return;

    auto* charRenderer = renderer_->getCharacterRenderer();
    uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    auto& inventory = gameHandler_->getInventory();

    loadEquippedHelm(inventory);

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) {
        LOG_WARNING("loadEquippedWeapons: failed to load ItemDisplayInfo.dbc");
        return;
    }
    // Mapping: EquipSlot → held attachment. Sheathed attachment is resolved
    // from the item's InventoryType below.
    struct WeaponSlot {
        game::EquipSlot slot;
        uint32_t attachmentId;
    };
    WeaponSlot weaponSlots[] = {
        { game::EquipSlot::MAIN_HAND, kAttachRightHand },
        { game::EquipSlot::OFF_HAND,  kAttachLeftHand },
    };

    // Equipment reloads and Z toggles can move models between these points.
    // Clear both held and sheathed locations so old copies never remain behind.
    const uint32_t weaponAttachmentPoints[] = {
        kAttachShield, kAttachRightHand, kAttachLeftHand,
        kAttachRightHip, kAttachLeftHip, kAttachBack
    };
    for (uint32_t attachmentId : weaponAttachmentPoints) {
        charRenderer->detachWeapon(charInstanceId, attachmentId);
    }

    bool rightHandFilled = false;

    for (const auto& ws : weaponSlots) {
        const auto& equipSlot = inventory.getEquipSlot(ws.slot);

        // If slot is empty or has no displayInfoId, detach any existing weapon
        if (equipSlot.empty() || equipSlot.item.displayInfoId == 0) {
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        const uint32_t attachmentId = weaponAttachment(
            weaponsSheathed_, ws.slot, equipSlot.item.inventoryType);
        if (attachmentId == UINT32_MAX) continue;

        uint32_t displayInfoId = equipSlot.item.displayInfoId;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) {
            LOG_WARNING("loadEquippedWeapons: displayInfoId ", displayInfoId, " not found in DBC");
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
        std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
        std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);

        if (modelName.empty()) {
            LOG_WARNING("loadEquippedWeapons: empty model name for displayInfoId ", displayInfoId);
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        // Convert .mdx → .m2
        std::string modelFile = modelName;
        {
            size_t dotPos = modelFile.rfind('.');
            if (dotPos != std::string::npos) {
                modelFile = modelFile.substr(0, dotPos) + ".m2";
            } else {
                modelFile += ".m2";
            }
        }

        // Try Weapon directory first, then Shield
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (!loadWeaponM2(m2Path, weaponModel)) {
                LOG_WARNING("loadEquippedWeapons: failed to load ", modelFile);
                charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
                continue;
            }
        }

        // Build texture path
        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
            }
        }

        uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
        const glm::mat4 localTransform = weaponLocalTransform(
            weaponsSheathed_, ws.slot, equipSlot.item.inventoryType);
        bool ok = charRenderer->attachWeapon(charInstanceId, attachmentId,
                                              weaponModel, weaponModelId, texturePath,
                                              localTransform);
        if (ok) {
            LOG_INFO("Equipped weapon: ", m2Path, " at attachment ", attachmentId,
                     weaponsSheathed_ ? " (sheathed)" : " (held)");
            if (ws.slot == game::EquipSlot::MAIN_HAND) rightHandFilled = true;
            applyEnchantVisuals(charInstanceId, static_cast<int>(ws.slot), attachmentId);
        }
    }

    // --- RANGED slot (bow, gun, crossbow, thrown) ---
    // Show ranged weapon in right hand when main hand is empty.
    const auto& rangedSlot = inventory.getEquipSlot(game::EquipSlot::RANGED);
    if (!rightHandFilled && !rangedSlot.empty() && rangedSlot.item.displayInfoId != 0) {
        uint32_t displayInfoId = rangedSlot.item.displayInfoId;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx >= 0) {
            const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
            std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
            std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);

            if (!modelName.empty()) {
                std::string modelFile = modelName;
                {
                    size_t dotPos = modelFile.rfind('.');
                    if (dotPos != std::string::npos) {
                        modelFile = modelFile.substr(0, dotPos) + ".m2";
                    } else {
                        modelFile += ".m2";
                    }
                }

                std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
                pipeline::M2Model weaponModel;
                if (!loadWeaponM2(m2Path, weaponModel)) {
                    m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
                    loadWeaponM2(m2Path, weaponModel);
                }

                if (weaponModel.vertices.size() > 0) {
                    std::string texturePath;
                    if (!textureName.empty()) {
                        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
                        if (!assetManager_->fileExists(texturePath)) {
                            texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
                        }
                    }

                    uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
                    const uint32_t rangedAttachment = weaponsSheathed_
                        ? kAttachBack : kAttachRightHand;
                    const glm::mat4 localTransform = weaponsSheathed_
                        ? weaponLocalTransform(true, game::EquipSlot::MAIN_HAND,
                                               game::InvType::TWO_HAND)
                        : glm::mat4(1.0f);
                    bool ok = charRenderer->attachWeapon(charInstanceId, rangedAttachment,
                                                          weaponModel, weaponModelId, texturePath,
                                                          localTransform);
                    if (ok) {
                        LOG_INFO("Equipped ranged weapon: ", m2Path, " at attachment ",
                                 rangedAttachment, weaponsSheathed_ ? " (sheathed)" : " (held)");
                    }
                }
            }
        }
    }
}

void AppearanceComposer::showMiningPick(bool show) {
    if (show == showingMiningPick_) return;

    if (!show) {
        showingMiningPick_ = false;
        miningPickInstanceId_ = 0;
        loadEquippedWeapons();
        return;
    }

    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ ||
        !assetManager_->isInitialized() || !entitySpawner_) {
        return;
    }

    auto* charRenderer = renderer_->getCharacterRenderer();
    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    // Item 2901 (Mining Pick) resolves to ItemDisplayInfo 6568 in the WotLK DBC.
    constexpr uint32_t kMiningPickDisplayId = 6568;
    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;

    const int32_t recIdx = displayInfoDbc->findRecordById(kMiningPickDisplayId);
    if (recIdx < 0) {
        LOG_WARNING("showMiningPick: displayInfoId ", kMiningPickDisplayId,
                    " not found in DBC");
        return;
    }

    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::string modelName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
    std::string textureName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
    if (modelName.empty()) return;

    const size_t dotPos = modelName.rfind('.');
    std::string modelFile = dotPos == std::string::npos
        ? modelName + ".m2" : modelName.substr(0, dotPos) + ".m2";
    const std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;

    pipeline::M2Model pickModel;
    if (!loadWeaponM2(m2Path, pickModel)) {
        LOG_WARNING("showMiningPick: failed to load ", m2Path);
        return;
    }

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
    }

    charRenderer->detachWeapon(charInstanceId, kAttachRightHand);
    const uint32_t modelId = entitySpawner_->allocateWeaponModelId();
    if (charRenderer->attachWeapon(charInstanceId, kAttachRightHand, pickModel,
                                   modelId, texturePath)) {
        showingMiningPick_ = true;
        miningPickInstanceId_ = charInstanceId;
        showingRanged_ = false;
        if (renderer_->getAnimationController())
            renderer_->getAnimationController()->setRangedWeaponActive(false);
        LOG_INFO("Mining pick attached at right hand: ", m2Path);
    } else {
        // Do not leave the player empty-handed if the temporary model failed.
        loadEquippedWeapons();
    }
}

void AppearanceComposer::showFishingPole(bool show) {
    if (show == showingFishingPole_) return;

    if (!show) {
        showingFishingPole_ = false;
        loadEquippedWeapons();
        return;
    }
    if (!renderer_ || !renderer_->getCharacterRenderer() || !gameHandler_ ||
        !assetManager_ || !assetManager_->isInitialized() || !entitySpawner_) {
        return;
    }

    const auto isFishingPole = [this](const game::ItemSlot& slot) {
        if (slot.empty()) return false;
        if (slot.item.subclassName == "Fishing Pole") return true;
        const auto* info = gameHandler_->getItemInfo(slot.item.itemId);
        return info && info->valid && info->itemClass == 2 && info->subClass == 20;
    };

    const game::ItemSlot* pole = nullptr;
    const auto& inventory = gameHandler_->getInventory();
    const auto& mainHand = inventory.getEquipSlot(game::EquipSlot::MAIN_HAND);
    if (isFishingPole(mainHand)) pole = &mainHand;
    for (int i = 0; !pole && i < inventory.getBackpackSize(); ++i) {
        const auto& slot = inventory.getBackpackSlot(i);
        if (isFishingPole(slot)) pole = &slot;
    }
    for (int bag = 0; !pole && bag < game::Inventory::NUM_BAG_SLOTS; ++bag) {
        for (int slotIndex = 0; !pole && slotIndex < inventory.getBagSize(bag); ++slotIndex) {
            const auto& slot = inventory.getBagSlot(bag, slotIndex);
            if (isFishingPole(slot)) pole = &slot;
        }
    }
    if (!pole || pole->item.displayInfoId == 0) {
        LOG_WARNING("showFishingPole: no fishing pole with display data found in inventory");
        return;
    }

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const int32_t recIdx = displayInfoDbc->findRecordById(pole->item.displayInfoId);
    if (recIdx < 0) return;

    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::string modelName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
    std::string textureName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
    if (modelName.empty()) return;

    const size_t dotPos = modelName.rfind('.');
    const std::string modelFile = dotPos == std::string::npos
        ? modelName + ".m2" : modelName.substr(0, dotPos) + ".m2";
    std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
    pipeline::M2Model poleModel;
    if (!loadWeaponM2(m2Path, poleModel)) return;

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
    }

    auto* charRenderer = renderer_->getCharacterRenderer();
    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;
    charRenderer->detachWeapon(charInstanceId, kAttachRightHand);
    const uint32_t modelId = entitySpawner_->allocateWeaponModelId();
    if (charRenderer->attachWeapon(charInstanceId, kAttachRightHand, poleModel,
                                   modelId, texturePath)) {
        showingFishingPole_ = true;
        showingRanged_ = false;
        if (renderer_->getAnimationController())
            renderer_->getAnimationController()->setRangedWeaponActive(false);
        LOG_INFO("Fishing pole attached at right hand: ", m2Path);
    } else {
        loadEquippedWeapons();
    }
}

void AppearanceComposer::showRangedWeapon(bool show) {
    if (show == showingRanged_) return;

    if (!renderer_ || !renderer_->getCharacterRenderer() || !gameHandler_ || !assetManager_ || !assetManager_->isInitialized())
        return;

    auto* charRenderer = renderer_->getCharacterRenderer();
    uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    if (!show) {
        showingRanged_ = false;
        if (renderer_->getAnimationController())
            renderer_->getAnimationController()->setRangedWeaponActive(false);
        // Swap back to normal melee weapons
        loadEquippedWeapons();
        return;
    }

    auto& inventory = gameHandler_->getInventory();
    const auto& rangedSlot = inventory.getEquipSlot(game::EquipSlot::RANGED);
    if (rangedSlot.empty() || rangedSlot.item.displayInfoId == 0) return;

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;

    uint32_t displayInfoId = rangedSlot.item.displayInfoId;
    int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
    if (recIdx < 0) return;

    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
    std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
    if (modelName.empty()) return;

    std::string modelFile = modelName;
    {
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos)
            modelFile = modelFile.substr(0, dotPos) + ".m2";
        else
            modelFile += ".m2";
    }

    std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
    pipeline::M2Model weaponModel;
    if (!loadWeaponM2(m2Path, weaponModel)) {
        m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
        if (!loadWeaponM2(m2Path, weaponModel)) return;
    }

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
        if (!assetManager_->fileExists(texturePath))
            texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
    }

    // Detach current right-hand weapon and attach ranged weapon
    charRenderer->detachWeapon(charInstanceId, 1);
    uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
    bool ok = charRenderer->attachWeapon(charInstanceId, 1, weaponModel, weaponModelId, texturePath);
    if (ok) {
        showingRanged_ = true;
        if (renderer_->getAnimationController())
            renderer_->getAnimationController()->setRangedWeaponActive(true);
        LOG_INFO("Swapped to ranged weapon: ", m2Path, " at attachment 1 (right hand)");
    }
}

} // namespace core
} // namespace wowee
