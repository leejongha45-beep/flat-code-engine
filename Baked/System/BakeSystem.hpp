#pragma once

#include <cstdint>

namespace fce
{
    struct BakedSymbol;
    struct BakedEdge;
    class  SymbolStore;

    /**
     * Baking system — POD -> SymbolStore loading
     *
     * [MainThread] Called at Flush time (not tick-based)
     * - BakedSymbol -> CSymbol addition
     * - BakedEdge   -> Edge addition (RelationKind is already included in BakedEdge)
     *
     * All IDs reference the global StringPool (pre-converted during Flush).
     */
    namespace BakeSystem
    {
        /**
         * Add symbol — convert to CSymbol and store
         *
         * @param store  Global SymbolStore
         * @param sym    Symbol data with IDs referencing the global pool
         * @return Array index
         */
        uint32_t CreateSymbol(SymbolStore& store, const BakedSymbol& sym);

        /**
         * Add edge — store directly
         *
         * @param store  Global SymbolStore
         * @param edge   Edge data with IDs referencing the global pool
         * @return Array index
         */
        uint32_t CreateEdge(SymbolStore& store, const BakedEdge& edge);
    }

} // namespace fce
