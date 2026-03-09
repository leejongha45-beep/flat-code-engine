#include "Core/DensityEvaluator.hpp"

namespace fce
{

// ─── Default weight table ────────────────────────────────────────────────────

DensityConfig DensityConfig::Default()
{
    DensityConfig cfg;

    // ── Class (idx 1) ──
    constexpr int kClass = static_cast<int>(SymbolKind::Class);
    cfg.weights[kClass][static_cast<int>(RelationKind::Inherits)]  = 3.0f;
    cfg.weights[kClass][static_cast<int>(RelationKind::DerivedBy)] = 2.0f;
    cfg.weights[kClass][static_cast<int>(RelationKind::HasMember)] = 3.0f;
    cfg.weights[kClass][static_cast<int>(RelationKind::Contains)]  = 2.0f;
    cfg.weights[kClass][static_cast<int>(RelationKind::MemberOf)]  = 1.0f;

    // ── Struct (idx 2) — same as Class ──
    constexpr int kStruct = static_cast<int>(SymbolKind::Struct);
    cfg.weights[kStruct][static_cast<int>(RelationKind::Inherits)]  = 3.0f;
    cfg.weights[kStruct][static_cast<int>(RelationKind::DerivedBy)] = 2.0f;
    cfg.weights[kStruct][static_cast<int>(RelationKind::HasMember)] = 3.0f;
    cfg.weights[kStruct][static_cast<int>(RelationKind::Contains)]  = 2.0f;
    cfg.weights[kStruct][static_cast<int>(RelationKind::MemberOf)]  = 1.0f;

    // ── Function (idx 0) ──
    constexpr int kFunc = static_cast<int>(SymbolKind::Function);
    cfg.weights[kFunc][static_cast<int>(RelationKind::Calls)]    = 3.0f;
    cfg.weights[kFunc][static_cast<int>(RelationKind::CalledBy)] = 2.0f;
    cfg.weights[kFunc][static_cast<int>(RelationKind::Returns)]  = 2.0f;
    cfg.weights[kFunc][static_cast<int>(RelationKind::TypedAs)]  = 1.0f;
    cfg.weights[kFunc][static_cast<int>(RelationKind::MemberOf)] = 1.0f;

    // ── Variable (idx 4) ──
    constexpr int kVar = static_cast<int>(SymbolKind::Variable);
    cfg.weights[kVar][static_cast<int>(RelationKind::TypedAs)]  = 2.0f;
    cfg.weights[kVar][static_cast<int>(RelationKind::MemberOf)] = 1.0f;

    // ── Enum (idx 3) ──
    constexpr int kEnum = static_cast<int>(SymbolKind::Enum);
    cfg.weights[kEnum][static_cast<int>(RelationKind::Contains)] = 2.0f;
    cfg.weights[kEnum][static_cast<int>(RelationKind::MemberOf)] = 1.0f;

    // ── Macro (idx 5) ──
    constexpr int kMacro = static_cast<int>(SymbolKind::Macro);
    cfg.weights[kMacro][static_cast<int>(RelationKind::Contains)] = 1.0f;

    return cfg;
}

// ─── Evaluate ───────────────────────────────────────────────────────────────

DensityResult DensityEvaluator::Evaluate(
    SymbolKind symbolKind,
    const std::vector<RelatedSymbol>& related,
    const DensityConfig& config)
{
    DensityResult result;

    const int kindIdx = static_cast<int>(symbolKind);
    if (kindIdx < 0 || kindIdx >= DensityConfig::kSymbolKinds)
        return result;

    // 1. Count occurrences per RelationKind
    int counts[DensityConfig::kRelationKinds] = {};
    for (const auto& sym : related)
    {
        const int relIdx = static_cast<int>(sym.relation);
        if (relIdx >= 0 && relIdx < DensityConfig::kRelationKinds)
            ++counts[relIdx];
    }

    // 2. Sum weighted scores
    float score = 0.0f;
    int typeCount = 0;
    for (int r = 0; r < DensityConfig::kRelationKinds; ++r)
    {
        if (counts[r] > 0)
        {
            score += static_cast<float>(counts[r]) * config.weights[kindIdx][r];
            ++typeCount;
        }
    }

    // 3. Dual gate decision
    result.weightedScore = score;
    result.edgeTypeCount = typeCount;
    result.sufficient = (score >= config.scoreThreshold)
                     && (typeCount >= config.minEdgeTypes);

    return result;
}

} // namespace fce
