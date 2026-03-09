#pragma once

#include "Symbol/RelationKind.hpp"
#include <cstdint>

namespace fce
{

/**
 * Thread-local parse result — edge POD (0 heap allocation, 16 bytes)
 *
 * IDs reference the thread-local StringPool.
 * Re-interned into the global pool during main-thread Flush, then relation entities are created.
 */
struct BakedEdge
{
    uint32_t fromNameId  = 0;
    uint32_t toNameId    = 0;
    uint32_t fileId      = 0;   ///< For bulk deletion on file modification
    RelationKind relation = RelationKind::DefinedIn;
};

} // namespace fce
