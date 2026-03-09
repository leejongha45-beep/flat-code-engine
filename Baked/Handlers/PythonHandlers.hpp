#pragma once
/**
 * PythonHandlers.hpp — Python tree-sitter handlers
 *
 * function_definition, class_definition, import, decorated_definition.
 */

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register Python handlers.
 *
 * class, function, import, decorated_definition.
 */
void RegisterPythonHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
