#include "open_format_emitter.hpp"

// Android extract build: open-format sidecars are not required for play.
// Provide stubs so extractor.cpp links without pulling M2/WMO/ADT/BLP emitters.

namespace wowee {
namespace tools {

bool emitPngFromBlp(const std::string&, const std::string&) { return false; }
bool emitJsonFromDbc(const std::string&, const std::string&) { return false; }
bool emitWomFromM2(const std::string&, const std::string&) { return false; }
bool emitWobFromWmo(const std::string&, const std::string&) { return false; }
bool emitTerrainFromAdt(const std::string&, const std::string&) { return false; }

void emitOpenFormats(const std::string&,
                     bool, bool, bool, bool, bool,
                     OpenFormatStats&,
                     unsigned int,
                     bool) {}

} // namespace tools
} // namespace wowee
