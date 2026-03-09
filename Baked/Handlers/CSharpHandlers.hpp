#pragma once
/**
 * CSharpHandlers.hpp — C# tree-sitter handlers
 *
 * class, struct, interface, enum, method, constructor, property, namespace, using.
 */

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register C# handlers.
 */
void RegisterCSharpHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
