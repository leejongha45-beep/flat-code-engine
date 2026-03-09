#include "Baked/Handlers/CSharpHandlers.hpp"
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
 * Collect all call target names from invocation_expression nodes in the subtree.
 * Traversed via DFS stack (no recursion — dev-guidelines).
 *
 * C# invocation_expression: child[0] is identifier or member_access_expression.
 * member_access_expression -> field "name" -> the method name.
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

        if (std::strcmp(ts_node_type(cur), "invocation_expression") == 0)
        {
            TSNode funcNode = ts_node_child(cur, 0);
            if (!ts_node_is_null(funcNode))
            {
                const char* type = ts_node_type(funcNode);
                if (std::strcmp(type, "identifier") == 0)
                {
                    auto name = NodeText(funcNode, source);
                    if (!name.empty())
                        out.push_back(name);
                }
                else if (std::strcmp(type, "member_access_expression") == 0)
                {
                    TSNode nameNode = ts_node_child_by_field_name(funcNode, "name", 4);
                    if (!ts_node_is_null(nameNode))
                    {
                        auto name = NodeText(nameNode, source);
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

/**
 * Extract type name from a type node (identifier, generic_name, qualified_name, predefined_type).
 * For generic_name: returns just the "name" field (e.g., "List" from "List<T>").
 */
std::string_view ExtractTypeName(TSNode typeNode, std::string_view source)
{
    if (ts_node_is_null(typeNode)) return {};

    const char* type = ts_node_type(typeNode);

    if (std::strcmp(type, "identifier") == 0 || std::strcmp(type, "predefined_type") == 0)
        return NodeText(typeNode, source);

    if (std::strcmp(type, "generic_name") == 0)
    {
        TSNode nameNode = ts_node_child_by_field_name(typeNode, "name", 4);
        if (!ts_node_is_null(nameNode))
            return NodeText(nameNode, source);
    }

    if (std::strcmp(type, "qualified_name") == 0)
        return NodeText(typeNode, source);

    return {};
}

/**
 * Extract Inherits edges from a base_list node.
 * base_list contains comma-separated types (identifier, generic_name, qualified_name).
 */
void ExtractBaseList(TSNode node, std::string_view source,
                     uint32_t nameId, uint32_t fileId,
                     fce::StringPool& pool, fce::BakeResult& result)
{
    // Look for base_list child
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "base_list") != 0) continue;

        static constexpr uint32_t kMaxBases = 64;
        const uint32_t baseCount = ts_node_child_count(child);
        for (uint32_t j = 0; j < baseCount && j < kMaxBases; ++j)
        {
            TSNode baseChild = ts_node_child(child, j);
            if (!ts_node_is_named(baseChild)) continue;

            auto baseName = ExtractTypeName(baseChild, source);
            if (!baseName.empty())
            {
                uint32_t baseId = pool.Intern(baseName);
                result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
            }
        }
        break;  // Only one base_list per declaration
    }
}

/**
 * Build signature from node start to body start, trimming trailing '{' and whitespace.
 */
uint32_t BuildSignature(TSNode node, TSNode bodyNode,
                        std::string_view source, fce::StringPool& pool)
{
    if (ts_node_is_null(bodyNode)) return 0;

    uint32_t sigStart = ts_node_start_byte(node);
    uint32_t sigEnd   = ts_node_start_byte(bodyNode);
    if (sigEnd <= sigStart || sigEnd > source.size()) return 0;

    auto sig = source.substr(sigStart, sigEnd - sigStart);
    while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\n' ||
                             sig.back() == '\r' || sig.back() == '\t' ||
                             sig.back() == '{'))
        sig.remove_suffix(1);

    if (sig.empty()) return 0;
    return pool.Intern(sig);
}

/**
 * Push all named children of a body node onto the DFS stack.
 */
