#include "Baked/Handlers/JsTsHandlers.hpp"
#include <cstring>

namespace
{

// ─── Utilities ───────────────────────────────────────────────────────────────

std::string_view NodeText(TSNode node, std::string_view source)
{
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end   = ts_node_end_byte(node);
    if (start >= end || end > source.size()) return {};
    return source.substr(start, end - start);
}

/**
 * Collect all call target names from call_expression nodes in the subtree.
 * Traversed via DFS stack (no recursion).
 *
 * call_expression: "function" field -> identifier or member_expression.
 * member_expression: "property" field -> property_identifier.
 */
void CollectCallTargets(TSNode root, std::string_view source,
                        std::vector<std::string_view>& out)
{
    std::vector<TSNode> dfs;
    dfs.push_back(root);

    static constexpr uint32_t kMaxIter = 10000;
    for (uint32_t iter = 0; iter < kMaxIter && !dfs.empty(); ++iter)
    {
        TSNode cur = dfs.back();
        dfs.pop_back();

        if (std::strcmp(ts_node_type(cur), "call_expression") == 0)
        {
            TSNode funcNode = ts_node_child_by_field_name(cur, "function", 8);
            if (!ts_node_is_null(funcNode))
            {
                const char* type = ts_node_type(funcNode);
                if (std::strcmp(type, "identifier") == 0)
                {
                    auto name = NodeText(funcNode, source);
                    if (!name.empty())
                        out.push_back(name);
                }
                else if (std::strcmp(type, "member_expression") == 0)
                {
                    TSNode prop = ts_node_child_by_field_name(funcNode, "property", 8);
                    if (!ts_node_is_null(prop))
                    {
                        auto name = NodeText(prop, source);
                        if (!name.empty())
                            out.push_back(name);
                    }
                }
            }
        }

        const uint32_t cc = ts_node_child_count(cur);
        for (uint32_t i = cc; i-- > 0;)
            dfs.push_back(ts_node_child(cur, i));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJsFunction — function_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJsFunction(TSNode node, std::string_view source,
                      uint32_t fileId, uint32_t parentId,
                      fce::StringPool& pool, fce::BakeResult& result,
                      std::vector<fce::BakeFrame>& /*stack*/)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Signature — from function start to body start
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = 0;
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t sigStart = ts_node_start_byte(node);
        uint32_t sigEnd   = ts_node_start_byte(bodyNode);
        if (sigEnd > sigStart && sigEnd <= source.size())
        {
            auto sig = source.substr(sigStart, sigEnd - sigStart);
            while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\n' ||
                                     sig.back() == '\r' || sig.back() == '\t' ||
                                     sig.back() == '{'))
                sig.remove_suffix(1);
            sigId = pool.Intern(sig);
        }
    }

    // Step 3: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 4: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Function,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, parentId
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Call relation — Calls
    if (!ts_node_is_null(bodyNode))
    {
        std::vector<std::string_view> calls;
        CollectCallTargets(bodyNode, source, calls);
        for (auto callee : calls)
        {
            uint32_t calleeId = pool.Intern(callee);
            result.edges.push_back(fce::BakedEdge{ nameId, calleeId, fileId, fce::RelationKind::Calls });
        }
    }

    // Step 8: Return type — return_type (TS type annotation, null for JS)
    TSNode retType = ts_node_child_by_field_name(node, "return_type", 11);
    if (!ts_node_is_null(retType))
    {
        auto typeText = NodeText(retType, source);
        if (!typeText.empty())
        {
            uint32_t tid = pool.Intern(typeText);
            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJsClass — class_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJsClass(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t parentId,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Emit symbol
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Class,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // Step 3: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 4: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 5: Inheritance — class_heritage child contains extends_clause
    static constexpr uint32_t kMaxChildren = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxChildren; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;

        if (std::strcmp(ts_node_type(child), "class_heritage") == 0)
        {
            const uint32_t hcc = ts_node_child_count(child);
            for (uint32_t j = 0; j < hcc; ++j)
            {
                TSNode hChild = ts_node_child(child, j);
                if (!ts_node_is_named(hChild)) continue;

                const char* hType = ts_node_type(hChild);

                std::string_view baseName;
                if (std::strcmp(hType, "identifier") == 0)
                {
                    baseName = NodeText(hChild, source);
                }
                else if (std::strcmp(hType, "member_expression") == 0)
                {
                    TSNode prop = ts_node_child_by_field_name(hChild, "property", 8);
                    if (!ts_node_is_null(prop))
                        baseName = NodeText(prop, source);
                }

                if (!baseName.empty())
                {
                    uint32_t baseId = pool.Intern(baseName);
                    result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
                }
            }
        }
    }

    // Step 6: Body children — parentClassNameId = this class
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t bcc = ts_node_child_count(body);
        for (uint32_t i = bcc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJsMethod — method_definition
// ═══════════════════════════════════════════════════════════════════════════

void HandleJsMethod(TSNode node, std::string_view source,
                    uint32_t fileId, uint32_t parentId,
                    fce::StringPool& pool, fce::BakeResult& result,
                    std::vector<fce::BakeFrame>& /*stack*/)
{
    // Step 1: Name extraction — skip computed property names
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;
    if (std::strcmp(ts_node_type(nameNode), "computed_property_name") == 0) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Signature — from node start to body start
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = 0;
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t sigStart = ts_node_start_byte(node);
        uint32_t sigEnd   = ts_node_start_byte(bodyNode);
        if (sigEnd > sigStart && sigEnd <= source.size())
        {
            auto sig = source.substr(sigStart, sigEnd - sigStart);
            while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\n' ||
                                     sig.back() == '\r' || sig.back() == '\t' ||
                                     sig.back() == '{'))
                sig.remove_suffix(1);
            sigId = pool.Intern(sig);
        }
    }

    // Step 3: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 4: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Function,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, parentId
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Call relation — Calls
    if (!ts_node_is_null(bodyNode))
    {
        std::vector<std::string_view> calls;
        CollectCallTargets(bodyNode, source, calls);
        for (auto callee : calls)
        {
            uint32_t calleeId = pool.Intern(callee);
            result.edges.push_back(fce::BakedEdge{ nameId, calleeId, fileId, fce::RelationKind::Calls });
        }
    }

    // Step 8: Return type — return_type (TS type annotation, null for JS)
    TSNode retType = ts_node_child_by_field_name(node, "return_type", 11);
    if (!ts_node_is_null(retType))
    {
        auto typeText = NodeText(retType, source);
        if (!typeText.empty())
        {
            uint32_t tid = pool.Intern(typeText);
            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
        }
    }

    // Step 9: Parameter types — TypedAs edges (TS type annotations)
    TSNode params = ts_node_child_by_field_name(node, "parameters", 10);
    if (!ts_node_is_null(params))
    {
        const uint32_t pcc = ts_node_child_count(params);
        for (uint32_t i = 0; i < pcc; ++i)
        {
            TSNode param = ts_node_child(params, i);
            if (!ts_node_is_named(param)) continue;

            TSNode typeAnnotation = ts_node_child_by_field_name(param, "type", 4);
            if (ts_node_is_null(typeAnnotation)) continue;

            auto typeText = NodeText(typeAnnotation, source);
            if (!typeText.empty())
            {
                uint32_t tid = pool.Intern(typeText);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJsVariable — variable_declaration / lexical_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJsVariable(TSNode node, std::string_view source,
                      uint32_t fileId, uint32_t parentId,
                      fce::StringPool& pool, fce::BakeResult& result,
                      std::vector<fce::BakeFrame>& /*stack*/)
{
    static constexpr uint32_t kMaxDeclarators = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxDeclarators; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "variable_declarator") != 0) continue;

        // Name — skip destructuring patterns
        TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
        if (ts_node_is_null(nameNode)) continue;

        const char* nameType = ts_node_type(nameNode);
        if (std::strcmp(nameType, "identifier") != 0) continue; // skip object_pattern, array_pattern

        auto name = NodeText(nameNode, source);
        if (name.empty()) continue;
        const uint32_t nameId = pool.Intern(name);

        // Check if the value is an arrow_function or function expression
        TSNode valueNode = ts_node_child_by_field_name(child, "value", 5);
        bool bIsFunction = false;
        if (!ts_node_is_null(valueNode))
        {
            const char* valType = ts_node_type(valueNode);
            bIsFunction = (std::strcmp(valType, "arrow_function") == 0 ||
                           std::strcmp(valType, "function") == 0);
        }

        fce::SymbolKind kind = bIsFunction ? fce::SymbolKind::Function : fce::SymbolKind::Variable;

        // Signature for function-valued variables
        uint32_t sigId = 0;
        if (bIsFunction)
        {
            TSNode funcBody = ts_node_child_by_field_name(valueNode, "body", 4);
            if (!ts_node_is_null(funcBody))
            {
                uint32_t sigStart = ts_node_start_byte(node);
                uint32_t sigEnd   = ts_node_start_byte(funcBody);
                if (sigEnd > sigStart && sigEnd <= source.size())
                {
                    auto sig = source.substr(sigStart, sigEnd - sigStart);
                    while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\n' ||
                                             sig.back() == '\r' || sig.back() == '\t' ||
                                             sig.back() == '{'))
                        sig.remove_suffix(1);
                    sigId = pool.Intern(sig);
                }
            }
        }

        // Line information
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt   = ts_node_end_point(node);

        // Emit symbol
        result.symbols.push_back(fce::BakedSymbol{
            nameId, fileId, kind,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            sigId, parentId
        });

        // Edge — DefinedIn
        result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

        // Edge — MemberOf
        if (parentId != 0)
            result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

        // Calls for function-valued variables
        if (bIsFunction && !ts_node_is_null(valueNode))
        {
            TSNode funcBody = ts_node_child_by_field_name(valueNode, "body", 4);
            if (!ts_node_is_null(funcBody))
            {
                std::vector<std::string_view> calls;
                CollectCallTargets(funcBody, source, calls);
                for (auto callee : calls)
                {
                    uint32_t calleeId = pool.Intern(callee);
                    result.edges.push_back(fce::BakedEdge{ nameId, calleeId, fileId, fce::RelationKind::Calls });
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJsImport — import_statement
// ═══════════════════════════════════════════════════════════════════════════

void HandleJsImport(TSNode node, std::string_view source,
                    uint32_t fileId, uint32_t /*parentId*/,
                    fce::StringPool& pool, fce::BakeResult& result,
                    std::vector<fce::BakeFrame>& /*stack*/)
{
    // Extract the module source: field "source" -> string node -> strip quotes
    TSNode srcNode = ts_node_child_by_field_name(node, "source", 6);
    if (ts_node_is_null(srcNode)) return;

    auto srcText = NodeText(srcNode, source);
    if (srcText.size() < 2) return;

    // Strip surrounding quotes (' or ")
    if ((srcText.front() == '\'' || srcText.front() == '"') &&
        (srcText.back() == '\'' || srcText.back() == '"'))
    {
        srcText = srcText.substr(1, srcText.size() - 2);
    }
    if (srcText.empty()) return;

    const uint32_t modId = pool.Intern(srcText);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        modId, fileId, fce::SymbolKind::Include,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ fileId, modId, fileId, fce::RelationKind::Includes });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJsExport — export_statement (pass-through)
// ═══════════════════════════════════════════════════════════════════════════

void HandleJsExport(TSNode node, std::string_view /*source*/,
                    uint32_t /*fileId*/, uint32_t parentId,
                    fce::StringPool& /*pool*/, fce::BakeResult& /*result*/,
                    std::vector<fce::BakeFrame>& stack)
{
    // Push all named children onto stack so inner declarations are handled
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = cc; i-- > 0;)
    {
        TSNode child = ts_node_child(node, i);
        if (ts_node_is_named(child))
            stack.push_back({ child, parentId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleTsInterface — interface_declaration (TS-only)
// ═══════════════════════════════════════════════════════════════════════════

void HandleTsInterface(TSNode node, std::string_view source,
                       uint32_t fileId, uint32_t parentId,
                       fce::StringPool& pool, fce::BakeResult& result,
                       std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Emit symbol
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Interface,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // Step 3: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 4: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 5: Inheritance — extends_type_clause child -> type_identifier -> Inherits
    static constexpr uint32_t kMaxChildren = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxChildren; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;

        if (std::strcmp(ts_node_type(child), "extends_type_clause") == 0)
        {
            const uint32_t ecc = ts_node_child_count(child);
            for (uint32_t j = 0; j < ecc; ++j)
            {
                TSNode eChild = ts_node_child(child, j);
                if (!ts_node_is_named(eChild)) continue;

                const char* eType = ts_node_type(eChild);

                std::string_view baseName;
                if (std::strcmp(eType, "type_identifier") == 0 ||
                    std::strcmp(eType, "identifier") == 0)
                {
                    baseName = NodeText(eChild, source);
                }
                else if (std::strcmp(eType, "member_expression") == 0)
                {
                    TSNode prop = ts_node_child_by_field_name(eChild, "property", 8);
                    if (!ts_node_is_null(prop))
                        baseName = NodeText(prop, source);
                }

                if (!baseName.empty())
                {
                    uint32_t baseId = pool.Intern(baseName);
                    result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
                }
            }
        }
    }

    // Step 6: Body children — parentClassNameId = this interface
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t bcc = ts_node_child_count(body);
        for (uint32_t i = bcc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleTsEnum — enum_declaration (TS-only)
// ═══════════════════════════════════════════════════════════════════════════

void HandleTsEnum(TSNode node, std::string_view source,
                  uint32_t fileId, uint32_t parentId,
                  fce::StringPool& pool, fce::BakeResult& result,
                  std::vector<fce::BakeFrame>& /*stack*/)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Emit symbol
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Enum,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // Step 3: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 4: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleTsTypeAlias — type_alias_declaration (TS-only)
// ═══════════════════════════════════════════════════════════════════════════

void HandleTsTypeAlias(TSNode node, std::string_view source,
                       uint32_t fileId, uint32_t /*parentId*/,
                       fce::StringPool& pool, fce::BakeResult& result,
                       std::vector<fce::BakeFrame>& /*stack*/)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Emit symbol
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Alias,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    // Step 3: Edge — DefinedIn only
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

static void RegisterJsSharedHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "function_declaration",  20, true), HandleJsFunction);
    set.Register(ts_language_symbol_for_name(lang, "class_declaration",     17, true), HandleJsClass);
    set.Register(ts_language_symbol_for_name(lang, "method_definition",     17, true), HandleJsMethod);
    set.Register(ts_language_symbol_for_name(lang, "variable_declaration",  20, true), HandleJsVariable);
    set.Register(ts_language_symbol_for_name(lang, "lexical_declaration",   19, true), HandleJsVariable);
    set.Register(ts_language_symbol_for_name(lang, "import_statement",      16, true), HandleJsImport);
    set.Register(ts_language_symbol_for_name(lang, "export_statement",      16, true), HandleJsExport);
}

static void RegisterTsOnlyHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "interface_declaration",     21, true), HandleTsInterface);
    set.Register(ts_language_symbol_for_name(lang, "enum_declaration",          16, true), HandleTsEnum);
    set.Register(ts_language_symbol_for_name(lang, "type_alias_declaration",    22, true), HandleTsTypeAlias);
}

void RegisterJsHandlers(HandlerSet& set, TSLanguage* lang)
{
    RegisterJsSharedHandlers(set, lang);
}

void RegisterTsHandlers(HandlerSet& set, TSLanguage* lang)
{
    RegisterJsSharedHandlers(set, lang);
    RegisterTsOnlyHandlers(set, lang);
}

void RegisterTsxHandlers(HandlerSet& set, TSLanguage* lang)
{
    RegisterJsSharedHandlers(set, lang);
    RegisterTsOnlyHandlers(set, lang);
}

} // namespace fce::handlers
