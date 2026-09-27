#include "addons/addon_manager.hpp"
#include "core/logger.hpp"
#include "core/config_paths.hpp"
#include <sstream>
#include "ui/xml_parser.hpp"
#include "ui/framexml_emitter.hpp"
#include <algorithm>
#include <set>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace wowee::addons {

AddonManager::AddonManager() = default;
AddonManager::~AddonManager() { shutdown(); }

bool AddonManager::initialize(game::GameHandler* gameHandler, const LuaServices& services) {
    gameHandler_ = gameHandler;
    luaServices_ = services;
    if (!luaEngine_.initialize()) return false;
    luaEngine_.setGameHandler(gameHandler);
    luaEngine_.setLuaServices(luaServices_);
    return true;
}

namespace {

/// Find a child of `base` whose name matches `name` ignoring case, or empty.
///
/// Extracted game data does not agree with itself about case: this install has
/// interface/framexml in lower case beside interface/AddOns in mixed. The asset
/// manager copes because it goes through a manifest of normalised paths, but
/// anything reaching the filesystem directly has to look, and on a
/// case-sensitive filesystem a hardcoded spelling simply misses.
std::filesystem::path resolveChild(const std::filesystem::path& base,
                                   const std::string& name) {
    std::error_code ec;
    const std::filesystem::path exact = base / name;
    if (std::filesystem::exists(exact, ec)) return exact;

    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    const std::string wanted = lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (lower(entry.path().filename().string()) == wanted) return entry.path();
    }
    return {};
}

/// Walk a relative path one component at a time, matching each without regard
/// to case.
std::filesystem::path resolvePath(const std::filesystem::path& base,
                                  const std::string& relative) {
    std::filesystem::path at = base;
    for (const auto& part : std::filesystem::path(relative)) {
        if (part.empty() || part == ".") continue;
        at = resolveChild(at, part.string());
        if (at.empty()) return {};
    }
    return at;
}

} // namespace

void AddonManager::scanAddons(const std::string& addonsPath) {
    addonsPath_ = addonsPath;
    addons_.clear();

    // Two places are searched. The game data's own Interface\AddOns is where a
    // player's existing addons already live, and an "addons" directory beside
    // the executable is where this client's own ship without anyone having to
    // copy files into an extracted game install to try them.
    std::vector<fs::path> roots;
    {
        // Same case problem as FrameXML: this install has interface/addons in
        // lower case, and a hardcoded AddOns misses it on a case-sensitive
        // filesystem.
        std::error_code ec;
        fs::path p(addonsPath);
        if (!fs::is_directory(p, ec)) {
            p = resolvePath(fs::path(addonsPath).parent_path().parent_path(),
                            "interface/AddOns");
        }
        if (!p.empty()) roots.emplace_back(p);
    }
    std::error_code rec;
    for (const char* local : {"addons", "../addons", "../../addons"}) {
        fs::path p = fs::absolute(local, rec);
        if (fs::is_directory(p, rec)) roots.push_back(fs::weakly_canonical(p, rec));
    }

    int scannedDirs = 0, loadOnDemand = 0, noToc = 0;
    std::vector<fs::path> dirs;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            LOG_INFO("AddonManager: no AddOns directory at ", root.string());
            continue;
        }
        // Said out loud, because which directory this lands on decides what
        // gets loaded and it is resolved differently on a case-insensitive
        // filesystem. A report of the interface appearing when nobody asked
        // for it is unanswerable without knowing where the scan looked.
        LOG_WARNING("AddonManager: scanning for addons in ", root.string());
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.is_directory()) dirs.push_back(entry.path());
        }
    }
    // Sort alphabetically for deterministic load order
    std::sort(dirs.begin(), dirs.end());

    // One addon per name, however many roots supply it. Searching more than one
    // place means the same addon can be found twice — a copy staged beside the
    // executable and the original it was staged from, say — and loading both
    // runs its Lua twice, which builds two of every frame. They sit exactly on
    // top of each other, so it reads as one frame that will not hide: the
    // toggle hides the copy it has a handle to and the other stays.
    std::set<std::string> seen;
    int duplicates = 0;

    for (const auto& dir : dirs) {
        ++scannedDirs;
        std::string dirName = dir.filename().string();
        // The original interface is not an addon and must never be loaded as
        // one. It ships with a .toc of its own, so a scan that lands on
        // Data/interface rather than Data/interface/AddOns — which is what a
        // case-insensitive filesystem can produce — would find FrameXML and
        // load the whole of it, with none of the opt-in that is supposed to
        // guard it. It has exactly one way in, and this is not it.
        {
            std::string lower = dirName;
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower == "framexml" || lower == "gluexml") {
                LOG_WARNING("AddonManager: refusing to load ", dirName,
                            " as an addon — the original interface loads only "
                            "through WOWEE_LOAD_FRAMEXML");
                continue;
            }
        }

        std::string tocPath = (dir / (dirName + ".toc")).string();
        auto toc = parseTocFile(tocPath);
        if (!toc) { ++noToc; continue; }

        if (toc->isLoadOnDemand()) {
            ++loadOnDemand;
            continue;
        }

        if (!seen.insert(toc->addonName).second) {
            ++duplicates;
            LOG_INFO("AddonManager: '", toc->addonName, "' already found elsewhere; "
                     "ignoring the copy at ", dir.string());
            continue;
        }

        LOG_INFO("AddonManager: registered addon '", toc->getTitle(),
                 "' (", toc->files.size(), " files) from ", dir.string());
        addons_.push_back(std::move(*toc));
    }

    // Say what happened even when nothing loads, which is the case that used to
    // be silent: every Blizzard addon in a stock Interface directory is
    // LoadOnDemand, so a scan can look at dozens of folders, register none of
    // them, and print one line that reads like an empty directory.
    LOG_INFO("AddonManager: scanned ", scannedDirs, " directories, registered ",
             addons_.size(), " addons (", loadOnDemand, " load-on-demand, ",
             noToc, " without a .toc, ", duplicates, " duplicate)");
    // Load persisted enable/disable choices now that we know which addons exist.
    loadEnabledState();
}

