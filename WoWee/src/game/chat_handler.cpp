#include "game/chat_handler.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/opcode_table.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace wowee {
namespace game {

namespace {

void fireChatMessageEvent(GameHandler& owner, const MessageChatData& data,
                          const std::string& senderName, uint64_t guid) {
    if (!owner.addonEventCallbackRef()) return;
    std::string eventName = "CHAT_MSG_";
    eventName += getChatTypeString(data.type);
    char guidBuf[32];
    snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
    int channelIndex = 0;
    if (data.type == ChatType::CHANNEL && owner.getChatHandler())
        channelIndex = owner.getChatHandler()->getChannelIndex(data.channelName);
    owner.addonEventCallbackRef()(eventName, {
        data.message,
        senderName,
        chatLanguageName(data.language),
        data.channelName,
        "",
        "",
        "0",
        std::to_string(channelIndex),
        data.channelName,
        "0",
        "0",
        guidBuf
    });
}

} // namespace

ChatHandler::ChatHandler(GameHandler& owner)
    : owner_(owner) {
    initializeChatLog();
}

namespace {

bool isTruthyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool isFalsyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.empty() || lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

std::string escapeChatLogField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string formatChatLogTimestamp(std::chrono::system_clock::time_point timestamp) {
    auto time = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string formatChatLogGuid(uint64_t guid) {
    if (guid == 0) return "";
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setfill('0') << std::setw(16) << guid;
    return oss.str();
}

std::string chatParticipant(const std::string& name, uint64_t guid, const char* fallback) {
    if (!name.empty()) return name;
    if (guid != 0) return formatChatLogGuid(guid);
    return fallback ? fallback : "";
}

bool chatPacketDiagEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("WOWEE_CHAT_PACKET_DIAG");
        if (!raw) return false;
        return !isFalsyEnvValue(raw);
    }();
    return enabled;
}

} // namespace

void ChatHandler::initializeChatLog() {
    const char* enabledRaw = std::getenv("WOWEE_CHAT_LOG");
    if (!enabledRaw) return;

    std::string enabledValue(enabledRaw);
    if (isFalsyEnvValue(enabledValue)) return;

    const char* pathRaw = std::getenv("WOWEE_CHAT_LOG_PATH");
    const bool hasPathOverride = pathRaw && *pathRaw;
    chatLogPath_ = hasPathOverride ? pathRaw : "logs/chat.log";
    if (!hasPathOverride && !isTruthyEnvValue(enabledValue) && !enabledValue.empty()) {
        chatLogPath_ = enabledValue;
    }

    std::filesystem::path path(chatLogPath_);
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    chatLogStream_.open(path, std::ios::out | std::ios::app);
    if (!chatLogStream_.is_open()) {
        LOG_WARNING("Chat external log requested but could not open: ", path.string());
        return;
    }

    chatLogEnabled_ = true;
    chatLogInitialized_ = true;
    chatLogStream_ << "# WoWee chat log started " << formatChatLogTimestamp(std::chrono::system_clock::now()) << '\n';
    chatLogStream_ << "# timestamp\tsource\ttype\tsender\tsender_guid\treceiver\treceiver_guid\tchannel\tmessage\n";
    chatLogStream_.flush();
    LOG_INFO("External chat logging enabled: ", path.string());
}

void ChatHandler::logChatMessage(const MessageChatData& msg, const char* source) {
    if (!chatLogEnabled_ || !chatLogInitialized_ || !chatLogStream_.is_open()) return;
    if (msg.message.empty()) return;

    const std::string sender = chatParticipant(msg.senderName, msg.senderGuid, "System");
    const std::string receiver = chatParticipant(msg.receiverName, msg.receiverGuid, "");
    chatLogStream_
        << formatChatLogTimestamp(msg.timestamp) << '\t'
        << (source ? source : "") << '\t'
        << getChatTypeString(msg.type) << '\t'
        << escapeChatLogField(sender) << '\t'
        << formatChatLogGuid(msg.senderGuid) << '\t'
        << escapeChatLogField(receiver) << '\t'
        << formatChatLogGuid(msg.receiverGuid) << '\t'
        << escapeChatLogField(msg.channelName) << '\t'
        << escapeChatLogField(msg.message) << '\n';
    chatLogStream_.flush();
}

void ChatHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet);
    };
    table[Opcode::SMSG_GM_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        // SMSG_GM_MESSAGECHAT has the same header as SMSG_MESSAGECHAT
        // (type[1]+lang[4]+senderGuid[8]+unk[4] = 17 bytes) followed by an
        // extra gmNameLen[4]+gmName[N] before the type-specific body.
        // Strip the GM name field to produce standard SMSG_MESSAGECHAT format.
        if (!packet.hasRemaining(21)) return; // 17 header + 4 gmNameLen min
        uint8_t  type       = packet.readUInt8();
        uint32_t lang       = packet.readUInt32();
        uint64_t senderGuid = packet.readUInt64();
        uint32_t unk        = packet.readUInt32();
        uint32_t gmNameLen  = packet.readUInt32();
        if (!packet.hasRemaining(gmNameLen)) return;
        packet.setReadPos(packet.getReadPos() + gmNameLen); // skip gmName

        // Rebuild as regular SMSG_MESSAGECHAT (header + remaining body)
        network::Packet regular(0);
        regular.writeUInt8(type);
        regular.writeUInt32(lang);
        regular.writeUInt64(senderGuid);
        regular.writeUInt32(unk);
        const auto& raw = packet.getData();
        size_t pos = packet.getReadPos();
        if (pos < raw.size())
            regular.writeBytes(raw.data() + pos, raw.size() - pos);
        handleMessageChat(regular);
    };
    table[Opcode::SMSG_TEXT_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleTextEmote(packet);
    };
    table[Opcode::SMSG_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        if (!packet.hasRemaining(12)) return;
        uint32_t emoteId    = packet.readUInt32();
        uint64_t sourceGuid = packet.readUInt64();
        uint32_t animId = rendering::AnimationController::getEmoteAnimByEmotesId(emoteId);
        // Emotes.dbc EmoteSpecProc distinguishes persistent STATE_ emotes from
        // one-shots. Emote 0 (ONESHOT_NONE) cancels a one-shot but must not
        // clear a UNIT_NPC_EMOTESTATE work loop, so it is forwarded as non-state.
        const bool isState = emoteId != 0 &&
            rendering::AnimationController::isStateEmoteById(emoteId);
        if (owner_.emoteAnimCallbackRef() && sourceGuid != 0 && (animId != 0 || emoteId == 0)) {
            owner_.emoteAnimCallbackRef()(sourceGuid, animId, isState);
        } else if (emoteId != 0 && animId == 0) {
            LOG_DEBUG("SMSG_EMOTE emoteId=", emoteId, " had no Emotes.dbc animation mapping");
        }
    };
    table[Opcode::SMSG_CHANNEL_NOTIFY] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD ||
            owner_.getState() == WorldState::ENTERING_WORLD)
            handleChannelNotify(packet);
    };
    table[Opcode::SMSG_CHAT_PLAYER_NOT_FOUND] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty()) addSystemChatMessage("No player named '" + name + "' is currently playing.");
    };
    table[Opcode::SMSG_CHAT_PLAYER_AMBIGUOUS] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty()) addSystemChatMessage("Player name '" + name + "' is ambiguous.");
    };
    table[Opcode::SMSG_CHAT_WRONG_FACTION] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You cannot send messages to members of that faction.");
        addSystemChatMessage("You cannot send messages to members of that faction.");
    };
    table[Opcode::SMSG_CHAT_NOT_IN_PARTY] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You are not in a party.");
        addSystemChatMessage("You are not in a party.");
    };
    table[Opcode::SMSG_CHAT_RESTRICTED] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You cannot send chat messages in this area.");
        addSystemChatMessage("You cannot send chat messages in this area.");
    };

    // ---- Channel list ----

    // ---- Server / defense / area-trigger messages (moved from GameHandler) ----
    table[Opcode::SMSG_DEFENSE_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(5)) {
            /*uint32_t zoneId =*/ packet.readUInt32();
            std::string defMsg = packet.readString();
            if (!defMsg.empty()) addSystemChatMessage("[Defense] " + defMsg);
        }
    };
    // Server messages
    table[Opcode::SMSG_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t msgType = packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                std::string prefix;
                switch (msgType) {
                    case 1: prefix = "[Shutdown] ";   owner_.addUIError("Server shutdown: " + msg);  break;
                    case 2: prefix = "[Restart] ";    owner_.addUIError("Server restart: " + msg);   break;
                    case 4: prefix = "[Shutdown cancelled] "; break;
                    case 5: prefix = "[Restart cancelled] ";  break;
                    default: prefix = "[Server] "; break;
                }
                addSystemChatMessage(prefix + msg);
            }
        }
    };
    table[Opcode::SMSG_CHAT_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            /*uint32_t msgType =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) addSystemChatMessage("[Announcement] " + msg);
        }
    };
    table[Opcode::SMSG_AREA_TRIGGER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            /*uint32_t len =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                owner_.addUIError(msg);
                addSystemChatMessage(msg);
                owner_.areaTriggerMsgsRef().push_back(msg);
            }
        }
    };

    table[Opcode::SMSG_CHANNEL_LIST] = [this](network::Packet& p) { handleChannelList(p); };
}

void ChatHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send chat in state: ", static_cast<int>(owner_.getState()));
        return;
    }

    if (message.empty()) {
        LOG_WARNING("Cannot send empty chat message");
        return;
    }

    LOG_INFO("OUTGOING CHAT: type=", static_cast<int>(type),
             " (", getChatTypeString(type), ") target='", target, "' msg='", message.substr(0, 60), "'");

    // Use the player's faction language. AzerothCore rejects wrong language.
    // Alliance races: Human(1), Dwarf(3), NightElf(4), Gnome(7), Draenei(11) → COMMON (7)
    // Horde races: Orc(2), Undead(5), Tauren(6), Troll(8), BloodElf(10) → ORCISH (1)
    uint8_t race = owner_.getPlayerRace();
    bool isHorde = (race == 2 || race == 5 || race == 6 || race == 8 || race == 10);
    ChatLanguage language = isHorde ? ChatLanguage::ORCISH : ChatLanguage::COMMON;

    auto packet = MessageChatPacket::build(type, language, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);

    // Add local echo so the player sees their own message immediately
    MessageChatData echo;
    echo.senderGuid = owner_.getPlayerGuid();
    echo.language = language;
    echo.message = message;

    auto nameIt = owner_.getPlayerNameCache().find(owner_.getPlayerGuid());
    if (nameIt != owner_.getPlayerNameCache().end()) {
        echo.senderName = nameIt->second;
    } else if (const Character* active = owner_.getActiveCharacter()) {
        echo.senderName = active->name;
    }

    if (type == ChatType::WHISPER) {
        echo.type = ChatType::WHISPER_INFORM;
        echo.receiverName = target;
    } else {
        echo.type = type;
    }

    if (type == ChatType::CHANNEL) {
        echo.channelName = target;
    }

    addLocalChatMessage(echo);
}

void ChatHandler::sendAddonMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD || message.empty()) return;

    auto packet = MessageChatPacket::build(type, ChatLanguage::ADDON, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT addon type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);
}

void ChatHandler::handleMessageChat(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_MESSAGECHAT");
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG RX SMSG_MESSAGECHAT bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }

    MessageChatData data;
    if (!owner_.getPacketParsers()->parseMessageChat(packet, data)) {
        const auto& raw = packet.getData();
        LOG_WARNING("Failed to parse SMSG_MESSAGECHAT, size=", packet.getSize(),
                    " data=[", raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
        return;
    }
    if (chatPacketDiagEnabled()) {
        LOG_WARNING("CHAT PACKET DIAG PARSED SMSG_MESSAGECHAT type=", getChatTypeString(data.type),
                    " sender=", formatChatLogGuid(data.senderGuid), " senderName='", data.senderName,
                    "' receiver=", formatChatLogGuid(data.receiverGuid), " receiverName='", data.receiverName,
                    "' channel='", data.channelName, "' msg='", data.message, "'");
    }
    LOG_DEBUG("INCOMING CHAT: type=", static_cast<int>(data.type),
             " (", getChatTypeString(data.type), ") sender=0x", std::hex, data.senderGuid, std::dec,
             " '", data.senderName, "' msg='", data.message.substr(0, 60), "'");

    // WoW servers echo successful outgoing whispers as WHISPER_INFORM, but the
    // client already creates a local WHISPER_INFORM row immediately on send.
    if (data.type == ChatType::WHISPER_INFORM) {
        return;
    }

    // Skip server echo of our own messages (we already added a local echo)
    if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0) {
        if (data.type == ChatType::WHISPER && !data.senderName.empty()) {
            owner_.lastWhisperSenderRef() = data.senderName;
        }
        return;
    }

    // Resolve sender name from entity/cache if not already set by parser
    if (data.senderName.empty() && data.senderGuid != 0) {
        auto nameIt = owner_.getPlayerNameCache().find(data.senderGuid);
        if (nameIt != owner_.getPlayerNameCache().end()) {
            data.senderName = nameIt->second;
        } else {
            auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
            if (entity) {
                if (entity->getType() == ObjectType::PLAYER) {
                    auto player = std::dynamic_pointer_cast<Player>(entity);
                    if (player && !player->getName().empty()) {
                        data.senderName = player->getName();
                    }
                } else if (entity->getType() == ObjectType::UNIT) {
                    auto unit = std::dynamic_pointer_cast<Unit>(entity);
                    if (unit && !unit->getName().empty()) {
                        data.senderName = unit->getName();
                    }
                }
            }
        }

        if (data.senderName.empty()) {
            const auto& partyData = owner_.getPartyData();
            for (const auto& member : partyData.members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        if (data.senderName.empty()) {
            owner_.queryPlayerName(data.senderGuid);
        }
    }

    if (data.message.empty()) {
        return;
    }

    if (data.type == ChatType::ACHIEVEMENT ||
        data.type == ChatType::GUILD_ACHIEVEMENT) {
        auto replaceAll = [](std::string& text, const std::string& marker,
                             const std::string& replacement) {
            size_t pos = 0;
            while ((pos = text.find(marker, pos)) != std::string::npos) {
                text.replace(pos, marker.size(), replacement);
                pos += replacement.size();
            }
        };

        if (data.achievementId != 0) {
            owner_.ensureAchievementNamesLoaded();
            std::string achievementName = owner_.getAchievementName(data.achievementId);
            if (achievementName.empty()) {
                achievementName = "Achievement #" + std::to_string(data.achievementId);
            }
            std::ostringstream link;
            link << "|cffffff00|Hachievement:" << data.achievementId
                 << ":0:0:0:0:0:0:0:0:0|h[" << achievementName << "]|h|r";
            replaceAll(data.message, "$a", link.str());
        }
    }

    // Addon messages use the same chat types as player messages, but belong to
    // CHAT_MSG_ADDON and must never be inserted into visible chat history.
    std::string addonPrefix;
    std::string addonPayload;
    if (decodeAddonChatPayload(data, addonPrefix, addonPayload)) {
        if (owner_.addonEventCallbackRef() && !addonPrefix.empty()) {
            owner_.addonEventCallbackRef()(
                "CHAT_MSG_ADDON",
                {addonPrefix, addonPayload, getChatTypeString(data.type), data.senderName});
        }
        return;
    }

    // Server monster messages use %s as a placeholder for the creature's name.
    if (!data.senderName.empty() && (
            data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
            data.type == ChatType::MONSTER_EMOTE || data.type == ChatType::MONSTER_WHISPER ||
            data.type == ChatType::MONSTER_PARTY ||
            data.type == ChatType::RAID_BOSS_EMOTE || data.type == ChatType::RAID_BOSS_WHISPER)) {
        size_t pos = data.message.find("%s");
        while (pos != std::string::npos) {
            data.message.replace(pos, 2, data.senderName);
            pos = data.message.find("%s", pos + data.senderName.size());
        }
    }

    // Filter BG/Arena queue announcer spam (server-side modules on
    // ChromieCraft/AzerothCore). Common formats:
    //   |cffff0000[BG Queue Announcer]:|r ...
    //   |cffff0000[Arena Queue Announcer]:|r ...
    //   |cFFFFA500<player> joined : |cFF00FFFF2x2|r
    //   |cFFFFA500<player> exited |cFF00FFFF3x3|r
    // The third/fourth forms drop the "Queue Announcer" prefix entirely, so
    // we also detect the announcer-shaped pattern: a colored message that
    // names an arena/BG bracket suffix like "2x2|r" / "3v3|r".
    {
        const auto& msg = data.message;
        auto containsCI = [&](const char* needle) {
            const size_t nlen = std::strlen(needle);
            if (msg.size() < nlen) return false;
            const size_t last = msg.size() - nlen;
            for (size_t i = 0; i <= last; ++i) {
                bool match = true;
                for (size_t j = 0; j < nlen; ++j) {
                    unsigned char a = static_cast<unsigned char>(msg[i + j]);
                    unsigned char b = static_cast<unsigned char>(needle[j]);
                    if (std::tolower(a) != std::tolower(b)) { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        };
        if (containsCI("queue announcer") || containsCI("queue status")) {
            return;
        }
        // Pattern-based catch for prefix-less variants. Require the message to
        // contain a color code (server-formatted) AND an arena/BG bracket token
        // immediately followed by |r AND a verb word ("joined", "exited",
        // "left", "entered"). Plain player chat won't hit all three.
        const bool hasColor = msg.find("|c") != std::string::npos;
        if (hasColor) {
            static const char* kBracketTokens[] = {
                "2x2|r", "3x3|r", "5x5|r",
                "2v2|r", "3v3|r", "5v5|r",
                "2X2|r", "3X3|r", "5X5|r",
                "2V2|r", "3V3|r", "5V5|r",
            };
            bool hasBracket = false;
            for (const char* t : kBracketTokens) {
                if (msg.find(t) != std::string::npos) { hasBracket = true; break; }
            }
            if (hasBracket && (containsCI("joined") || containsCI("exited") ||
                               containsCI(" left ") || containsCI("entered") ||
                               containsCI("queue"))) {
                return;
            }
        }
    }

    // Filter officer chat if player doesn't have officer chat permission.
    // Some servers send officer chat to all guild members regardless of rank.
    // WoW guild right bit 0x40 = GR_RIGHT_OFFCHATSPEAK, 0x80 = GR_RIGHT_OFFCHATLISTEN
    if (data.type == ChatType::OFFICER) {
        const auto& roster = owner_.getGuildRoster();
        uint64_t myGuid = owner_.getPlayerGuid();
        uint32_t myRankIdx = 0;
        for (const auto& m : roster.members) {
            if (m.guid == myGuid) { myRankIdx = m.rankIndex; break; }
        }
        if (myRankIdx < roster.ranks.size()) {
            uint32_t rights = roster.ranks[myRankIdx].rights;
            if (!(rights & 0x80)) { // GR_RIGHT_OFFCHATLISTEN = 0x80
                return; // Don't show officer chat to non-officers
            }
        }
    }

    // Filter addon-to-addon whispers (GearScore, DBM, oRA, etc.) from player chat.
    // These are invisible in the real WoW client.
    if (data.type == ChatType::WHISPER || data.type == ChatType::WHISPER_INFORM) {
        const auto& msg = data.message;
        if (msg.size() >= 3 && (
            msg.rfind("GS_", 0) == 0 ||          // GearScore
            msg.rfind("DVNE", 0) == 0 ||          // DBM (DeadlyBossMods)
            msg.rfind("oRA", 0) == 0 ||            // oRA raid addon
            msg.rfind("BWVQ", 0) == 0 ||           // BigWigs
            msg.rfind("AVR", 0) == 0 ||            // AVR (Augmented Virtual Reality)
            msg.rfind("\t", 0) == 0 ||             // Tab-prefixed addon messages
            (msg.size() > 4 && static_cast<unsigned char>(msg[0]) > 127))) {  // Binary data
            return; // Silently discard addon whisper
        }
    }

    // Add to chat history
    data.uid = ++chatUidCounter_;
    chatHistory_.push_back(data);
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.erase(chatHistory_.begin());
    }
    logChatMessage(data, "server");

    // Track whisper sender for /r command
    if (data.type == ChatType::WHISPER) {
        // Always store GUID so getLastWhisperSender() can resolve the name
        // from the player name cache even if name wasn't available yet
        if (data.senderGuid != 0)
            owner_.lastWhisperSenderGuidRef() = data.senderGuid;
        if (!data.senderName.empty())
            owner_.lastWhisperSenderRef() = data.senderName;

        if (!data.senderName.empty()) {
            // Only auto-reply once per sender per AFK/DND session to prevent loops
            if (owner_.afkStatusRef() && afkAutoRepliedSenders_.insert(data.senderName).second) {
                std::string reply = owner_.afkMessageRef().empty() ? "Away from Keyboard" : owner_.afkMessageRef();
                sendChatMessage(ChatType::WHISPER, "<AFK> " + reply, data.senderName);
            } else if (owner_.dndStatusRef() && afkAutoRepliedSenders_.insert(data.senderName).second) {
                std::string reply = owner_.dndMessageRef().empty() ? "Do Not Disturb" : owner_.dndMessageRef();
                sendChatMessage(ChatType::WHISPER, "<DND> " + reply, data.senderName);
            }
        }
    }

    // Trigger chat bubble for SAY/YELL messages from others
    if (owner_.chatBubbleCallbackRef() && data.senderGuid != 0) {
        if (data.type == ChatType::SAY || data.type == ChatType::YELL ||
            data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
            data.type == ChatType::MONSTER_PARTY) {
            bool isYell = (data.type == ChatType::YELL || data.type == ChatType::MONSTER_YELL);
            owner_.chatBubbleCallbackRef()(data.senderGuid, data.message, isYell);
        }
    }

    // Log the message
    std::string senderInfo;
    if (!data.senderName.empty()) {
        senderInfo = data.senderName;
    } else if (data.senderGuid != 0) {
        senderInfo = "Unknown-" + std::to_string(data.senderGuid);
    } else {
        senderInfo = "System";
    }

    std::string channelInfo;
    if (!data.channelName.empty()) {
        channelInfo = "[" + data.channelName + "] ";
    }

    LOG_DEBUG("[", getChatTypeString(data.type), "] ", channelInfo, senderInfo, ": ", data.message);

    // Fire CHAT_MSG_* addon events
    if (owner_.addonChatCallbackRef()) owner_.addonChatCallbackRef()(data);
    fireChatMessageEvent(owner_, data, data.senderName, data.senderGuid);
}

void ChatHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = TextEmotePacket::build(textEmoteId, targetGuid);
    owner_.getSocket()->send(packet);
}

void ChatHandler::handleTextEmote(network::Packet& packet) {
    const bool legacyFormat = isClassicLikeExpansion();
    TextEmoteData data;
    if (!TextEmoteParser::parse(packet, data, legacyFormat)) {
        LOG_WARNING("Failed to parse SMSG_TEXT_EMOTE");
        return;
    }

    if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0) {
        return;
    }

    std::string senderName = owner_.lookupName(data.senderGuid);
    // SMSG_TEXT_EMOTE is the narrated chat line. The server sends the actual
    // visual separately in SMSG_EMOTE; replaying an animation here restarts
    // one-shots and replaces correctly resolved STATE_* loops such as /dance.
    if (senderName.empty()) {
        const auto& partyData = owner_.getPartyData();
        for (const auto& member : partyData.members) {
            if (member.guid == data.senderGuid && !member.name.empty()) {
                senderName = member.name;
                break;
            }
        }
    }

    if (senderName.empty()) {
        owner_.queryPlayerName(data.senderGuid);
        LOG_DEBUG("Deferred chat text for unresolved SMSG_TEXT_EMOTE sender=0x",
                  std::hex, data.senderGuid, std::dec,
                  " emoteId=", data.textEmoteId);
        return;
    }

    const std::string* targetPtr = data.targetName.empty() ? nullptr : &data.targetName;
    std::string emoteText = rendering::AnimationController::getEmoteTextByDbcId(data.textEmoteId, senderName, targetPtr);
    if (emoteText.empty()) {
        emoteText = data.targetName.empty()
            ? senderName + " performs an emote."
            : senderName + " performs an emote at " + data.targetName + ".";
    }

    MessageChatData chatMsg;
    chatMsg.type = ChatType::TEXT_EMOTE;
    chatMsg.language = ChatLanguage::COMMON;
    chatMsg.senderGuid = data.senderGuid;
    chatMsg.senderName = senderName;
    chatMsg.message = emoteText;

    addLocalChatMessage(chatMsg);

    LOG_INFO("TEXT_EMOTE from ", senderName, " (emoteId=", data.textEmoteId, ")");
}

void ChatHandler::joinChannel(const std::string& channelName, const std::string& password) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildJoinChannel(channelName, password)
        : JoinChannelPacket::build(channelName, password);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting to join channel: ", channelName);
}

void ChatHandler::leaveChannel(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildLeaveChannel(channelName)
        : LeaveChannelPacket::build(channelName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting to leave channel: ", channelName);
}

std::string ChatHandler::getChannelByIndex(int index) const {
    if (index < 1 || index > static_cast<int>(joinedChannels_.size())) return "";
    return joinedChannels_[index - 1];
}

int ChatHandler::getChannelIndex(const std::string& channelName) const {
    for (int i = 0; i < static_cast<int>(joinedChannels_.size()); ++i) {
        if (joinedChannels_[i] == channelName) return i + 1;
    }
    return 0;
}

void ChatHandler::handleChannelNotify(network::Packet& packet) {
    ChannelNotifyData data;
    if (!ChannelNotifyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_CHANNEL_NOTIFY");
        return;
    }

    switch (data.notifyType) {
        case ChannelNotifyType::YOU_JOINED: {
            if (std::find(joinedChannels_.begin(), joinedChannels_.end(), data.channelName) == joinedChannels_.end()) {
                joinedChannels_.push_back(data.channelName);
            }
            MessageChatData msg;
            msg.type = ChatType::SYSTEM;
            msg.message = "Joined channel: " + data.channelName;
            addLocalChatMessage(msg);
            LOG_INFO("Joined channel: ", data.channelName);
            break;
        }
        case ChannelNotifyType::YOU_LEFT: {
            joinedChannels_.erase(
                std::remove(joinedChannels_.begin(), joinedChannels_.end(), data.channelName),
                joinedChannels_.end());
            MessageChatData msg;
            msg.type = ChatType::SYSTEM;
            msg.message = "Left channel: " + data.channelName;
            addLocalChatMessage(msg);
            LOG_INFO("Left channel: ", data.channelName);
            break;
        }
        case ChannelNotifyType::PLAYER_ALREADY_MEMBER: {
            // Server confirms we're in this channel but our local list doesn't have it yet —
            // can happen after reconnect or if the join notification was missed.
            if (std::find(joinedChannels_.begin(), joinedChannels_.end(), data.channelName) == joinedChannels_.end()) {
                joinedChannels_.push_back(data.channelName);
                LOG_INFO("Already in channel: ", data.channelName);
            }
            break;
        }
        case ChannelNotifyType::NOT_IN_AREA:
            addSystemChatMessage("You must be in the area to join '" + data.channelName + "'.");
            LOG_DEBUG("Cannot join channel ", data.channelName, " (not in area)");
            break;
        case ChannelNotifyType::WRONG_PASSWORD:
            addSystemChatMessage("Wrong password for channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::NOT_MEMBER:
            addSystemChatMessage("You are not in channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::NOT_MODERATOR:
            addSystemChatMessage("You are not a moderator of '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::MUTED:
            addSystemChatMessage("You are muted in channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::BANNED:
            addSystemChatMessage("You are banned from channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::THROTTLED:
            addSystemChatMessage("Channel '" + data.channelName + "' is throttled. Please wait.");
            break;
        case ChannelNotifyType::NOT_IN_LFG:
            addSystemChatMessage("You must be in a LFG queue to join '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_KICKED:
            addSystemChatMessage("A player was kicked from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PASSWORD_CHANGED:
            addSystemChatMessage("Password for '" + data.channelName + "' changed.");
            break;
        case ChannelNotifyType::OWNER_CHANGED:
            addSystemChatMessage("Owner of '" + data.channelName + "' changed.");
            break;
        case ChannelNotifyType::NOT_OWNER:
            addSystemChatMessage("You are not the owner of '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::INVALID_NAME:
            addSystemChatMessage("Invalid channel name '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_NOT_FOUND:
            addSystemChatMessage("Player not found.");
            break;
        case ChannelNotifyType::ANNOUNCEMENTS_ON:
            addSystemChatMessage("Channel '" + data.channelName + "': announcements enabled.");
            break;
        case ChannelNotifyType::ANNOUNCEMENTS_OFF:
            addSystemChatMessage("Channel '" + data.channelName + "': announcements disabled.");
            break;
        case ChannelNotifyType::MODERATION_ON:
            addSystemChatMessage("Channel '" + data.channelName + "' is now moderated.");
            break;
        case ChannelNotifyType::MODERATION_OFF:
            addSystemChatMessage("Channel '" + data.channelName + "' is no longer moderated.");
            break;
        case ChannelNotifyType::PLAYER_BANNED:
            addSystemChatMessage("A player was banned from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_UNBANNED:
            addSystemChatMessage("A player was unbanned from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_NOT_BANNED:
            addSystemChatMessage("That player is not banned from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::INVITE:
            addSystemChatMessage("You have been invited to join channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::INVITE_WRONG_FACTION:
        case ChannelNotifyType::WRONG_FACTION:
            addSystemChatMessage("Wrong faction for channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::NOT_MODERATED:
            addSystemChatMessage("Channel '" + data.channelName + "' is not moderated.");
            break;
        case ChannelNotifyType::PLAYER_INVITED:
            addSystemChatMessage("Player invited to channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_INVITE_BANNED:
            addSystemChatMessage("That player is banned from '" + data.channelName + "'.");
            break;
        default:
            LOG_DEBUG("Channel notify type ", static_cast<int>(data.notifyType),
                     " for channel ", data.channelName);
            break;
    }
}

void ChatHandler::autoJoinDefaultChannels() {
    LOG_INFO("autoJoinDefaultChannels: general=", chatAutoJoin.general,
             " trade=", chatAutoJoin.trade, " localDefense=", chatAutoJoin.localDefense,
             " lfg=", chatAutoJoin.lfg, " local=", chatAutoJoin.local);
    if (chatAutoJoin.general) joinChannel("General");
    if (chatAutoJoin.trade) joinChannel("Trade");
    if (chatAutoJoin.localDefense) joinChannel("LocalDefense");
    if (chatAutoJoin.lfg) joinChannel("LookingForGroup");
    if (chatAutoJoin.local) joinChannel("Local");
}

void ChatHandler::addLocalChatMessage(const MessageChatData& msg) {
    chatHistory_.push_back(msg);
    chatHistory_.back().uid = ++chatUidCounter_;
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.pop_front();
    }
    logChatMessage(msg, "local");
    if (owner_.addonChatCallbackRef()) owner_.addonChatCallbackRef()(msg);

    {
        const Character* ac = owner_.getActiveCharacter();
        std::string senderName = msg.senderName.empty()
            ? (ac ? ac->name : std::string{}) : msg.senderName;
        const uint64_t guid = msg.senderGuid != 0 ? msg.senderGuid : owner_.getPlayerGuid();
        fireChatMessageEvent(owner_, msg, senderName, guid);
    }
}

void ChatHandler::addSystemChatMessage(const std::string& message) {
    if (message.empty()) return;
    MessageChatData msg;
    msg.type = ChatType::SYSTEM;
    msg.language = ChatLanguage::UNIVERSAL;
    msg.message = message;
    addLocalChatMessage(msg);
}

void ChatHandler::toggleAfk(const std::string& message) {
    owner_.afkStatusRef() = !owner_.afkStatusRef();
    owner_.afkMessageRef() = message;

    if (owner_.afkStatusRef()) {
        if (message.empty()) {
            addSystemChatMessage("You are now AFK.");
        } else {
            addSystemChatMessage("You are now AFK: " + message);
        }
        // If DND was active, turn it off
        if (owner_.dndStatusRef()) {
            owner_.dndStatusRef() = false;
            owner_.dndMessageRef().clear();
        }
    } else {
        addSystemChatMessage("You are no longer AFK.");
        owner_.afkMessageRef().clear();
        afkAutoRepliedSenders_.clear();
    }

    LOG_INFO("AFK status: ", owner_.afkStatusRef(), ", message: ", message);
}

void ChatHandler::toggleDnd(const std::string& message) {
    owner_.dndStatusRef() = !owner_.dndStatusRef();
    owner_.dndMessageRef() = message;

    if (owner_.dndStatusRef()) {
        if (message.empty()) {
            addSystemChatMessage("You are now DND (Do Not Disturb).");
        } else {
            addSystemChatMessage("You are now DND: " + message);
        }
        // If AFK was active, turn it off
        if (owner_.afkStatusRef()) {
            owner_.afkStatusRef() = false;
            owner_.afkMessageRef().clear();
        }
    } else {
        addSystemChatMessage("You are no longer DND.");
        owner_.dndMessageRef().clear();
        afkAutoRepliedSenders_.clear();
    }

    LOG_INFO("DND status: ", owner_.dndStatusRef(), ", message: ", message);
}

void ChatHandler::replyToLastWhisper(const std::string& message) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot send whisper: not in world or not connected");
        return;
    }

    if (owner_.lastWhisperSenderRef().empty()) {
        addSystemChatMessage("No one has whispered you yet.");
        return;
    }

    if (message.empty()) {
        addSystemChatMessage("You must specify a message to send.");
        return;
    }

    // Send whisper using the standard message chat function
    sendChatMessage(ChatType::WHISPER, message, owner_.lastWhisperSenderRef());
    LOG_INFO("Replied to ", owner_.lastWhisperSenderRef(), ": ", message);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void ChatHandler::handleChannelList(network::Packet& packet) {
    std::string chanName = packet.readString();
    if (!packet.hasRemaining(5)) return;
    /*uint8_t chanFlags =*/ packet.readUInt8();
    uint32_t memberCount = packet.readUInt32();
    memberCount = std::min(memberCount, 200u);
    addSystemChatMessage(chanName + " has " + std::to_string(memberCount) + " member(s):");
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(9)) break;
        uint64_t memberGuid = packet.readUInt64();
        uint8_t memberFlags = packet.readUInt8();
        std::string name;
        auto entity = owner_.getEntityManager().getEntity(memberGuid);
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) name = player->getName();
        }
        if (name.empty()) name = owner_.lookupName(memberGuid);
        if (name.empty()) name = "(unknown)";
        std::string entry = "  " + name;
        if (memberFlags & 0x01) entry += " [Moderator]";
        if (memberFlags & 0x02) entry += " [Muted]";
        addSystemChatMessage(entry);
    }
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void ChatHandler::submitGmTicket(const std::string& text) {
    if (!owner_.isInWorld()) return;

    // CMSG_GMTICKET_CREATE (WotLK 3.3.5a):
    // string   ticket_text
    // float[3] position (server coords)
    // float    facing
    // uint32   mapId
    // uint8    need_response (1 = yes)
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_CREATE));
    pkt.writeString(text);
    pkt.writeFloat(owner_.movementInfoRef().x);
    pkt.writeFloat(owner_.movementInfoRef().y);
    pkt.writeFloat(owner_.movementInfoRef().z);
    pkt.writeFloat(owner_.movementInfoRef().orientation);
    pkt.writeUInt32(owner_.currentMapIdRef());
    pkt.writeUInt8(1);  // need_response = yes
    owner_.getSocket()->send(pkt);
    LOG_INFO("Submitted GM ticket: '", text, "'");
}

void ChatHandler::handleMotd(network::Packet& packet) {
    LOG_INFO("Handling SMSG_MOTD");

    MotdData data;
    if (!MotdParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_MOTD");
        return;
    }

    if (!data.isEmpty()) {
        LOG_INFO("========================================");
        LOG_INFO("   MESSAGE OF THE DAY");
        LOG_INFO("========================================");
        for (const auto& line : data.lines) {
            LOG_INFO(line);
            addSystemChatMessage(std::string("MOTD: ") + line);
        }
        // Add a visual separator after MOTD block so subsequent messages don't
        // appear glued to the last MOTD line.
        MessageChatData spacer;
        spacer.type = ChatType::SYSTEM;
        spacer.language = ChatLanguage::UNIVERSAL;
        spacer.message = "";
        addLocalChatMessage(spacer);
        LOG_INFO("========================================");
    }
}

void ChatHandler::handleNotification(network::Packet& packet) {
    // SMSG_NOTIFICATION: single null-terminated string
    std::string message = packet.readString();
    if (!message.empty()) {
        LOG_INFO("Server notification: ", message);
        addSystemChatMessage(message);
    }
}

} // namespace game
} // namespace wowee
