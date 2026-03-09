#include "Baked/Handlers/JavaHandlers.hpp"
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
 * Collect all call target names from method_invocation nodes in the subtree.
 * Traversed via DFS stack (no recursion — dev-guidelines).
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

        if (std::strcmp(ts_node_type(cur), "method_invocation") == 0)
        {
            TSNode nameNode = ts_node_child_by_field_name(cur, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                auto name = NodeText(nameNode, source);
                if (!name.empty())
                    out.push_back(name);
            }
        }

        const uint32_t cc = ts_node_child_count(cur);
        for (uint32_t i = cc; i-- > 0;)
            dfs.push_back(ts_node_child(cur, i));
    }
}

/**
 * Collect type_identifier nodes from a type node subtree.
 * Handles simple types (type_identifier) and generic types (generic_type → "name" field).
 */
void ExtractTypeIdentifiers(TSNode typeNode, std::string_view source,
                             std::vector<std::string_view>& out)
{
    std::vector<TSNode> dfs;
    dfs.push_back(typeNode);

    static constexpr uint32_t kMaxIter = 10000;
    for (uint32_t iter = 0; iter < kMaxIter && !dfs.empty(); ++iter)
    {
        TSNode cur = dfs.back();
        dfs.pop_back();

        const char* type = ts_node_type(cur);

        if (std::strcmp(type, "type_identifier") == 0)
        {
            auto name = NodeText(cur, source);
            if (!name.empty())
                out.push_back(name);
            continue; // no need to descend further
        }

        if (std::strcmp(type, "generic_type") == 0)
        {
            // Extract just the base name (e.g., "List" from "List<String>")
            TSNode nameNode = ts_node_child_by_field_name(cur, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                auto name = NodeText(nameNode, source);
                if (!name.empty())
                    out.push_back(name);
            }
            continue;
        }

        const uint32_t cc = ts_node_child_count(cur);
        for (uint32_t i = cc; i-- > 0;)
            dfs.push_back(ts_node_child(cur, i));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaClass — class_declaration, record_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaClass(TSNode node, std::string_view source,
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

    // Step 2: Signature — from node start to body start, trim trailing '{' and whitespace
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
        nameId, fileId, fce::SymbolKind::Class,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, parentId
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Edge — MemberOf (inner class)
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Inheritance — superclass field → type_identifier or generic_type
    TSNode superclass = ts_node_child_by_field_name(node, "superclass", 10);
    if (!ts_node_is_null(superclass))
    {
        std::vector<std::string_view> bases;
        ExtractTypeIdentifiers(superclass, source, bases);
        for (auto base : bases)
        {
            uint32_t baseId = pool.Intern(base);
            result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
        }
    }

    // Step 8: Interfaces — "interfaces" field → type_list → iterate named children
    TSNode interfaces = ts_node_child_by_field_name(node, "interfaces", 10);
    if (!ts_node_is_null(interfaces))
    {
        // interfaces points to a super_interfaces node; find the type_list inside
        const uint32_t cc = ts_node_child_count(interfaces);
        static constexpr uint32_t kMaxIfaces = 64;
        for (uint32_t i = 0; i < cc && i < kMaxIfaces; ++i)
        {
            TSNode child = ts_node_child(interfaces, i);
            if (!ts_node_is_named(child)) continue;

            std::vector<std::string_view> types;
            ExtractTypeIdentifiers(child, source, types);
            for (auto t : types)
            {
                uint32_t tid = pool.Intern(t);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Inherits });
            }
        }
    }

    // Step 9: Push body children onto DFS stack with parentId = this class
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t cc = ts_node_child_count(bodyNode);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(bodyNode, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaInterface — interface_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaInterface(TSNode node, std::string_view source,
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

    // Step 7: Inheritance — extends_interfaces field → type_list → Inherits edges
    TSNode extendsIfaces = ts_node_child_by_field_name(node, "extends_interfaces", 18);
    if (!ts_node_is_null(extendsIfaces))
    {
        const uint32_t cc = ts_node_child_count(extendsIfaces);
        static constexpr uint32_t kMaxIfaces = 64;
        for (uint32_t i = 0; i < cc && i < kMaxIfaces; ++i)
        {
            TSNode child = ts_node_child(extendsIfaces, i);
            if (!ts_node_is_named(child)) continue;

            std::vector<std::string_view> types;
            ExtractTypeIdentifiers(child, source, types);
            for (auto t : types)
            {
                uint32_t tid = pool.Intern(t);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Inherits });
            }
        }
    }

    // Step 8: Push body children onto DFS stack with parentId = this interface
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t cc = ts_node_child_count(bodyNode);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(bodyNode, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaEnum — enum_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaEnum(TSNode node, std::string_view source,
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

    // Step 5: Edge — MemberOf (nested enum)
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 6: Interfaces — "interfaces" field → type_list → Inherits edges
    TSNode interfaces = ts_node_child_by_field_name(node, "interfaces", 10);
    if (!ts_node_is_null(interfaces))
    {
        const uint32_t cc = ts_node_child_count(interfaces);
        static constexpr uint32_t kMaxIfaces = 64;
        for (uint32_t i = 0; i < cc && i < kMaxIfaces; ++i)
        {
            TSNode child = ts_node_child(interfaces, i);
            if (!ts_node_is_named(child)) continue;

            std::vector<std::string_view> types;
            ExtractTypeIdentifiers(child, source, types);
            for (auto t : types)
            {
                uint32_t tid = pool.Intern(t);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Inherits });
            }
        }
    }

    // Step 7: Push body children onto DFS stack with parentId = this enum
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t cc = ts_node_child_count(bodyNode);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(bodyNode, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaMethod — method_declaration, constructor_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaMethod(TSNode node, std::string_view source,
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

    // Step 2: Signature — from node start to body start (or node end for abstract methods)
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    uint32_t sigId = 0;
    {
        uint32_t sigStart = ts_node_start_byte(node);
        uint32_t sigEnd   = ts_node_is_null(bodyNode)
                            ? ts_node_end_byte(node)
                            : ts_node_start_byte(bodyNode);
        if (sigEnd > sigStart && sigEnd <= source.size())
        {
            auto sig = source.substr(sigStart, sigEnd - sigStart);
            while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\n' ||
                                     sig.back() == '\r' || sig.back() == '\t' ||
                                     sig.back() == '{' || sig.back() == ';'))
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

    // Step 6: Edge — MemberOf (method inside a class/interface/enum)
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 7: Calls — find method_invocation nodes in body
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

    // Step 8: Returns — "type" field (return type, not present on constructors)
    TSNode retType = ts_node_child_by_field_name(node, "type", 4);
    if (!ts_node_is_null(retType))
    {
        std::vector<std::string_view> retTypes;
        ExtractTypeIdentifiers(retType, source, retTypes);
        for (auto rt : retTypes)
        {
            uint32_t tid = pool.Intern(rt);
            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
        }
    }

    // Step 9: TypedAs — parameters → formal_parameter → "type" field
    TSNode params = ts_node_child_by_field_name(node, "parameters", 10);
    if (!ts_node_is_null(params))
    {
        static constexpr uint32_t kMaxParams = 64;
        const uint32_t cc = ts_node_child_count(params);
        for (uint32_t i = 0; i < cc && i < kMaxParams; ++i)
        {
            TSNode child = ts_node_child(params, i);
            if (!ts_node_is_named(child)) continue;

            if (std::strcmp(ts_node_type(child), "formal_parameter") != 0 &&
                std::strcmp(ts_node_type(child), "spread_parameter") != 0)
                continue;

            TSNode paramType = ts_node_child_by_field_name(child, "type", 4);
            if (ts_node_is_null(paramType)) continue;

            std::vector<std::string_view> types;
            ExtractTypeIdentifiers(paramType, source, types);
            for (auto t : types)
            {
                uint32_t tid = pool.Intern(t);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaField — field_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaField(TSNode node, std::string_view source,
                     uint32_t fileId, uint32_t parentId,
                     fce::StringPool& pool, fce::BakeResult& result,
                     std::vector<fce::BakeFrame>& /*stack*/)
{
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Iterate named children to find variable_declarator(s) — handles `int a, b;`
    static constexpr uint32_t kMaxDeclarators = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxDeclarators; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "variable_declarator") != 0) continue;

        TSNode varName = ts_node_child_by_field_name(child, "name", 4);
        if (ts_node_is_null(varName)) continue;

        auto name = NodeText(varName, source);
        if (name.empty()) continue;
        const uint32_t nameId = pool.Intern(name);

        // Emit symbol
        result.symbols.push_back(fce::BakedSymbol{
            nameId, fileId, fce::SymbolKind::Variable,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            0, parentId
        });

        // DefinedIn
        result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

        // MemberOf
        if (parentId != 0)
            result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaImport — import_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaImport(TSNode node, std::string_view source,
                      uint32_t fileId, uint32_t /*parentId*/,
                      fce::StringPool& pool, fce::BakeResult& result,
                      std::vector<fce::BakeFrame>& /*stack*/)
{
    // Find scoped_identifier or identifier child for the import path
    static constexpr uint32_t kMaxChildren = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxChildren; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;

        const char* childType = ts_node_type(child);
        if (std::strcmp(childType, "scoped_identifier") != 0 &&
            std::strcmp(childType, "identifier") != 0)
            continue;

        auto importPath = NodeText(child, source);
        if (importPath.empty()) continue;

        const uint32_t importId = pool.Intern(importPath);
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt   = ts_node_end_point(node);

        result.symbols.push_back(fce::BakedSymbol{
            importId, fileId, fce::SymbolKind::Include,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            0, 0
        });

        result.edges.push_back(fce::BakedEdge{ fileId, importId, fileId, fce::RelationKind::Includes });
        break; // one import path per declaration
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleJavaAnnotationType — annotation_type_declaration (@interface)
// ═══════════════════════════════════════════════════════════════════════════

void HandleJavaAnnotationType(TSNode node, std::string_view source,
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

    // Step 2: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 3: Emit symbol (treated as Class)
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Class,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // Step 4: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 5: Edge — MemberOf
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });

    // Step 6: Push body children onto DFS stack
    TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t cc = ts_node_child_count(bodyNode);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(bodyNode, i), nameId });
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterJavaHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "class_declaration",           17, true), HandleJavaClass);
    set.Register(ts_language_symbol_for_name(lang, "record_declaration",          18, true), HandleJavaClass);
    set.Register(ts_language_symbol_for_name(lang, "interface_declaration",       21, true), HandleJavaInterface);
    set.Register(ts_language_symbol_for_name(lang, "enum_declaration",            16, true), HandleJavaEnum);
    set.Register(ts_language_symbol_for_name(lang, "method_declaration",          18, true), HandleJavaMethod);
    set.Register(ts_language_symbol_for_name(lang, "constructor_declaration",     23, true), HandleJavaMethod);
    set.Register(ts_language_symbol_for_name(lang, "field_declaration",           17, true), HandleJavaField);
    set.Register(ts_language_symbol_for_name(lang, "import_declaration",          18, true), HandleJavaImport);
    set.Register(ts_language_symbol_for_name(lang, "annotation_type_declaration", 27, true), HandleJavaAnnotationType);
}

} // namespace fce::handlers