void AddonManager::loadAllAddons() {
    // The original interface, when asked for. Before any addon, because addons
    // are written against a world where FrameXML has already defined its frames
    // and its several thousand functions.
    //
    // Off by default and deliberately so: this is an experiment for now, it
    // wants the missing-API fallback on beside it to get anywhere, and a
    // half-loaded FrameXML on top of the client's own interface is not a state
    // anyone wants to be in by accident.
    const char* wantFrameXml = std::getenv("WOWEE_LOAD_FRAMEXML");
    if (wantFrameXml && *wantFrameXml && std::string(wantFrameXml) != "0" &&
        !frameXmlDir_.empty()) {
        loadFrameXml(frameXmlDir_);
    }

    // Only hand the Lua VM the addons that are actually enabled, so disabled ones
    // don't appear via GetNumAddOns/IsAddOnLoaded either.
    std::vector<TocFile> enabled;
    enabled.reserve(addons_.size());
    for (const auto& addon : addons_) {
        if (isAddonEnabled(addon.addonName)) enabled.push_back(addon);
    }
    luaEngine_.setAddonList(enabled);
    int loaded = 0, failed = 0, skipped = 0;
    for (const auto& addon : addons_) {
        if (!isAddonEnabled(addon.addonName)) {
            LOG_INFO("AddonManager: skipping disabled addon: ", addon.addonName);
            skipped++;
            continue;
        }
        if (loadAddon(addon)) loaded++;
        else failed++;
    }
    addonsLoaded_ = true;
    LOG_INFO("AddonManager: loaded ", loaded, " addons",
             (failed > 0 ? (", " + std::to_string(failed) + " failed") : ""),
             (skipped > 0 ? (", " + std::to_string(skipped) + " disabled") : ""));
}

// ---- Per-addon enable/disable (persisted) ----------------------------------

bool AddonManager::isAddonEnabled(const std::string& addonName) const {
    auto it = addonEnabled_.find(addonName);
    return (it == addonEnabled_.end()) ? true : it->second;  // default: enabled
}

void AddonManager::setAddonEnabled(const std::string& addonName, bool enabled) {
    addonEnabled_[addonName] = enabled;
    saveEnabledState();
}

std::string AddonManager::enabledStatePath() {
    return core::getConfigRoot() + "/addons.cfg";
}

void AddonManager::loadEnabledState() {
    std::ifstream in(enabledStatePath());
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!name.empty()) addonEnabled_[name] = (val == "1");
    }
}

