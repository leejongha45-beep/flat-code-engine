#pragma once
/**
 * JavaHandlers.hpp — Java tree-sitter handlers
 *
 * class, interface, enum, method, field, import, annotation type.
 */

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register Java handlers.
 *
 * class, interface, enum, method, constructor, field, import.
 */
void RegisterJavaHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
