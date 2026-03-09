#pragma once

#include "Baked/Component/Components.hpp"
#include "Baked/Data/BakedEdge.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fce
{

/**
 * Flat SOA symbol/edge store — replaces EnTT registry.
 *
 * Optimized for static data (read-only after parsing):
 *   - No entity ID/version — array index is the identifier
 *   - 0 sparse set / signals overhead
 *   - uint32_t index -> 4B limit (resolves EnTT 20-bit = 1M limit)
 *
 * Triple hash index:
 *   m_nameIndex[nameId]       -> symbol index list for the given name
 *   m_fromIndex[fromNameId]   -> forward edge indices (BFS)
 *   m_toIndex[toNameId]       -> reverse edge indices (CalledBy)
 */
class SymbolStore
{
public:
    // ── Loading (parse Flush phase) ──────────────────────────────────────────

    /** Add symbol, return array index */
    uint32_t AddSymbol(const CSymbol& sym);

    /** Add edge, return array index */
    uint32_t AddEdge(const BakedEdge& edge);

    /** Build triple index after loading is complete (name, from, to) */
    void RebuildIndices();

    /** Build sorted name index for prefix search (call after RebuildIndices) */
    void BuildSearchIndex(const class StringPool& pool);

    /** Full reset (call before re-indexing) */
    void Clear();

    /** Pre-allocate for expected size (minimizes realloc) */
    void Reserve(size_t symbolCount, size_t edgeCount);

    // ── Query (read-only) ────────────────────────────────────────────────

    /** Lookup symbol indices by nameId O(1) */
    const std::vector<uint32_t>& LookupByNameId(uint32_t nameId) const;

    /** Forward edge indices by fromNameId (for BFS) */
    const std::vector<uint32_t>& EdgesFrom(uint32_t fromNameId) const;

    /** Reverse edge indices by toNameId (for CalledBy) */
    const std::vector<uint32_t>& EdgesTo(uint32_t toNameId) const;

    /** Index -> symbol reference O(1) */
    const CSymbol& GetSymbol(uint32_t index) const;

    /** Index -> edge reference O(1) */
    const BakedEdge& GetEdge(uint32_t index) const;

    inline size_t SymbolCount() const { return m_symbols.size(); }
    inline size_t EdgeCount()   const { return m_edges.size(); }

    // ── Incremental Update ──────────────────────────────────────────────────────

    /** Remove symbols/edges for a specific file + rebuild indices */
    void RemoveFile(uint32_t fileId);

    // ── Sorted index prefix search O(log N + k) ───────────────────────

    /** Sorted (lowercase_name, symIdx) array. Built during RebuildIndices */
    struct SortedNameEntry
    {
        std::string lowerName;   // Lowercase-converted name
        uint32_t    nameId;      // Original name ID (StringPool)
        uint32_t    symIdx;      // m_symbols index
    };

    /** Return symbol indices starting with prefix (up to limit, deduplicated by name) */
    std::vector<uint32_t> SearchByPrefix(const std::string& lowerPrefix, int limit) const;

    /** Substring search (fallback when prefix misses) */
    std::vector<uint32_t> SearchBySubstring(const std::string& lowerQuery, int limit) const;

protected:
    // ── Dense Arrays (SOA) ────────────────────────────────────────────────
    std::vector<CSymbol>    m_symbols;
    std::vector<BakedEdge>  m_edges;

    // ── Hash Indices ────────────────────────────────────────────────────
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_nameIndex;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_fromIndex;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_toIndex;

    // ── Sorted Index (for prefix search) ──────────────────────────────────────
    std::vector<SortedNameEntry> m_sortedNames;

    /** Empty vector sentinel — returned by reference on lookup miss */
    static const std::vector<uint32_t> s_empty;
};

} // namespace fce
