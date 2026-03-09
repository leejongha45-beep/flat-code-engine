#pragma once

#include "Symbol/DensityConfig.hpp"
#include "Core/QueryEngine.hpp"
#include <vector>

namespace fce::DensityEvaluator
{

/**
 * Evaluate the density of related_symbols results.
 *
 * @param symbolKind  SymbolKind of the queried symbol
 * @param related     List of RelatedSymbols collected via BFS
 * @param config      Weight + threshold configuration
 * @return            DensityResult (score, type count, pass/fail)
 */
DensityResult Evaluate(SymbolKind symbolKind,
                       const std::vector<RelatedSymbol>& related,
                       const DensityConfig& config);

} // namespace fce::DensityEvaluator
