#pragma once

#include "Baked/Data/BakedSymbol.hpp"
#include "Baked/Data/BakedEdge.hpp"
#include "Baked/StringPool.hpp"
#include <tree_sitter/api.h>
#include <string_view>
#include <vector>

namespace fce
{

/** DFS stack frame (shared with handler functions) */
struct BakeFrame
{
    TSNode   node;
    uint32_t parentClassNameId;   ///< Owning struct/class name ID (0 = global)
};

/** Baking result container (POD vectors) */
struct BakeResult
{
    std::vector<BakedSymbol> symbols;
    std::vector<BakedEdge>   edges;
};

/**
 * Handler function signature.
 *
 * All handlers must share the same signature to fit in the function pointer table.
 * Handlers that don't use the stack still accept the parameter but ignore it.
 */
using HandleFn = void(*)(TSNode node,
                          std::string_view source,
                          uint32_t fileId,
                          uint32_t parentId,
                          StringPool& pool,
                          BakeResult& result,
                          std::vector<BakeFrame>& stack);

/**
 * Function pointer vtable — direct dispatch by TSSymbol(uint16_t) index.
 *
 * - C table: only struct/function/macro registered, class/namespace = nullptr
 * - C++ table: all handlers registered
 *
 * In the DFS loop: table[ts_node_symbol(node)] -> O(1) dispatch, 0 branch mispredictions.
 */
struct HandlerSet
{
    static constexpr size_t kMaxSymbol = 1024;
    HandleFn table[kMaxSymbol]{};

    void Register(TSSymbol sym, HandleFn fn)
    {
        if (sym > 0 && sym < kMaxSymbol) table[sym] = fn;
    }

    HandleFn operator[](TSSymbol sym) const
    {
        return (sym < kMaxSymbol) ? table[sym] : nullptr;
    }
};

} // namespace fce
