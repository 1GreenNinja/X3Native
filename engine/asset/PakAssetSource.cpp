// Pak/VFS implementation of IAssetSource — D5 (clean-room).
// Spec: specs/D5-asset-source.spec.md
//
// Mounts .x3pak (zip via miniz) + loose dev dirs with priority override.
// Virtual-path index per pak, zip-slip rejection, byte-blob reads.
// Verified via runAssetSelfTest() (the 7 acceptance tests).

#include "IAssetSource.h"
#include "../core/x3_log.h"

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace x3::asset {

namespace {

// Normalize a virtual path: forward slashes, lowercase (Windows-insensitive),
// no leading slash. Returns empty on a path that escapes the root (zip-slip).
std::string normalize(std::string_view in) {
    std::string s(in);
    for (char& c : s) { if (c == '\\') c = '/'; c = static_cast<char>(::tolower((unsigned char)c)); }
    // strip leading slashes
    size_t b = s.find_first_not_of('/');
    if (b == std::string::npos) return {};
    s = s.substr(b);
    // reject traversal
    if (s.find("..") != std::string::npos) return {};
    return s;
}

struct Mount {
    int priority = 0;
    bool isDir = false;
    std::string root;                         // dir path (for isDir)
    std::string pakPath;                      // pak path (for !isDir)
    std::map<std::string, int> index;         // normalized vpath -> zip file index
    std::shared_ptr<mz_zip_archive> zip;      // open archive (for !isDir)
};

class PakAssetSource final : public IAssetSource {
public:
    ~PakAssetSource() override { for (auto& m : m_mounts) closeZip(m); }

    bool mountPak(std::string_view pakPath, int priority) override {
        Mount m;
        m.priority = priority; m.isDir = false; m.pakPath = std::string(pakPath);
        m.zip = std::shared_ptr<mz_zip_archive>(new mz_zip_archive(), [](mz_zip_archive* z){ delete z; });
        std::memset(m.zip.get(), 0, sizeof(mz_zip_archive));
        if (!mz_zip_reader_init_file(m.zip.get(), m.pakPath.c_str(), 0)) {
            logWarn(std::string("[asset] mountPak failed (not a zip / missing): ") + m.pakPath);
            return false;
        }
        mz_uint n = mz_zip_reader_get_num_files(m.zip.get());
        for (mz_uint i = 0; i < n; ++i) {
            if (mz_zip_reader_is_file_a_directory(m.zip.get(), i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(m.zip.get(), i, &st)) continue;
            std::string vp = normalize(st.m_filename);
            if (vp.empty()) { logWarn(std::string("[asset] rejected pak entry (zip-slip): ") + st.m_filename); continue; }
            m.index[vp] = static_cast<int>(i);
        }
        insertSorted(std::move(m));
        return true;
    }

    bool mountDir(std::string_view dirPath, int priority) override {
        if (!fs::exists(dirPath)) { logWarn(std::string("[asset] mountDir missing: ") + std::string(dirPath)); return false; }
        Mount m; m.priority = priority; m.isDir = true; m.root = std::string(dirPath);
        insertSorted(std::move(m));
        return true;
    }

    Blob read(std::string_view virtualPath) override {
        std::string vp = normalize(virtualPath);
        if (vp.empty()) return {};
        for (auto& m : m_mounts) { // already sorted highest-priority first
            if (m.isDir) {
                fs::path p = fs::path(m.root) / vp;
                std::error_code ec;
                if (fs::exists(p, ec) && !fs::is_directory(p, ec)) {
                    std::ifstream f(p, std::ios::binary);
                    if (f) { Blob b; b.bytes.assign(std::istreambuf_iterator<char>(f), {}); b.ok = true; return b; }
                }
            } else {
                auto it = m.index.find(vp);
                if (it != m.index.end()) {
                    size_t sz = 0;
                    void* data = mz_zip_reader_extract_to_heap(m.zip.get(), (mz_uint)it->second, &sz, 0);
                    if (data) {
                        Blob b; b.bytes.assign((uint8_t*)data, (uint8_t*)data + sz); b.ok = true;
                        mz_free(data); return b;
                    }
                    logWarn(std::string("[asset] extract failed: ") + vp + " in " + m.pakPath);
                }
            }
        }
        return {};
    }

    bool exists(std::string_view virtualPath) const override {
        std::string vp = normalize(virtualPath);
        if (vp.empty()) return false;
        for (auto& m : m_mounts) {
            if (m.isDir) { std::error_code ec; if (fs::exists(fs::path(m.root)/vp, ec)) return true; }
            else if (m.index.count(vp)) return true;
        }
        return false;
    }

    std::vector<std::string> list(std::string_view prefix) const override {
        std::string pre = normalize(prefix);
        std::vector<std::string> out; std::map<std::string,bool> seen;
        for (auto& m : m_mounts) {
            if (m.isDir) {
                fs::path base = fs::path(m.root) / pre;
                std::error_code ec;
                if (fs::exists(base, ec)) {
                    for (auto& e : fs::recursive_directory_iterator(base, ec)) {
                        if (e.is_directory()) continue;
                        std::string rel = normalize(fs::relative(e.path(), m.root, ec).string());
                        if (!rel.empty() && !seen[rel]) { seen[rel]=true; out.push_back(rel); }
                    }
                }
            } else {
                for (auto& kv : m.index) {
                    if (pre.empty() || kv.first.rfind(pre, 0) == 0) {
                        if (!seen[kv.first]) { seen[kv.first]=true; out.push_back(kv.first); }
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void refresh() override { /* dir mounts are read live; pak index is static */ }

private:
    void insertSorted(Mount&& m) {
        m_mounts.push_back(std::move(m));
        std::stable_sort(m_mounts.begin(), m_mounts.end(),
                         [](const Mount& a, const Mount& b){ return a.priority > b.priority; });
    }
    static void closeZip(Mount& m) { if (!m.isDir && m.zip) mz_zip_reader_end(m.zip.get()); }

    std::vector<Mount> m_mounts;
};

// --- test helpers ---
bool writeTestPak(const std::string& path, const std::vector<std::pair<std::string,std::string>>& entries) {
    mz_zip_archive zip; std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.c_str(), 0)) return false;
    for (auto& e : entries)
        mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION);
    bool ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}
std::string blobStr(const Blob& b){ return std::string((const char*)b.bytes.data(), b.bytes.size()); }

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; logInfo(std::string("[asset-test] PASS ") + name); }
    else      { ++g_fail; logError(std::string("[asset-test] FAIL ") + name); }
}

} // namespace

IAssetSource* createAssetSource() { return new PakAssetSource(); }

bool runAssetSelfTest() {
    g_pass = g_fail = 0;
    fs::path tmp = fs::temp_directory_path() / "x3native_assettest";
    std::error_code ec; fs::remove_all(tmp, ec); fs::create_directories(tmp, ec);
    std::string base = (tmp / "base.x3pak").string();
    std::string game = (tmp / "game.x3pak").string();

    writeTestPak(base, { {"hello.txt","hi"}, {"a.txt","base"}, {"materials/floor.ktx2","BASEFLOOR"} });
    writeTestPak(game, { {"a.txt","game"}, {"materials/wall.ktx2","GAMEWALL"} });

    // T1 single pak read
    { std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(base,0);
      check(s->read("hello.txt").ok && blobStr(s->read("hello.txt"))=="hi", "T1 single-pak read"); }

    // T2 priority override
    { std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(base,0); s->mountPak(game,10);
      check(blobStr(s->read("a.txt"))=="game", "T2 priority override"); }

    // T3 loose-dir dev override + refresh fallback
    { std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(base,0); s->mountPak(game,10);
      fs::path d = tmp/"loose"; fs::create_directories(d, ec);
      { std::ofstream(d/"a.txt") << "loose"; }
      s->mountDir(d.string(),100);
      bool over = blobStr(s->read("a.txt"))=="loose";
      fs::remove(d/"a.txt", ec); s->refresh();
      bool fell = blobStr(s->read("a.txt"))=="game";
      check(over && fell, "T3 loose-dir override + fallback"); }

    // T4 missing path
    { std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(base,0);
      check(!s->read("nope.txt").ok && !s->exists("nope.txt"), "T4 missing path"); }

    // T5 listing across mounts, deduped
    { std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(base,0); s->mountPak(game,10);
      auto l = s->list("materials/");
      bool hasFloor = std::find(l.begin(),l.end(),"materials/floor.ktx2")!=l.end();
      bool hasWall  = std::find(l.begin(),l.end(),"materials/wall.ktx2")!=l.end();
      check(hasFloor && hasWall && l.size()==2, "T5 list across mounts"); }

    // T6 zip-slip rejected
    { std::string evil = (tmp/"evil.x3pak").string();
      writeTestPak(evil, { {"../evil.txt","pwned"}, {"ok.txt","fine"} });
      std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(evil,0);
      check(!s->exists("../evil.txt") && !s->exists("evil.txt") && s->read("ok.txt").ok, "T6 zip-slip rejected"); }

    // T7 case-insensitive virtual paths (Windows)
    { std::unique_ptr<IAssetSource> s(createAssetSource()); s->mountPak(base,0);
      check(blobStr(s->read("MATERIALS/Floor.KTX2"))=="BASEFLOOR", "T7 case-insensitive path"); }

    fs::remove_all(tmp, ec);
    logInfo(std::string("[asset-test] ") + std::to_string(g_pass) + " passed, " + std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::asset
