#pragma once
/**
 * RustHandlers.hpp — Rust tree-sitter handlers
 *
 * function, struct, enum, trait, impl, mod, use, type alias, const, static, macro.
 */

#include "Baked/Handlers/HandlerSet.hpp"
#include <tree_sitter/api.h>

namespace fce::handlers
{

/**
 * Register Rust handlers.
 */
void RegisterRustHandlers(HandlerSet& set, TSLanguage* lang);

} // namespace fce::handlers
