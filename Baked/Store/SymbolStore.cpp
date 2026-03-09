#include "Baked/Store/SymbolStore.hpp"
#include "Baked/StringPool.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>

namespace fce
{

const std::vector<uint32_t> SymbolStore::s_empty{};

// ── Loading ────────────────────────────────────────────────────────────────

uint32_t SymbolStore::AddSymbol(const CSymbol& sym)
{
    const auto idx = static_cast<uint32_t>(m_symbols.size());
    m_symbols.push_back(sym);
    return idx;
}

uint32_t SymbolStore::AddEdge(const BakedEdge& edge)
{
    const auto idx = static_cast<uint32_t>(m_edges.size());
    m_edges.push_back(edge);
    return idx;
}

void SymbolStore::RebuildIndices()
{
    m_nameIndex.clear();
    m_fromIndex.clear();
    m_toIndex.clear();

    // Symbol index: nameId -> [symIdx, ...]
    for (uint32_t i = 0, n = static_cast<uint32_t>(m_symbols.size()); i < n; ++i)
        m_nameIndex[m_symbols[i].nameId].push_back(i);

    // Edge index: fromNameId → [edgeIdx, ...], toNameId → [edgeIdx, ...]
    for (uint32_t i = 0, n = static_cast<uint32_t>(m_edges.size()); i < n; ++i)
    {
        m_fromIndex[m_edges[i].fromNameId].push_back(i);
        m_toIndex[m_edges[i].toNameId].push_back(i);
    }
}

void SymbolStore::Clear()
{
    m_symbols.clear();
    m_edges.clear();
    m_nameIndex.clear();
    m_fromIndex.clear();
    m_toIndex.clear();
    m_sortedNames.clear();
}

void SymbolStore::Reserve(size_t symbolCount, size_t edgeCount)
{
    m_symbols.reserve(symbolCount);
    m_edges.reserve(edgeCount);
}

// ── Query ────────────────────────────────────────────────────────────────

const std::vector<uint32_t>& SymbolStore::LookupByNameId(uint32_t nameId) const
{
    auto it = m_nameIndex.find(nameId);
    return (it != m_nameIndex.end()) ? it->second : s_empty;
}

const std::vector<uint32_t>& SymbolStore::EdgesFrom(uint32_t fromNameId) const
{
    auto it = m_fromIndex.find(fromNameId);
    return (it != m_fromIndex.end()) ? it->second : s_empty;
}

const std::vector<uint32_t>& SymbolStore::EdgesTo(uint32_t toNameId) const
{
    auto it = m_toIndex.find(toNameId);
    return (it != m_toIndex.end()) ? it->second : s_empty;
}

const CSymbol& SymbolStore::GetSymbol(uint32_t index) const
{
    assert(index < m_symbols.size() && "SymbolStore::GetSymbol — invalid index");
    return m_symbols[index];
}

const BakedEdge& SymbolStore::GetEdge(uint32_t index) const
{
    assert(index < m_edges.size() && "SymbolStore::GetEdge — invalid index");
    return m_edges[index];
}

// ── Sorted index (prefix search) ──────────────────────────────────────────────

static std::string ToLower(std::string_view sv)
{
    std::string out(sv.size(), '\0');
    for (size_t i = 0; i < sv.size(); ++i)
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(sv[i])));
    return out;
}

void SymbolStore::BuildSearchIndex(const StringPool& pool)
{
    // nameId → first symIdx (deduplicate by name)
    // m_nameIndex already has this: nameId → [symIdx...]
    m_sortedNames.clear();
    m_sortedNames.reserve(m_nameIndex.size());

    for (const auto& [nameId, indices] : m_nameIndex)
    {
        if (nameId == 0 || indices.empty()) continue;
        std::string_view name = pool.Resolve(nameId);
        if (name.empty()) continue;

        m_sortedNames.push_back({ToLower(name), nameId, indices[0]});
    }

    std::sort(m_sortedNames.begin(), m_sortedNames.end(),
        [](const SortedNameEntry& a, const SortedNameEntry& b) {
            return a.lowerName < b.lowerName;
        });
}

std::vector<uint32_t> SymbolStore::SearchByPrefix(const std::string& lowerPrefix, int limit) const
{
    std::vector<uint32_t> results;
    if (m_sortedNames.empty() || lowerPrefix.empty()) return results;

    // binary search for first entry >= prefix
    auto it = std::lower_bound(m_sortedNames.begin(), m_sortedNames.end(), lowerPrefix,
        [](const SortedNameEntry& entry, const std::string& prefix) {
            return entry.lowerName < prefix;
        });

    // collect all entries that start with prefix
    while (it != m_sortedNames.end() &&
           static_cast<int>(results.size()) < limit &&
           it->lowerName.compare(0, lowerPrefix.size(), lowerPrefix) == 0)
    {
        results.push_back(it->symIdx);
        ++it;
    }

    return results;
}

std::vector<uint32_t> SymbolStore::SearchBySubstring(const std::string& lowerQuery, int limit) const
{
    std::vector<uint32_t> results;
    if (m_sortedNames.empty() || lowerQuery.empty()) return results;

    for (const auto& entry : m_sortedNames)
    {
        if (static_cast<int>(results.size()) >= limit) break;
        if (entry.lowerName.find(lowerQuery) != std::string::npos)
            results.push_back(entry.symIdx);
    }

    return results;
}

// ── Incremental update ────────────────────────────────────────────────────────────

void SymbolStore::RemoveFile(uint32_t fileId)
{
    // erase-remove pattern: remove symbols/edges matching fileId
    m_symbols.erase(
        std::remove_if(m_symbols.begin(), m_symbols.end(),
            [fileId](const CSymbol& s) { return s.fileId == fileId; }),
        m_symbols.end());

    m_edges.erase(
        std::remove_if(m_edges.begin(), m_edges.end(),
            [fileId](const BakedEdge& e) { return e.fileId == fileId; }),
        m_edges.end());

    // Indices invalidated — rebuild
    RebuildIndices();
}

} // namespace fce