void AddonManager::saveEnabledState() const {
    std::ofstream out(enabledStatePath(), std::ios::trunc);
    if (!out) {
        LOG_WARNING("AddonManager: could not write ", enabledStatePath());
        return;
    }
    // Persist an explicit line only for addons we actually know about, so stale
    // entries for removed addons don't accumulate.
    for (const auto& addon : addons_) {
        out << addon.addonName << "=" << (isAddonEnabled(addon.addonName) ? "1" : "0") << "\n";
    }
}

std::string AddonManager::getSavedVariablesPath(const TocFile& addon) const {
    return addon.basePath + "/" + addon.addonName + ".lua.saved";
}

std::string AddonManager::getSavedVariablesPerCharacterPath(const TocFile& addon) const {
    if (characterName_.empty()) return "";
    return addon.basePath + "/" + addon.addonName + "." + characterName_ + ".lua.saved";
}

bool AddonManager::loadFrameXml(const std::string& frameXmlDir) {
    std::error_code ec;
    std::filesystem::path dir(frameXmlDir);
    if (!std::filesystem::is_directory(dir, ec)) {
        // The directory itself may be spelled differently on disk.
        dir = resolvePath(std::filesystem::path(frameXmlDir).parent_path(),
                          std::filesystem::path(frameXmlDir).filename().string());
    }
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
        LOG_WARNING("FrameXML: no directory at ", frameXmlDir);
        return false;
    }
    const std::filesystem::path tocPath = resolveChild(dir, "FrameXML.toc");
    auto toc = tocPath.empty() ? std::nullopt : parseTocFile(tocPath.string());
    if (!toc) {
        LOG_WARNING("FrameXML: no manifest in ", dir.string());
        return false;
    }
    const std::string resolvedDir = dir.string();

    LOG_WARNING("FrameXML: attempting to load the original interface — ",
                toc->files.size(), " files from ", resolvedDir);

    int lua = 0, xml = 0, failed = 0;
    // Kept and printed together at the end. Spread through the log these are
    // unreadable: the reasons land among thousands of other lines, and one
    // broken script takes down every file that references it, so what matters
    // is seeing them side by side and spotting the cause they share.
    std::vector<std::pair<std::string, std::string>> failures;
    // Timed per file. This load runs on the main thread during world entry, so
    // whatever it costs the client is frozen for — long enough and the server
    // drops the connection for want of a heartbeat. Knowing it is slow is not
    // useful; knowing which file is.
    const auto loadStart = std::chrono::steady_clock::now();
    // Generous: all 139 files together used to run in 216ms, so a single one
    // reaching this has stopped making progress. Aborting it costs that file
    // and keeps the client answering, which beats freezing until it is killed.
    luaEngine_.setChunkTimeoutMs(5000);
    struct BudgetReset {
        LuaEngine& e;
        ~BudgetReset() { e.setChunkTimeoutMs(0); }
    } budgetReset{luaEngine_};
    auto sinceMs = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - from).count();
    };
    for (const auto& filename : toc->files) {
        const auto fileStart = std::chrono::steady_clock::now();
        // Named before it is loaded, not after. Timing it afterwards says
        // nothing about the one case that matters — a file that never returns
        // prints nothing at all, and the load simply stops with the last
        // successful file as the only clue.
        // At warning level because release builds drop INFO, and this is the
        // one line that identifies a file which never returns. Noisy for 139
        // files, and worth it only while this path is still experimental.
        LOG_WARNING("FrameXML: loading ", filename);
        std::string lower = filename;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::filesystem::path resolved = resolvePath(dir, filename);
        if (resolved.empty()) {
            LOG_WARNING("FrameXML: ", filename, " is listed but not on disk");
            ++failed;
            failures.emplace_back(filename, "listed in the manifest but not on disk");
            continue;
        }
        const std::string full = resolved.string();

        // The manifest's order is the load order and matters: GlobalStrings and
        // Constants before anything reads them, Fonts before the frames that
        // inherit from them. Following it is most of what makes this possible
        // at all.
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lua") == 0) {
            if (luaEngine_.executeFile(full)) {
                ++lua;
            } else {
                ++failed;
                failures.emplace_back(filename, luaEngine_.lastError());
            }
        } else if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".xml") == 0) {
            lastXmlError_.clear();
            if (loadXmlFile(full, 0)) {
                ++xml;
            } else {
                ++failed;
                failures.emplace_back(filename, lastXmlError_.empty()
                                                    ? "(no reason recorded)"
                                                    : lastXmlError_);
            }
        }
        // Reported as it happens rather than only in the summary, because a
        // load that never reaches the summary is exactly the case worth
        // diagnosing.
        if (const auto ms = sinceMs(fileStart); ms >= 250) {
            LOG_WARNING("FrameXML: ", filename, " took ", ms, "ms");
        }
    }
    // Screen insets the panel manager reads straight off UIParent. The real
    // client supplies these; FrameXML only ever reads them, and
    // UIParentManageFramePositions adds them to a coordinate on the next line,
    // so absent they are arithmetic on nil the first time a panel opens.
    //
    // All six of them, because setting one wakes the panel manager and it then
    // reads the rest: seeding only the two offsets moved the failure from
    // LEFT_OFFSET to DEFAULT_FRAME_WIDTH one call deeper. The widths are
    // Blizzard's own defaults for a standard panel.
    luaEngine_.executeString(
        "if UIParent and UIParent.SetAttribute then\n"
        "  local defaults = {\n"
        "    TOP_OFFSET = 0, LEFT_OFFSET = 0, CENTER_OFFSET = 0,\n"
        "    RIGHT_OFFSET = 0, RIGHT_OFFSET_BUFFER = 0,\n"
        "    DEFAULT_FRAME_WIDTH = 338,\n"
        "  }\n"
        // Written straight into the attribute table rather than through
        // SetAttribute, which fires OnAttributeChanged: the panel manager runs
        // on the first one and reads the rest before the loop has set them, so
        // seeding through the setter failed on whichever name pairs() happened
        // to leave for last. These are initial values, not changes.
        "  local store = rawget(UIParent, '__attributes')\n"
        "  if not store then store = {}; rawset(UIParent, '__attributes', store) end\n"
        "  for name, value in pairs(defaults) do\n"
        "    if store[name] == nil then store[name] = value end\n"
        "  end\n"
        "end\n");

    LOG_WARNING("FrameXML: ", lua, " Lua files and ", xml, " XML files loaded, ",
                failed, " failed in ", sinceMs(loadStart), "ms");
    for (const auto& [file, why] : failures) {
        LOG_WARNING("FrameXML:   ", file, " — ", why);
    }
    return failed == 0;
}

