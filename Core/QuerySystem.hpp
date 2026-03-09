#pragma once

#include "Baked/Component/Components.hpp"
#include "Baked/StringPool.hpp"
#include "Baked/Store/SymbolStore.hpp"
#include "Symbol/RelationKind.hpp"
#include "Symbol/DensityConfig.hpp"
#include "Core/QueryEngine.hpp"
#include <string>
#include <vector>

namespace fce
{

/**
 * Query system — BFS graph traversal + symbol lookup.
 *
 * Receives SymbolStore/StringPool via injection for read-only traversal.
 */
class QuerySystem
{
public:
    /**
     * BFS graph traversal.
     *
     * @param name       Symbol name to search for
     * @param bfsDepth   BFS depth (0 = direct lookup only)
     * @param filter     RelationMask filter (kAllRelations = pass all)
     * @param reverse    true → EdgesTo (reverse direction, CalledBy etc.)
     */
    QueryResult Query(const std::string& name,
                      int bfsDepth,
                      RelationMask filter,
                      bool reverse,
                      const SymbolStore& store,
                      const StringPool& pool) const;

    /** Direct symbol lookup by name → convert to SymbolEntry */
    std::vector<SymbolEntry> Lookup(const std::string& name,
                                     const SymbolStore& store,
                                     const StringPool& pool) const;

    /**
     * Engine-driven intelligent query — density evaluation + auto-expansion.
     *
     * 1. depth=1 forward query
     * 2. Density evaluation → auto-expand if insufficient (reverse, depth+1)
     * 3. Return when dual gate passes or maxExpansions reached
     */
    SmartQueryResult SmartQuery(const std::string& name,
                                const DensityConfig& densityConfig,
                                const SymbolStore& store,
                                const StringPool& pool) const;

    /**
     * Pincer extraction — extract only ±radius lines around target call sites from a file.
     *
     * @param filePath    Source file path
     * @param startLine   Symbol start line (1-based)
     * @param endLine     Symbol end line (1-based)
     * @param targetName  Target function name to find call sites for
     * @param radius      Number of lines before/after each call site (default 7)
     */
    static SnippetResult PincerExtract(
        const std::string& filePath,
        int startLine, int endLine,
        const std::string& targetName,
        int radius = 7);

    /** SymbolKind → string (pybind11 compatible) */
    static const char* KindToString(SymbolKind kind);

    /** String → SymbolKind (internal use by SmartQuery) */
    static SymbolKind StringToKind(const std::string& kindStr);
};

} // namespace fce