void PushBodyChildren(TSNode bodyNode, uint32_t parentId,
                      std::vector<fce::BakeFrame>& stack)
{
    if (ts_node_is_null(bodyNode)) return;
    const uint32_t cc = ts_node_child_count(bodyNode);
    for (uint32_t i = cc; i-- > 0;)
        stack.push_back({ ts_node_child(bodyNode, i), parentId });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsClass — class_declaration, record_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsClass(TSNode node, std::string_view source,
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

    // Step 2: Signature — from node start to body start
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = BuildSignature(node, bodyNode, source, pool);

    // Step 3: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 4: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Class,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, parentId
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Edge — MemberOf (nested class)
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Inheritance — base_list -> Inherits edges
    ExtractBaseList(node, source, nameId, fileId, pool, result);

    // Step 8: Body children — parentClassNameId = this class
    PushBodyChildren(bodyNode, nameId, stack);
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsStruct — struct_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsStruct(TSNode node, std::string_view source,
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

    // Step 2: Signature
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = BuildSignature(node, bodyNode, source, pool);

    // Step 3: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 4: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Struct,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, parentId
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Inheritance — base_list -> Inherits edges
    ExtractBaseList(node, source, nameId, fileId, pool, result);

    // Step 8: Body children
    PushBodyChildren(bodyNode, nameId, stack);
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsInterface — interface_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsInterface(TSNode node, std::string_view source,
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

    // Step 2: Signature
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = BuildSignature(node, bodyNode, source, pool);

    // Step 3: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 4: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Interface,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, parentId
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Edge — MemberOf (nested interface)
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Inheritance — base_list -> Inherits edges (interface extends)
    ExtractBaseList(node, source, nameId, fileId, pool, result);

    // Step 8: Body children
    PushBodyChildren(bodyNode, nameId, stack);
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsEnum — enum_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsEnum(TSNode node, std::string_view source,
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

    // Step 2: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 3: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Enum,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // Step 4: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 5: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsMethod — method_declaration, constructor_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsMethod(TSNode node, std::string_view source,
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

    // Step 2: Signature — from node start to body start
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = BuildSignature(node, bodyNode, source, pool);

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

    // Step 8: Return type — "type" field (method_declaration only, constructor has none)
    const char* nodeType = ts_node_type(node);
    if (std::strcmp(nodeType, "method_declaration") == 0)
    {
        TSNode retType = ts_node_child_by_field_name(node, "type", 4);
        if (!ts_node_is_null(retType))
        {
            auto typeName = ExtractTypeName(retType, source);
            if (!typeName.empty())
            {
                uint32_t tid = pool.Intern(typeName);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
            }
        }
    }

    // Step 9: Parameter types — TypedAs edges
    TSNode params = ts_node_child_by_field_name(node, "parameters", 10);
    if (!ts_node_is_null(params))
    {
        static constexpr uint32_t kMaxParams = 64;
        const uint32_t cc = ts_node_child_count(params);
        for (uint32_t i = 0; i < cc && i < kMaxParams; ++i)
        {
            TSNode param = ts_node_child(params, i);
            if (!ts_node_is_named(param)) continue;
            if (std::strcmp(ts_node_type(param), "parameter") != 0) continue;

            TSNode paramType = ts_node_child_by_field_name(param, "type", 4);
            if (ts_node_is_null(paramType)) continue;

            auto typeName = ExtractTypeName(paramType, source);
            if (!typeName.empty())
            {
                uint32_t tid = pool.Intern(typeName);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsProperty — property_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsProperty(TSNode node, std::string_view source,
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

    // Step 2: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 3: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Variable,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // Step 4: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 5: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsNamespace — namespace_declaration, file_scoped_namespace_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsNamespace(TSNode node, std::string_view source,
                       uint32_t fileId, uint32_t /*parentId*/,
                       fce::StringPool& pool, fce::BakeResult& result,
                       std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Name extraction (may be identifier or qualified_name)
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 3: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Namespace,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    // Step 4: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 5: Body children — parentId = 0 (namespace resets context like C++)
    const char* nodeType = ts_node_type(node);
    if (std::strcmp(nodeType, "namespace_declaration") == 0)
    {
        // Regular namespace: has explicit "body" (declaration_list)
        TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
        PushBodyChildren(bodyNode, 0, stack);
    }
    else
    {
        // File-scoped namespace: children include all declarations
        const uint32_t cc = ts_node_named_child_count(node);
        for (uint32_t i = cc; i-- > 0;)
        {
            TSNode child = ts_node_named_child(node, i);
            // Skip the name node itself
            if (ts_node_start_byte(child) == ts_node_start_byte(nameNode) &&
                ts_node_end_byte(child) == ts_node_end_byte(nameNode))
                continue;
            stack.push_back({ child, 0 });
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsUsing — using_directive
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsUsing(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t /*parentId*/,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    // Find the first named child that is identifier or qualified_name
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;

        const char* childType = ts_node_type(child);
        if (std::strcmp(childType, "identifier") != 0 &&
            std::strcmp(childType, "qualified_name") != 0)
            continue;

        auto usingName = NodeText(child, source);
        if (usingName.empty()) continue;

        const uint32_t usingId = pool.Intern(usingName);

        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt   = ts_node_end_point(node);

        result.symbols.push_back(fce::BakedSymbol{
            usingId, fileId, fce::SymbolKind::Include,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            0, 0
        });

        result.edges.push_back(fce::BakedEdge{ fileId, usingId, fileId, fce::RelationKind::Includes });
        break;  // Only one import target per using directive
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsField — field_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsField(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t parentId,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    // field_declaration -> variable_declaration -> variable_declarator -> "name" (identifier)
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "variable_declaration") != 0) continue;

        const uint32_t vc = ts_node_child_count(child);
        for (uint32_t j = 0; j < vc; ++j)
        {
            TSNode declarator = ts_node_child(child, j);
            if (!ts_node_is_named(declarator)) continue;
            if (std::strcmp(ts_node_type(declarator), "variable_declarator") != 0) continue;

            TSNode nameNode = ts_node_child_by_field_name(declarator, "name", 4);
            if (ts_node_is_null(nameNode)) continue;

            auto name = NodeText(nameNode, source);
            if (name.empty()) continue;
            const uint32_t nameId = pool.Intern(name);

            TSPoint startPt = ts_node_start_point(node);
            TSPoint endPt   = ts_node_end_point(node);

            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Variable,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, parentId
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

            if (parentId != 0)
                result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
        }
        break;  // Only one variable_declaration per field_declaration
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleCsEvent — event_field_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleCsEvent(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t parentId,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    // Similar to field: extract name from variable_declaration -> variable_declarator
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "variable_declaration") != 0) continue;

        const uint32_t vc = ts_node_child_count(child);
        for (uint32_t j = 0; j < vc; ++j)
        {
            TSNode declarator = ts_node_child(child, j);
            if (!ts_node_is_named(declarator)) continue;
            if (std::strcmp(ts_node_type(declarator), "variable_declarator") != 0) continue;

            TSNode nameNode = ts_node_child_by_field_name(declarator, "name", 4);
            if (ts_node_is_null(nameNode)) continue;

            auto name = NodeText(nameNode, source);
            if (name.empty()) continue;
            const uint32_t nameId = pool.Intern(name);

            TSPoint startPt = ts_node_start_point(node);
            TSPoint endPt   = ts_node_end_point(node);

            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Variable,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, parentId
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

            if (parentId != 0)
                result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
        }
        break;  // Only one variable_declaration per event_field_declaration
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterCSharpHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "class_declaration",                  17, true), HandleCsClass);
    set.Register(ts_language_symbol_for_name(lang, "record_declaration",                 18, true), HandleCsClass);
    set.Register(ts_language_symbol_for_name(lang, "struct_declaration",                 18, true), HandleCsStruct);
    set.Register(ts_language_symbol_for_name(lang, "interface_declaration",              21, true), HandleCsInterface);
    set.Register(ts_language_symbol_for_name(lang, "enum_declaration",                   16, true), HandleCsEnum);
    set.Register(ts_language_symbol_for_name(lang, "method_declaration",                 18, true), HandleCsMethod);
    set.Register(ts_language_symbol_for_name(lang, "constructor_declaration",            23, true), HandleCsMethod);
    set.Register(ts_language_symbol_for_name(lang, "property_declaration",               20, true), HandleCsProperty);
    set.Register(ts_language_symbol_for_name(lang, "namespace_declaration",              21, true), HandleCsNamespace);
    set.Register(ts_language_symbol_for_name(lang, "file_scoped_namespace_declaration",  33, true), HandleCsNamespace);
    set.Register(ts_language_symbol_for_name(lang, "using_directive",                    15, true), HandleCsUsing);
    set.Register(ts_language_symbol_for_name(lang, "field_declaration",                  17, true), HandleCsField);
    set.Register(ts_language_symbol_for_name(lang, "event_field_declaration",            23, true), HandleCsEvent);
}

} // namespace fce::handlers
