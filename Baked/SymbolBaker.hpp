#pragma once

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>
#include <string_view>

namespace fce
{

/**
 * SymbolBaker — DFS orchestrator with handler table binding.
 *
 * Only handles the DFS loop. Actual parsing logic is in handler functions registered in the HandlerSet table.
 * table[TSSymbol] -> O(1) dispatch. No strcmp chains.
 *
 * Runs independently per thread: HandlerSet is read-only (function pointers), only StringPool needs sync.
 */
class SymbolBaker
{
public:
    /** Bind a handler table */
    void Possess(const HandlerSet* set)   { m_possessed = set; }
    void UnPossess()                       { m_possessed = nullptr; }

    /**
     * Bake a single file: TSNode root -> BakedSymbol/BakedEdge POD output.
     *
     * @pre A handler table must be bound via Possess() beforehand.
     */
    BakeResult BakeFile(TSNode root,
                        std::string_view source,
                        std::string_view filePath,
                        StringPool& pool) const;

protected:
    const HandlerSet* m_possessed = nullptr;
};

} // namespace fce