bool AddonManager::loadXmlFile(const std::string& path, int depth) {
    // Includes nest, and a file that includes itself would otherwise recurse
    // until the stack gives out.
    constexpr int kMaxDepth = 16;
    if (depth > kMaxDepth) {
        lastXmlError_ = "include nesting too deep";
        LOG_ERROR("AddonManager: include nesting too deep at ", path);
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        lastXmlError_ = "not on disk";
        LOG_WARNING("AddonManager: XML not found: ", path);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    ui::XmlNode root;
    std::string error;
    if (!ui::parseXml(buffer.str(), root, error)) {
        lastXmlError_ = "XML parse: " + error;
        LOG_ERROR("AddonManager: ", path, ": ", error);
        return false;
    }

    ui::EmitResult emitted = ui::emitFrameXml(root);
    for (const auto& w : emitted.warnings) {
        LOG_WARNING("AddonManager: ", path, ": ", w);
    }

    const fs::path dir = fs::path(path).parent_path();
    bool ok = true;

    // Resolved without regard to case, the same as the manifest's own files. A
    // Script element says MovieFrame.lua and the file on disk is
    // movieframe.lua, so joining the two naively fails — which took out most of
    // FrameXML on the first attempt, one referenced script at a time.
    auto sibling = [&](const std::string& name) {
        const fs::path p = resolvePath(dir, name);
        return p.empty() ? (dir / name) : p;
    };

    // Order matters and is not the order the emitter reports things in. Includes
    // carry the templates a file inherits from, and scripts define the functions
    // its handlers name, so both have to be in place before any frame is built.
    // A file is only as loadable as what it pulls in, so the reason kept here is
    // the first real one — the include or script that actually broke — rather
    // than the name of whichever file happened to reference it.
    for (const auto& inc : emitted.includeFiles) {
        if (!loadXmlFile(sibling(inc).string(), depth + 1)) {
            if (ok) lastXmlError_ = "include " + inc + ": " + lastXmlError_;
            ok = false;
        }
    }
    for (const auto& script : emitted.scriptFiles) {
        if (!luaEngine_.executeFile(sibling(script).string())) {
            if (ok) lastXmlError_ = "script " + script + ": " + luaEngine_.lastError();
            LOG_ERROR("AddonManager: ", path, " referenced ", script, " which failed");
            ok = false;
        }
    }
    if (!emitted.lua.empty()) {
        if (!luaEngine_.executeString(emitted.lua)) {
            if (ok) lastXmlError_ = "frames: " + luaEngine_.lastError();
            LOG_ERROR("AddonManager: frames from ", path, " failed to build");
            ok = false;
        } else {
            LOG_INFO("AddonManager: built frames from ", path);
        }
    }
    return ok;
}

bool AddonManager::loadAddon(const TocFile& addon) {
    // Load SavedVariables before addon code (so globals are available at load time)
    auto savedVars = addon.getSavedVariables();
    if (!savedVars.empty()) {
        std::string svPath = getSavedVariablesPath(addon);
        luaEngine_.loadSavedVariables(svPath);
        LOG_DEBUG("AddonManager: loaded saved variables for '", addon.addonName, "'");
    }
    // Load per-character SavedVariables
    auto savedVarsPC = addon.getSavedVariablesPerCharacter();
    if (!savedVarsPC.empty()) {
        std::string svpcPath = getSavedVariablesPerCharacterPath(addon);
        if (!svpcPath.empty()) {
            luaEngine_.loadSavedVariables(svpcPath);
            LOG_DEBUG("AddonManager: loaded per-character saved variables for '", addon.addonName, "'");
        }
    }

    bool success = true;
    for (const auto& filename : addon.files) {
        std::string lower = filename;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".lua") {
            std::string fullPath = addon.basePath + "/" + filename;
            if (!luaEngine_.executeFile(fullPath)) {
                LOG_ERROR("AddonManager: '", addon.addonName, "' failed on ", filename);
                success = false;
            } else {
                LOG_INFO("AddonManager: ran ", addon.addonName, "/", filename);
            }
        } else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".xml") {
            if (!loadXmlFile(addon.basePath + "/" + filename, 0)) success = false;
        }
    }

    // Fire ADDON_LOADED event after all addon files are executed
    // This is the standard WoW pattern for addon initialization
    if (success) {
        luaEngine_.fireEvent("ADDON_LOADED", {addon.addonName});
    }
    return success;
}

