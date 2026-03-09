#pragma once

#include "Symbol/RelationKind.hpp"
#include "Symbol/DensityConfig.hpp"
#include <string>
#include <vector>

namespace fce
{

/** Location and metadata for a single symbol */
struct SymbolEntry
{
    std::string kind;          ///< "FunctionNode", "ClassNode", etc.
    std::string name;
    std::string parent_class;  ///< Owning class name (empty string if global)
    std::string file_path;
    int start_line = 0;
    int end_line   = 0;
    std::string signature;
    std::vector<std::string> template_params;
};

/** Rich metadata for BFS-reached symbols (Backward Call Graph packaging) */
struct RelatedSymbol
{
    std::string    name;
    std::string    kind;
    std::string    file_path;
    int            start_line = 0;
    int            end_line   = 0;
    std::string    signature;
    RelationKind   relation = RelationKind::Calls;
    int            depth    = 0;
};

/** FceEngine::Query return value */
struct QueryResult
{
    /** (1) Direct symbol lookup results */
    std::vector<SymbolEntry> symbols;
    /** (2) BFS-reached node names */
    std::vector<std::string> related;
    /** (3) BFS-reached node rich metadata (edge type + symbol info) */
    std::vector<RelatedSymbol> related_symbols;
};

/** SmartQuery return value — query results + density metadata */
struct SmartQueryResult
{
    QueryResult    data;       ///< Combined query result
    DensityResult  density;    ///< Density evaluation result
};

/** Pincer extraction result — code snippet containing only ±radius lines around call sites */
struct SnippetResult
{
    std::string file_path;
    int         start_line     = 0;     ///< Original start line (1-based)
    int         end_line       = 0;     ///< Original end line (1-based)
    std::string code;                   ///< Extracted code (abbreviated when pincer applied)
    bool        pincer_applied = false; ///< Whether pincer extraction was applied
};

} // namespace fce
