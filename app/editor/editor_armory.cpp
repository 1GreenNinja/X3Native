// The Armory browser — parse / decode / filter. Pure logic (see editor_armory.h).
#include "editor_armory.h"
#include "../json_mini.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace jm = x3::game::jmini;

namespace x3::editor {

std::string armoryRoot() {
    // NEVER bake another machine's disk into the source (KNOWN_BUGS L2: a hardcoded
    // C:\...\OneDrive path silently overrode the repo's level for months). The env var
    // is the override; the default is where the Armory tool actually writes today.
    if (const char* e = std::getenv("X3_ARMORY_ROOT")) {
        if (e[0] != '\0') return std::string(e);
    }
    return "D:/Assets/_glb";
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        // NOTE: '+' is NOT a space here. This is a PATH, not a query string, and a
        // real pack is called "Sci-Fi Kit" — turning '+' into ' ' would corrupt any
        // filename that legitimately contains one.
        out.push_back(s[i]);
    }
    return out;
}

ArmoryIndex parseArmoryIndex(const std::string& json, const std::string& root) {
    ArmoryIndex idx;
    idx.root = root;

    jm::JReader rd(json);
    jm::JVal doc = rd.parse();
    if (!rd.ok || doc.t != jm::JVal::Obj) {
        idx.error = "galleries.json is missing or malformed";
        return idx;
    }
    const jm::JVal* packs = doc.get("packs");
    if (!packs || packs->t != jm::JVal::Arr) {
        idx.error = "galleries.json has no packs[] array";
        return idx;
    }

    for (const jm::JVal& p : packs->arr) {
        if (p.t != jm::JVal::Obj) continue;
        const std::string packName = p.sval("pack", "");
        const jm::JVal*   items    = p.get("items");
        if (!items || items->t != jm::JVal::Arr) continue;
        idx.packs.push_back(packName);
        for (const jm::JVal& it : items->arr) {
            if (it.t != jm::JVal::Obj) continue;
            const std::string glb = it.sval("glb", "");
            if (glb.empty()) continue;          // an item with no mesh cannot be placed
            ArmoryItem a;
            a.name    = it.sval("name", "");
            a.pack    = packName;
            a.relPath = urlDecode(glb);         // the index is percent-encoded (it is
                                                // served over HTTP); the filesystem is not
            idx.items.push_back(std::move(a));
        }
    }
    idx.ok = !idx.items.empty();
    if (!idx.ok && idx.error.empty()) idx.error = "the library index contains no meshes";
    return idx;
}

ArmoryIndex loadArmoryIndex() {
    const std::string root = armoryRoot();
    const std::string path = root + "/_host/galleries.json";
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        ArmoryIndex idx;
        idx.root  = root;
        idx.error = "no library index at " + path;
        return idx;   // NOT fatal: the curated Models list still works
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    ArmoryIndex idx = parseArmoryIndex(ss.str(), root);
    if (idx.ok)
        x3::logInfo("[armory] " + std::to_string(idx.items.size()) + " meshes across " +
                    std::to_string(idx.packs.size()) + " packs (" + root + ")");
    else
        x3::logWarn("[armory] " + idx.error);
    return idx;
}

namespace {
bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto lower = [](unsigned char c) { return (char)std::tolower(c); };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lower((unsigned char)hay[i + j]) ==
                                    lower((unsigned char)needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}
} // namespace

std::vector<uint32_t> filterArmory(const ArmoryIndex& idx, const std::string& query,
                                   const std::string& packFilter, size_t limit) {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < idx.items.size(); ++i) {
        const ArmoryItem& it = idx.items[i];
        if (!packFilter.empty() && it.pack != packFilter) continue;
        // Search NAME and PACK together: "sewer" should find the Modular Sewers pack
        // even though no individual mesh is called "sewer".
        if (!query.empty() && !icontains(it.name, query) && !icontains(it.pack, query))
            continue;
        out.push_back(i);
        if (out.size() >= limit) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
bool runArmorySelfTest() {
    int pass = 0, fail = 0;
    auto ck = [&](bool c, const char* what) {
        if (c) ++pass;
        else { ++fail; x3::logInfo(std::string("[armory-test]   FAIL: ") + what); }
    };

    // URL decoding — the thing that silently produces paths that do not exist.
    ck(urlDecode("Modular%20Castle") == "Modular Castle", "%20 decodes to a space");
    ck(urlDecode("HDRP%28Default%29") == "HDRP(Default)", "%28/%29 decode to parens");
    ck(urlDecode("a%2Fb") == "a/b", "%2F decodes to a slash");
    ck(urlDecode("Sci-Fi Kit") == "Sci-Fi Kit", "plain text is untouched");
    ck(urlDecode("a+b") == "a+b", "'+' is NOT a space (this is a PATH, not a query)");
    ck(urlDecode("100%") == "100%", "a trailing bare % does not overrun the buffer");
    ck(urlDecode("%zz") == "%zz", "a bad escape is left alone, not corrupted");

    // Parsing.
    const std::string js =
        "{\"pack_count\":2,\"packs\":["
        "{\"pack\":\"Command Center\",\"items\":["
        "  {\"name\":\"SM_Console\",\"glb\":\"tech/Command%20Center/SM_Console.glb\"},"
        "  {\"name\":\"SM_Desk\",\"glb\":\"tech/Command%20Center/SM_Desk.glb\"}]},"
        "{\"pack\":\"Modular Sewers\",\"items\":["
        "  {\"name\":\"SM_Pipe\",\"glb\":\"tech/Sewers/SM_Pipe.glb\"},"
        "  {\"name\":\"SM_NoMesh\"}]}"     // no glb -> must be dropped, not placed
        "]}";
    const ArmoryIndex idx = parseArmoryIndex(js, "D:/Assets/_glb");
    ck(idx.ok, "a well-formed index parses");
    ck(idx.items.size() == 3, "an item with NO glb path is dropped (it cannot be placed)");
    ck(idx.packs.size() == 2, "both packs are listed");
    ck(!idx.items.empty() && idx.items[0].relPath == "tech/Command Center/SM_Console.glb",
       "the item's path is DECODED (the filesystem wants a space, not %20)");
    ck(!idx.items.empty() && idx.items[0].pack == "Command Center",
       "the item carries its pack name");

    // A missing/garbage index must NOT be fatal — the curated list still works.
    const ArmoryIndex bad = parseArmoryIndex("not json at all", "R:/");
    ck(!bad.ok && !bad.error.empty(), "a garbage index fails SOFTLY, with a reason");

    // Filtering.
    ck(filterArmory(idx, "console", "", 64).size() == 1, "search by mesh NAME");
    ck(filterArmory(idx, "sewer", "", 64).size() == 1,
       "search by PACK name finds its meshes (no mesh is called 'sewer')");
    ck(filterArmory(idx, "", "Command Center", 64).size() == 2, "filter by pack");
    ck(filterArmory(idx, "SM_", "", 2).size() == 2, "the limit is respected");
    ck(filterArmory(idx, "CONSOLE", "", 64).size() == 1, "search is case-insensitive");
    ck(filterArmory(idx, "nothing_matches_this", "", 64).empty(), "no match -> empty");

    x3::logInfo("[armory-test] " + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::editor
