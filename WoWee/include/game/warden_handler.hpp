#pragma once

#include "game/opcode_table.hpp"
#include "game/warden_constants.hpp"
#include "network/packet.hpp"
#include <cstdint>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
class WardenCrypto;
class WardenMemory;
class WardenModule;
class WardenModuleManager;

class WardenHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit WardenHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API ---

    /** Reset all warden state (called on connect / disconnect). */
    void reset();

    /** Initialize warden module manager (called once from GameHandler ctor). */
    void initModuleManager();

    /** Whether the server requires Warden (gates char enum / create). */
    bool requiresWarden() const { return requiresWarden_; }
    void setRequiresWarden(bool v) { requiresWarden_ = v; }

    bool wardenGateSeen() const { return wardenGateSeen_; }

    /** Increment packet-after-gate counter (called from handlePacket). */
    void notifyPacketAfterGate() { ++wardenPacketsAfterGate_; }

    bool wardenCharEnumBlockedLogged() const { return wardenCharEnumBlockedLogged_; }
    void setWardenCharEnumBlockedLogged(bool v) { wardenCharEnumBlockedLogged_ = v; }

    /** Called from GameHandler::update() to drain async warden response + log gate timing. */
    void update(float deltaTime);

    /** Encrypt+send a completed async CHEAT_CHECKS_RESULT if ready (non-blocking). */
    void drainPendingResponse();

    /** Human-readable last Warden RX/TX for world-drop diagnostics. */
    std::string describeLastExchange() const;
    bool hasUnansweredCheatCheck() const { return unansweredCheatCheck_; }
    bool hasUnansweredHashRequest() const { return unansweredHashRequest_; }
    bool lastTxWasHashResult() const {
        return lastTxValid_ && lastTxOpcode_ == WARDEN_CMSG_HASH_RESULT;
    }
    bool lastRxWasModuleInit() const {
        return lastRxValid_ && lastRxOpcode_ == WARDEN_SMSG_MODULE_INITIALIZE;
    }
    size_t lastCheatResultBytes() const { return lastCheatResultBytes_; }

private:
    void handleWardenData(network::Packet& packet);
    bool loadWardenCRFile(const std::string& moduleHashHex);
    void mergePublishedCREntries(const std::string& moduleHashHex);
    void handleModuleInitialize(const std::vector<uint8_t>& decrypted);
    bool ensureWardenMemoryLoaded();

    GameHandler& owner_;

    // --- Warden state ---
    bool requiresWarden_ = false;
    bool wardenGateSeen_ = false;
    float wardenGateElapsed_ = 0.0f;
    float wardenGateNextStatusLog_ = 2.0f;
    uint32_t wardenPacketsAfterGate_ = 0;
    bool wardenCharEnumBlockedLogged_ = false;
    std::unique_ptr<WardenCrypto> wardenCrypto_;
    std::unique_ptr<WardenMemory> wardenMemory_;
    std::unique_ptr<WardenModuleManager> wardenModuleManager_;

    // Warden module download state
    enum class WardenState {
        WAIT_MODULE_USE,     // Waiting for first SMSG (MODULE_USE)
        WAIT_MODULE_CACHE,   // Sent MODULE_MISSING, receiving module chunks
        WAIT_HASH_REQUEST,   // Module received, waiting for HASH_REQUEST
        WAIT_CHECKS,         // Hash sent, waiting for check requests
    };
    WardenState wardenState_ = WardenState::WAIT_MODULE_USE;
    std::vector<uint8_t> wardenModuleHash_;    // 16 bytes MD5
    std::vector<uint8_t> wardenModuleKey_;     // 16 bytes RC4
    uint32_t wardenModuleSize_ = 0;
    std::vector<uint8_t> wardenModuleData_;    // Downloaded module chunks
    std::vector<uint8_t> wardenLoadedModuleImage_; // Parsed module image for key derivation
    std::shared_ptr<WardenModule> wardenLoadedModule_; // Loaded Warden module

    // Pre-computed challenge/response entries from .cr file
    struct WardenCREntry {
        uint8_t seed[16];
        uint8_t reply[20];
        uint8_t clientKey[16];  // Encrypt key (client→server)
        uint8_t serverKey[16]; // Decrypt key (server→client)
    };
    std::vector<WardenCREntry> wardenCREntries_;
    // Module-specific check type opcodes [9]: MEM, PAGE_A, PAGE_B, MPQ, LUA, DRIVER, TIMING, PROC, MODULE
    uint8_t wardenCheckOpcodes_[9] = {
        // Classic MaNGOS / 1.12 defaults used when no module .cr is cached yet.
        // Order: MEM, MODULE, PAGE_A, PAGE_B, MPQ, LUA, PROC, DRIVER, TIMING
        0xF3, 0xD9, 0xB2, 0xBF, 0x98, 0x8B, 0x7E, 0x71, 0x57
    };

    // Async Warden response: avoids main-loop stalls from PAGE_A/PAGE_B brute-force searches.
    // Future carries RAW resultData only — checksum + RC4 encrypt happen on the main thread
    // (OpenSSL SHA1/HMAC are not assumed thread-safe across the game loop).
    std::future<std::vector<uint8_t>> wardenPendingEncrypted_;
    bool wardenResponsePending_ = false;

    mutable std::recursive_mutex wardenIoMutex_;
    std::chrono::steady_clock::time_point lastRx_{};
    std::chrono::steady_clock::time_point lastTx_{};
    uint8_t lastRxOpcode_ = 0;
    uint8_t lastTxOpcode_ = 0;
    std::atomic<bool> unansweredCheatCheck_{false};
    std::atomic<bool> unansweredHashRequest_{false};
    size_t lastCheatResultBytes_ = 0;
    bool lastRxValid_ = false;
    bool lastTxValid_ = false;

    /** Shared GetTickCount-style ms clock for TIMING + LastHardwareAction pairing. */
    static uint32_t wardenTickMs();
    static std::vector<uint8_t> frameCheatChecksResult(const std::vector<uint8_t>& resultData);
};

} // namespace game
} // namespace wowee
