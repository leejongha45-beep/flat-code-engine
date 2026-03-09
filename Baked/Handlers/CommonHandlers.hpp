#pragma once

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register handlers common to both C and C++.
 *
 * struct, union, function, enum, macro, include, typedef, declaration.
 * For a C-only table, calling this alone is sufficient.
 */
void RegisterCommonHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
