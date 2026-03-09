#pragma once
/**
 * JsTsHandlers.hpp — JavaScript / TypeScript tree-sitter handlers
 *
 * JS: function, class, method, variable/lexical, import, export.
 * TS-only: interface, enum, type alias.
 * JS and TS share handler code; TS registers additional TS-only handlers.
 */

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/** Register JavaScript handlers (ES module syntax) */
void RegisterJsHandlers(HandlerSet& set, TSLanguage* lang);

/** Register TypeScript handlers (JS handlers + TS-only: interface, enum, type alias) */
void RegisterTsHandlers(HandlerSet& set, TSLanguage* lang);

/** Register TSX handlers (same as TS handlers, for .tsx files using tsx parser) */
void RegisterTsxHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
