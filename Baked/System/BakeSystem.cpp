#include "Baked/System/BakeSystem.hpp"
#include "Baked/Component/Components.hpp"
#include "Baked/Data/BakedSymbol.hpp"
#include "Baked/Data/BakedEdge.hpp"
#include "Baked/Store/SymbolStore.hpp"

namespace fce
{

uint32_t BakeSystem::CreateSymbol(SymbolStore& store,
                                   const BakedSymbol& sym)
{
    return store.AddSymbol(CSymbol{
        sym.nameId, sym.fileId, sym.kind,
        sym.startLine, sym.endLine,
        sym.sigId, sym.parentId
    });
}

uint32_t BakeSystem::CreateEdge(SymbolStore& store,
                                 const BakedEdge& edge)
{
    return store.AddEdge(edge);
}

} // namespace fce