bool AddonManager::runScript(const std::string& code) {
    return luaEngine_.executeString(code);
}

void AddonManager::fireEvent(const std::string& event, const std::vector<std::string>& args) {
    // 1.12 BAG_UPDATE's first argument is the bag id, and the container frame
    // ignores the event unless it matches. Call sites that do not know which
    // bag changed used to send nothing, so an open bag never redrew.
    if (event == "BAG_UPDATE" && args.empty()) {
        for (int bag = 0; bag <= 4; ++bag)
            luaEngine_.fireEvent(event, {std::to_string(bag)});
        return;
    }
    luaEngine_.fireEvent(event, args);
}

void AddonManager::update(float deltaTime) {
    luaEngine_.dispatchOnUpdate(deltaTime);
}

void AddonManager::saveAllSavedVariables() {
    for (const auto& addon : addons_) {
        auto savedVars = addon.getSavedVariables();
        if (!savedVars.empty()) {
            std::string svPath = getSavedVariablesPath(addon);
            luaEngine_.saveSavedVariables(svPath, savedVars);
        }
        auto savedVarsPC = addon.getSavedVariablesPerCharacter();
        if (!savedVarsPC.empty()) {
            std::string svpcPath = getSavedVariablesPerCharacterPath(addon);
            if (!svpcPath.empty()) {
                luaEngine_.saveSavedVariables(svpcPath, savedVarsPC);
            }
        }
    }
}

bool AddonManager::reload() {
    LOG_INFO("AddonManager: reloading all addons...");
    saveAllSavedVariables();
    addons_.clear();
    luaEngine_.shutdown();

    if (!luaEngine_.initialize()) {
        LOG_ERROR("AddonManager: failed to reinitialize Lua VM during reload");
        return false;
    }
    luaEngine_.setGameHandler(gameHandler_);
    luaEngine_.setLuaServices(luaServices_);

    if (!addonsPath_.empty()) {
        scanAddons(addonsPath_);
        loadAllAddons();
    }
    LOG_INFO("AddonManager: reload complete");
    return true;
}

void AddonManager::shutdown() {
    saveAllSavedVariables();
    addons_.clear();
    luaEngine_.shutdown();
}

} // namespace wowee::addons
