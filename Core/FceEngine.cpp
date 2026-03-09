#include "Core/FceEngine.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace fce
{

FceEngine::FceEngine() = default;
FceEngine::~FceEngine() = default;

// ─── Lifecycle (IFceObject) ───────────────────────────────────────────────────

void FceEngine::Initialize()
{
    std::lock_guard lock(m_mutex);
    m_baker.Initialize();
}

void FceEngine::Update(float /*dt*/)
{
    std::lock_guard lock(m_mutex);
    if (m_lastLocalFiles.empty()) return;

    m_store.Clear();
    m_pool.Clear();
    IndexLocalFilesNoLock(m_lastLocalFiles);
}

void FceEngine::Release()
{
    std::lock_guard lock(m_mutex);
    m_baker.Release();
}

// ─── Indexing API (→ BakeOrchestrator delegation) ─────────────────────────

int FceEngine::IndexLocalFiles(const std::vector<std::string>& filePaths)
{
    std::lock_guard lock(m_mutex);
    return IndexLocalFilesNoLock(filePaths);
}

// ─── Internal helper (no lock — caller guarantees m_mutex is held) ───────────

int FceEngine::IndexLocalFilesNoLock(const std::vector<std::string>& filePaths)
{
    m_lastLocalFiles = filePaths;

    static constexpr int32_t kMaxFiles = 200'000;
    std::vector<std::string> validFiles;
    validFiles.reserve(filePaths.size());

    for (const auto& path : filePaths)
    {
        if (static_cast<int32_t>(validFiles.size()) >= kMaxFiles) break;
        if (!BakeOrchestrator::DetectLang(path).empty())
            validFiles.push_back(path);
    }

    int result = m_baker.BakeLocalFiles(validFiles, m_store, m_pool);
    m_store.BuildSearchIndex(m_pool);
    return result;
}

int FceEngine::ReindexFiles(const std::vector<std::string>& filePaths)
{
    std::lock_guard lock(m_mutex);
    int result = m_baker.RebakeFiles(filePaths, m_store, m_pool);
    m_store.BuildSearchIndex(m_pool);
    return result;
}

void FceEngine::RemoveFiles(const std::vector<std::string>& filePaths)
{
    std::lock_guard lock(m_mutex);
    m_baker.RemoveFiles(filePaths, m_store, m_pool);
}

// ─── Preprocessing config (→ BakeOrchestrator delegation) ────────────────

void FceEngine::SetPreprocessConfig(const PreprocessConfig& config)
{
    std::lock_guard lock(m_mutex);
    m_baker.SetPreprocessConfig(config);
}

const BakeStats& FceEngine::GetBakeStats() const
{
    std::lock_guard lock(m_mutex);
    return m_baker.GetLastStats();
}

// ─── Query API (→ QuerySystem delegation) ─────────────────────────────────

QueryResult FceEngine::Query(const std::string& name,
                              int bfsDepth,
                              RelationMask filter,
                              bool reverse) const
{
    std::lock_guard lock(m_mutex);
    return m_query.Query(name, bfsDepth, filter, reverse, m_store, m_pool);
}

SmartQueryResult FceEngine::SmartQuery(const std::string& name,
                                       const DensityConfig& densityConfig) const
{
    std::lock_guard lock(m_mutex);
    return m_query.SmartQuery(name, densityConfig, m_store, m_pool);
}

std::vector<SymbolEntry> FceEngine::Lookup(const std::string& name) const
{
    std::lock_guard lock(m_mutex);
    return m_query.Lookup(name, m_store, m_pool);
}

std::size_t FceEngine::SymbolCount() const
{
    std::lock_guard lock(m_mutex);
    return m_store.SymbolCount();
}

SnippetResult FceEngine::PincerExtract(
    const std::string& filePath, int startLine, int endLine,
    const std::string& targetName, int radius)
{
    return QuerySystem::PincerExtract(filePath, startLine, endLine, targetName, radius);
}

// ─── Introspection (thin JSON conversion) ───────────────────────────────────

std::string FceEngine::GetFileTree() const
{
    std::lock_guard lock(m_mutex);
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    std::set<uint32_t> fileIds;
    const size_t symCount = m_store.SymbolCount();
    for (size_t i = 0; i < symCount; ++i)
    {
        const auto& sym = m_store.GetSymbol(static_cast<uint32_t>(i));
        if (sym.fileId != 0)
            fileIds.insert(sym.fileId);
    }

    json result = json::array();
    for (uint32_t fileId : fileIds)
    {
        std::string_view path = m_pool.Resolve(fileId);
        json fileObj;
        fileObj["path"] = std::string(path);
        fileObj["name"] = fs::path(std::string(path)).filename().string();
        result.push_back(std::move(fileObj));
    }
    return result.dump();
}

std::string FceEngine::GetFileSymbols(const std::string& path) const
{
    std::lock_guard lock(m_mutex);
    using json = nlohmann::json;
    json result = json::array();

    // exact match first
    uint32_t targetFileId = m_pool.Find(path);

    // partial match fallback: find first file whose path ends with the query
    if (targetFileId == 0)
    {
        const std::string suffix = [&path]() {
            // normalize separators to forward slash for matching
            std::string s = path;
            for (char& c : s) if (c == '\\') c = '/';
            return s;
        }();

        const size_t sc = m_store.SymbolCount();
        std::set<uint32_t> fileIds;
        for (size_t i = 0; i < sc; ++i)
            fileIds.insert(m_store.GetSymbol(static_cast<uint32_t>(i)).fileId);

        for (uint32_t fid : fileIds)
        {
            if (fid == 0) continue;
            std::string resolved(m_pool.Resolve(fid));
            for (char& c : resolved) if (c == '\\') c = '/';

            // ends-with or contains match
            if (resolved.length() >= suffix.length() &&
                resolved.find(suffix) != std::string::npos)
            {
                targetFileId = fid;
                break;
            }
        }
    }

    if (targetFileId == 0) return result.dump();

    const size_t symCount = m_store.SymbolCount();
    for (size_t i = 0; i < symCount; ++i)
    {
        const auto& sym = m_store.GetSymbol(static_cast<uint32_t>(i));
        if (sym.fileId != targetFileId) continue;

        json obj;
        obj["name"] = std::string(m_pool.Resolve(sym.nameId));
        obj["kind"] = QuerySystem::KindToString(sym.kind);
        obj["line"] = sym.startLine;
        result.push_back(std::move(obj));
    }
    return result.dump();
}

std::string FceEngine::SearchSymbols(const std::string& prefix, int limit) const
{
    std::lock_guard lock(m_mutex);
    using json = nlohmann::json;
    json result = json::array();

    auto toLower = [](const std::string& s) {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };

    const std::string low = toLower(prefix);

    // Phase 1: O(log N) prefix search via sorted index
    auto indices = m_store.SearchByPrefix(low, limit);

    // Phase 2: if prefix didn't find enough, fallback to substring O(N)
    if (static_cast<int>(indices.size()) < limit)
    {
        auto subIndices = m_store.SearchBySubstring(low, limit);
        // merge, avoiding duplicates
        std::set<uint32_t> seen(indices.begin(), indices.end());
        for (uint32_t idx : subIndices)
        {
            if (static_cast<int>(indices.size()) >= limit) break;
            if (seen.insert(idx).second)
                indices.push_back(idx);
        }
    }

    for (uint32_t symIdx : indices)
    {
        const auto& sym = m_store.GetSymbol(symIdx);
        json obj;
        obj["name"] = std::string(m_pool.Resolve(sym.nameId));
        obj["kind"] = QuerySystem::KindToString(sym.kind);
        obj["file"] = std::string(m_pool.Resolve(sym.fileId));
        obj["line"] = sym.startLine;
        result.push_back(std::move(obj));
    }
    return result.dump();
}

} // namespace fce
