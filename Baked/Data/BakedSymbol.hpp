#pragma once

#include "Baked/Component/Components.hpp"
#include <cstdint>

namespace fce
{

/**
 * Thread-local parse result — symbol POD (0 heap allocation)
 *
 * IDs reference the thread-local StringPool.
 * Re-interned into the global pool during main-thread Flush, then CSymbol entities are created.
 */
struct BakedSymbol
{
    uint32_t nameId    = 0;
    uint32_t fileId    = 0;
    SymbolKind kind    = SymbolKind::Unknown;
    int32_t startLine  = 0;
    int32_t endLine    = 0;
    uint32_t sigId     = 0;   ///< 0 = none
    uint32_t parentId  = 0;   ///< 0 = global
};

} // namespace fce
