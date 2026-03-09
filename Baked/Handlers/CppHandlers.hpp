#pragma once

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register C++-specific handlers.
 *
 * class, namespace, concept, alias (using X = Y).
 * Internally calls RegisterCommonHandlers as well, so for a C++ table this alone is sufficient.
 */
void RegisterCppHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
