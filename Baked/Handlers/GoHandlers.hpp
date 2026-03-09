#pragma once
/**
 * GoHandlers.hpp — Go tree-sitter handlers
 *
 * function, method, type (struct/interface/alias), import, const, var.
 */

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register Go handlers.
 */
void RegisterGoHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
