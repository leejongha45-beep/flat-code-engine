#include "Core/QuerySystem.hpp"
#include "Core/DensityEvaluator.hpp"
#include <algorithm>
#include <fstream>
#include <queue>
#include <set>
#include <sstream>

namespace fce
{

// ─── Query (BFS graph traversal) ─────────────────────────────────────────────

QueryResult QuerySystem::Query(
    const std::string& name,
    int bfsDepth,
    RelationMask filter,
    bool reverse,
    const SymbolStore& store,
    const StringPool& pool) const
{
    QueryResult result;

    // 1. Direct symbol lookup
    result.symbols = Lookup(name, store, pool);

    // 2. BFS: forward (EdgesFrom) or reverse (EdgesTo)
    if (bfsDepth > 0 && store.EdgeCount() > 0)
    {
        const uint32_t startNameId = pool.Find(name);
        if (startNameId != 0)
        {
            std::queue<std::pair<uint32_t, int>> bfsQueue;
            std::set<uint32_t> visited;

            bfsQueue.push({startNameId, 0});
            visited.insert(startNameId);

            static constexpr int32_t kMaxVisited = 10'000;

            while (!bfsQueue.empty())
            {
                auto [curNameId, depth] = bfsQueue.front();
                bfsQueue.pop();

                const auto& edges = reverse
                    ? store.EdgesTo(curNameId)
                    : store.EdgesFrom(curNameId);

                for (uint32_t edgeIdx : edges)
                {
                    const auto& edge = store.GetEdge(edgeIdx);

                    // RelationKind filter (0 = pass all)
                    if (filter != kAllRelations &&
                        !(filter & RelationBit(edge.relation)))
                        continue;

                    const uint32_t nextNameId = reverse
                        ? edge.fromNameId : edge.toNameId;

                    if (visited.count(nextNameId)) continue;
                    visited.insert(nextNameId);

                    if (static_cast<int32_t>(visited.size()) > kMaxVisited)
                        break;

                    result.related.emplace_back(pool.Resolve(nextNameId));

                    // ── Rich metadata collection (Backward Call Graph) ──
                    const auto& symIndices = store.LookupByNameId(nextNameId);
                    if (!symIndices.empty())
                    {
                        const auto& sym = store.GetSymbol(symIndices[0]);
                        RelatedSymbol rs;
                        rs.name       = std::string(pool.Resolve(nextNameId));
                        rs.kind       = KindToString(sym.kind);
                        rs.file_path  = std::string(pool.Resolve(sym.fileId));
                        rs.start_line = sym.startLine;
                        rs.end_line   = sym.endLine;
                        rs.signature  = sym.sigId ? std::string(pool.Resolve(sym.sigId)) : "";
                        rs.relation   = reverse ? InverseRelation(edge.relation) : edge.relation;
                        rs.depth      = depth + 1;
                        result.related_symbols.push_back(std::move(rs));
                    }

                    if (depth + 1 < bfsDepth)
                        bfsQueue.push({nextNameId, depth + 1});
                }

                if (static_cast<int32_t>(visited.size()) > kMaxVisited)
                    break;
            }
        }
    }

    return result;
}

// ─── Lookup ─────────────────────────────────────────────────────────────────

std::vector<SymbolEntry> QuerySystem::Lookup(
    const std::string& name,
    const SymbolStore& store,
    const StringPool& pool) const
{
    std::vector<SymbolEntry> results;

    const uint32_t nameId = pool.Find(name);
    if (nameId == 0) return results;

    const auto& indices = store.LookupByNameId(nameId);
    results.reserve(indices.size());

    for (uint32_t idx : indices)
    {
        const auto& sym = store.GetSymbol(idx);

        SymbolEntry entry;
        entry.name       = name;
        entry.kind       = KindToString(sym.kind);
        entry.file_path  = std::string(pool.Resolve(sym.fileId));
        entry.start_line = sym.startLine;
        entry.end_line   = sym.endLine;

        if (sym.sigId != 0)
            entry.signature = std::string(pool.Resolve(sym.sigId));

        if (sym.parentId != 0)
            entry.parent_class = std::string(pool.Resolve(sym.parentId));

        results.push_back(std::move(entry));
    }

    return results;
}

// ─── SmartQuery (engine-driven density evaluation + auto-expansion) ──────────

SmartQueryResult QuerySystem::SmartQuery(
    const std::string& name,
    const DensityConfig& densityConfig,
    const SymbolStore& store,
    const StringPool& pool) const
{
    SmartQueryResult result;

    // 1. Lookup → determine SymbolKind
    result.data.symbols = Lookup(name, store, pool);
    SymbolKind symbolKind = SymbolKind::Unknown;
    if (!result.data.symbols.empty())
        symbolKind = StringToKind(result.data.symbols[0].kind);

    // 2. Pre-query planner
    //    Phase 0: 1-hop bidirectional (always)
    //    Phase 1: result == 1 → single lead → trace with 2-hop bidirectional
    //    0 = fully isolated → 2-hop is pointless, 2+ = sufficient

    std::set<std::string> seen;

    auto mergeResult = [&seen, &result](QueryResult& stepResult)
    {
        for (auto& relName : stepResult.related)
        {
            if (seen.insert(relName).second)
                result.data.related.push_back(std::move(relName));
        }
        for (auto& rs : stepResult.related_symbols)
        {
            if (seen.count(rs.name) || seen.insert(rs.name).second)
                result.data.related_symbols.push_back(std::move(rs));
        }
    };

    // 3. Phase 0: 1-hop bidirectional set
    {
        QueryResult fwd = Query(name, 1, kSemanticRelations, false, store, pool);
        QueryResult rev = Query(name, 1, kSemanticRelations, true,  store, pool);
        mergeResult(fwd);
        mergeResult(rev);
    }

    // 4. Phase 1: exactly 1 lead → trace with 2-hop
    if (result.data.related_symbols.size() == 1)
    {
        QueryResult fwd2 = Query(name, 2, kSemanticRelations, false, store, pool);
        QueryResult rev2 = Query(name, 2, kSemanticRelations, true,  store, pool);
        mergeResult(fwd2);
        mergeResult(rev2);
    }

    // 4-b. Goldilocks Zone filter
    {
        auto& rs = result.data.related_symbols;
        const int32_t gMin = densityConfig.goldilocksMin;
        const int32_t gMax = densityConfig.goldilocksMax;

        rs.erase(std::remove_if(rs.begin(), rs.end(),
            [gMin, gMax](const RelatedSymbol& s) {
                const int32_t len = s.end_line - s.start_line;
                if (len <= 0) return false;              // keep declarations (len=0)
                if (len > gMax) return true;             // reject god functions
                // CalledBy(caller) that is too short is a wrapper → reject
                if (s.relation == RelationKind::CalledBy && len < gMin)
                    return true;
                return false;
            }), rs.end());
    }

    // 5. Density metadata (backward compatible)
    result.density = DensityEvaluator::Evaluate(
        symbolKind, result.data.related_symbols, densityConfig);
    result.density.expansionsUsed =
        (result.data.related_symbols.size() == 1) ? 2 : 1;

    return result;
}

// ─── KindToString ───────────────────────────────────────────────────────────

const char* QuerySystem::KindToString(SymbolKind kind)
{
    switch (kind)
    {
    case SymbolKind::Function:  return "FunctionNode";
    case SymbolKind::Class:     return "ClassNode";
    case SymbolKind::Struct:    return "ClassNode";
    case SymbolKind::Enum:      return "EnumNode";
    case SymbolKind::Variable:  return "VariableNode";
    case SymbolKind::Macro:     return "MacroNode";
    case SymbolKind::Alias:     return "AliasNode";
    case SymbolKind::Include:   return "IncludeNode";
    case SymbolKind::Namespace: return "NamespaceNode";
    case SymbolKind::Concept:   return "ConceptNode";
    case SymbolKind::Interface: return "InterfaceNode";
    case SymbolKind::File:      return "FileNode";
    default:                    return "Unknown";
    }
}

// ─── StringToKind ───────────────────────────────────────────────────────────

SymbolKind QuerySystem::StringToKind(const std::string& kindStr)
{
    if (kindStr == "FunctionNode")  return SymbolKind::Function;
    if (kindStr == "ClassNode")    return SymbolKind::Class;
    if (kindStr == "EnumNode")     return SymbolKind::Enum;
    if (kindStr == "VariableNode") return SymbolKind::Variable;
    if (kindStr == "MacroNode")    return SymbolKind::Macro;
    if (kindStr == "AliasNode")    return SymbolKind::Alias;
    if (kindStr == "IncludeNode")  return SymbolKind::Include;
    if (kindStr == "NamespaceNode") return SymbolKind::Namespace;
    if (kindStr == "ConceptNode")  return SymbolKind::Concept;
    if (kindStr == "InterfaceNode") return SymbolKind::Interface;
    if (kindStr == "FileNode")     return SymbolKind::File;
    return SymbolKind::Unknown;
}

// ─── PincerExtract (extract ±radius lines around call site) ─────────────────

SnippetResult QuerySystem::PincerExtract(
    const std::string& filePath,
    int startLine, int endLine,
    const std::string& targetName,
    int radius)
{
    SnippetResult result;
    result.file_path  = filePath;
    result.start_line = startLine;
    result.end_line   = endLine;

    // 1. File read → collect only [startLine, endLine] range
    std::ifstream ifs(filePath);
    if (!ifs.is_open())
        return result;

    std::vector<std::string> lines;
    {
        std::string line;
        int lineNum = 0;
        while (std::getline(ifs, line))
        {
            ++lineNum;
            if (lineNum < startLine) continue;
            if (lineNum > endLine)   break;
            lines.push_back(std::move(line));
        }
    }

    if (lines.empty())
        return result;

    const int totalLines = static_cast<int>(lines.size());

    // 2. If short enough, return entire range
    if (totalLines <= radius * 2 + 5)
    {
        std::ostringstream oss;
        for (int i = 0; i < totalLines; ++i)
        {
            if (i > 0) oss << '\n';
            oss << lines[i];
        }
        result.code = oss.str();
        result.pincer_applied = false;
        return result;
    }

    // 3. Search for targetName + "(" pattern
    const std::string pattern = targetName + "(";
    std::vector<int> callLines;

    for (int i = 0; i < totalLines; ++i)
    {
        if (lines[i].find(pattern) != std::string::npos)
            callLines.push_back(i);
    }

    // No matches → return first radius*2 lines
    if (callLines.empty())
    {
        std::ostringstream oss;
        const int cap = std::min(totalLines, radius * 2);
        for (int i = 0; i < cap; ++i)
        {
            if (i > 0) oss << '\n';
            oss << lines[i];
        }
        result.code = oss.str();
        result.pincer_applied = false;
        return result;
    }

    // 4. Create ±radius windows + merge
    std::vector<std::pair<int, int>> regions;
    for (int cl : callLines)
    {
        int lo = std::max(0, cl - radius);
        int hi = std::min(totalLines, cl + radius + 1);
        if (!regions.empty() && lo <= regions.back().second)
            regions.back().second = hi;   // overlap → merge
        else
            regions.emplace_back(lo, hi);
    }

    // 5. Combine regions (insert `// ...` between non-adjacent regions)
    std::ostringstream oss;
    for (size_t r = 0; r < regions.size(); ++r)
    {
        auto [lo, hi] = regions[r];
        if (lo > 0)
            oss << "// ...\n";
        for (int i = lo; i < hi; ++i)
        {
            oss << lines[i];
            if (i + 1 < hi || r + 1 < regions.size() || hi < totalLines)
                oss << '\n';
        }
    }
    if (regions.back().second < totalLines)
        oss << "// ...";

    result.code = oss.str();
    result.pincer_applied = true;
    return result;
}

} // namespace fce
