#pragma once

#include "Symbol/RelationKind.hpp"
#include "Baked/Component/Components.hpp"
#include <cstdint>

namespace fce
{

/**
 * Density evaluation config — SymbolKind x RelationKind weight 2D table.
 *
 * Dual gate:
 *   Gate 1: weighted sum score >= scoreThreshold
 *   Gate 2: edge type diversity >= minEdgeTypes
 *   Both must pass for sufficient=true
 */
struct DensityConfig
{
    static constexpr int kSymbolKinds   = 12;  ///< SymbolKind enum size
    static constexpr int kRelationKinds = 14;  ///< RelationKind enum size

    /// weights[SymbolKind][RelationKind] — weight for this combination
    float weights[kSymbolKinds][kRelationKinds] = {};

    float scoreThreshold = 5.0f;   ///< Gate 1: minimum weighted sum score
    int   minEdgeTypes   = 2;      ///< Gate 2: minimum edge type diversity
    int   maxExpansions  = 3;      ///< Maximum number of auto-expansions

    // ── Goldilocks Zone ──
    int32_t goldilocksMin = 5;     ///< CalledBy relation minimum line count (reject wrappers)
    int32_t goldilocksMax = 200;   ///< All relations maximum line count (reject god functions)

    /// Create default weight table
    static DensityConfig Default();
};

/// Density evaluation result
struct DensityResult
{
    float weightedScore  = 0.0f;   ///< Weighted sum score
    int   edgeTypeCount  = 0;      ///< Number of detected edge types
    bool  sufficient     = false;  ///< Whether dual gate passed
    int   expansionsUsed = 0;      ///< Number of auto-expansions used
};

} // namespace fce
