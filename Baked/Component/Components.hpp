#pragma once

#include <cstdint>

/**
 * SOA components — pure C++ POD structs (no heap allocation)
 *
 * Symbol: CSymbol (stored flat in SymbolStore::m_symbols)
 * Relation: BakedEdge (stored flat in SymbolStore::m_edges, distinguished by RelationKind)
 */

namespace fce
{

// ── Symbol Kind ──────────────────────────────────────────────────────────

enum class SymbolKind : uint8_t
{
    Function,
    Class,
    Struct,
    Enum,
    Variable,
    Macro,
    Alias,
    Include,
    Namespace,
    Concept,
    Interface,
    File,
    Unknown
};

// ── Symbol POD (SymbolStore dense array element) ─────────────────────────────

/**
 * Complete information for a single parsed code symbol.
 * IDs reference the global StringPool.
 */
struct CSymbol
{
    uint32_t nameId    = 0;
    uint32_t fileId    = 0;
    SymbolKind kind    = SymbolKind::Unknown;
    int32_t startLine  = 0;
    int32_t endLine    = 0;
    uint32_t sigId     = 0;   ///< Signature (0 = none)
    uint32_t parentId  = 0;   ///< Owning struct/class (0 = global)
};

} // namespace fce
