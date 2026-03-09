#include "Baked/Handlers/RustHandlers.hpp"
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
 * Traversed via DFS stack (no recursion — dev-guidelines).
 *
 * Rust call_expression: "function" field ->
 *   identifier      -> direct text
 *   field_expression -> "field" field text (obj.method)
 *   scoped_identifier -> "name" field text (path::func)
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
                std::string_view name;

                if (std::strcmp(type, "identifier") == 0)
                {
                    name = NodeText(funcNode, source);
                }
                else if (std::strcmp(type, "field_expression") == 0)
                {
                    TSNode field = ts_node_child_by_field_name(funcNode, "field", 5);
                    if (!ts_node_is_null(field))
                        name = NodeText(field, source);
                }
                else if (std::strcmp(type, "scoped_identifier") == 0)
                {
                    TSNode nameNode = ts_node_child_by_field_name(funcNode, "name", 4);
                    if (!ts_node_is_null(nameNode))
                        name = NodeText(nameNode, source);
                }

                if (!name.empty())
                    out.push_back(name);
            }
        }

        const uint32_t cc = ts_node_child_count(cur);
        for (uint32_t i = cc; i-- > 0;)
            dfs.push_back(ts_node_child(cur, i));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustFunction — function_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustFunction(TSNode node, std::string_view source,
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

    // Step 2: Signature — from function start to body start, trim trailing '{' and whitespace
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

    // Step 6: Edge — MemberOf (function inside impl/trait block)
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

    // Step 8: Return type — field "return_type", find first type_identifier in subtree
    TSNode retType = ts_node_child_by_field_name(node, "return_type", 11);
    if (!ts_node_is_null(retType))
    {
        // DFS to find the first type_identifier in the return type subtree
        std::vector<TSNode> rtDfs;
        rtDfs.push_back(retType);
        static constexpr uint32_t kMaxRetIter = 256;
        for (uint32_t ri = 0; ri < kMaxRetIter && !rtDfs.empty(); ++ri)
        {
            TSNode cur = rtDfs.back();
            rtDfs.pop_back();

            if (std::strcmp(ts_node_type(cur), "type_identifier") == 0)
            {
                auto typeText = NodeText(cur, source);
                if (!typeText.empty())
                {
                    uint32_t tid = pool.Intern(typeText);
                    result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
                }
                break;
            }

            const uint32_t cc = ts_node_child_count(cur);
            for (uint32_t i = cc; i-- > 0;)
                rtDfs.push_back(ts_node_child(cur, i));
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
            TSNode child = ts_node_child(params, i);
            if (!ts_node_is_named(child)) continue;
            if (std::strcmp(ts_node_type(child), "parameter") != 0) continue;

            TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
            if (ts_node_is_null(typeNode)) continue;

            // Find first type_identifier in the type subtree
            std::vector<TSNode> typeDfs;
            typeDfs.push_back(typeNode);
            static constexpr uint32_t kMaxTypeIter = 256;
            for (uint32_t ti = 0; ti < kMaxTypeIter && !typeDfs.empty(); ++ti)
            {
                TSNode cur = typeDfs.back();
                typeDfs.pop_back();

                if (std::strcmp(ts_node_type(cur), "type_identifier") == 0)
                {
                    auto typeText = NodeText(cur, source);
                    if (!typeText.empty())
                    {
                        uint32_t tid = pool.Intern(typeText);
                        result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
                    }
                    break;
                }

                const uint32_t tc = ts_node_child_count(cur);
                for (uint32_t j = tc; j-- > 0;)
                    typeDfs.push_back(ts_node_child(cur, j));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustStruct — struct_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustStruct(TSNode node, std::string_view source,
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
        nameId, fileId, fce::SymbolKind::Struct,
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
//  HandleRustEnum — enum_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustEnum(TSNode node, std::string_view source,
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
//  HandleRustTrait — trait_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustTrait(TSNode node, std::string_view source,
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

    // Step 5: Supertrait bounds — trait Foo: Bar + Baz → Inherits edges
    static constexpr uint32_t kMaxChildren = 64;
    const uint32_t nc = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < nc && i < kMaxChildren; ++i)
    {
        TSNode child = ts_node_named_child(node, i);
        if (std::strcmp(ts_node_type(child), "trait_bounds") == 0)
        {
            const uint32_t bc = ts_node_named_child_count(child);
            for (uint32_t j = 0; j < bc && j < kMaxChildren; ++j)
            {
                TSNode bound = ts_node_named_child(child, j);
                if (std::strcmp(ts_node_type(bound), "type_identifier") == 0)
                {
                    auto baseName = NodeText(bound, source);
                    if (!baseName.empty())
                    {
                        uint32_t baseId = pool.Intern(baseName);
                        result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
                    }
                }
            }
            break;
        }
    }

    // Step 6: Body children — push onto stack with parentId = this trait
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t cc = ts_node_child_count(body);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustImpl — impl_item (pass-through, emits NO symbol)
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustImpl(TSNode node, std::string_view source,
                    uint32_t fileId, uint32_t /*parentId*/,
                    fce::StringPool& pool, fce::BakeResult& result,
                    std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Extract target type name
    TSNode typeNode = ts_node_child_by_field_name(node, "type", 4);
    if (ts_node_is_null(typeNode)) return;

    std::string_view typeName;
    const char* typeType = ts_node_type(typeNode);

    if (std::strcmp(typeType, "type_identifier") == 0)
    {
        typeName = NodeText(typeNode, source);
    }
    else if (std::strcmp(typeType, "generic_type") == 0)
    {
        TSNode innerType = ts_node_child_by_field_name(typeNode, "type", 4);
        if (!ts_node_is_null(innerType))
            typeName = NodeText(innerType, source);
    }
    else if (std::strcmp(typeType, "scoped_type_identifier") == 0)
    {
        TSNode nameNode = ts_node_child_by_field_name(typeNode, "name", 4);
        if (!ts_node_is_null(nameNode))
            typeName = NodeText(nameNode, source);
    }

    if (typeName.empty()) return;
    const uint32_t typeNameId = pool.Intern(typeName);

    // Step 2: Trait impl — impl Trait for Type → Inherits edge
    TSNode traitNode = ts_node_child_by_field_name(node, "trait", 5);
    if (!ts_node_is_null(traitNode))
    {
        std::string_view traitName;
        const char* traitType = ts_node_type(traitNode);

        if (std::strcmp(traitType, "type_identifier") == 0)
        {
            traitName = NodeText(traitNode, source);
        }
        else if (std::strcmp(traitType, "scoped_identifier") == 0)
        {
            TSNode nameNode = ts_node_child_by_field_name(traitNode, "name", 4);
            if (!ts_node_is_null(nameNode))
                traitName = NodeText(nameNode, source);
        }
        else if (std::strcmp(traitType, "generic_type") == 0)
        {
            TSNode innerType = ts_node_child_by_field_name(traitNode, "type", 4);
            if (!ts_node_is_null(innerType))
                traitName = NodeText(innerType, source);
        }

        if (!traitName.empty())
        {
            uint32_t traitId = pool.Intern(traitName);
            result.edges.push_back(fce::BakedEdge{ typeNameId, traitId, fileId, fce::RelationKind::Inherits });
        }
    }

    // Step 3: Push body children with parentId = target type
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t cc = ts_node_child_count(body);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), typeNameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustMod — mod_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustMod(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t /*parentId*/,
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
        nameId, fileId, fce::SymbolKind::Namespace,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    // Step 3: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 4: Body children — inline module (mod foo { ... })
    // Namespace resets context (parentId = 0), same as C++
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t cc = ts_node_child_count(body);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), 0 });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustUse — use_declaration
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustUse(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t /*parentId*/,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    // Find the first named child that isn't a visibility modifier
    const uint32_t nc = ts_node_named_child_count(node);
    TSNode useArg = { {0}, nullptr, nullptr };
    for (uint32_t i = 0; i < nc; ++i)
    {
        TSNode child = ts_node_named_child(node, i);
        if (std::strcmp(ts_node_type(child), "visibility_modifier") != 0)
        {
            useArg = child;
            break;
        }
    }
    if (ts_node_is_null(useArg)) return;

    auto useText = NodeText(useArg, source);
    if (useText.empty()) return;

    const uint32_t useId = pool.Intern(useText);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        useId, fileId, fce::SymbolKind::Include,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ fileId, useId, fileId, fce::RelationKind::Includes });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustTypeAlias — type_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustTypeAlias(TSNode node, std::string_view source,
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
        nameId, fileId, fce::SymbolKind::Alias,
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
//  HandleRustConst — const_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustConst(TSNode node, std::string_view source,
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
        nameId, fileId, fce::SymbolKind::Variable,
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
//  HandleRustStatic — static_item
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustStatic(TSNode node, std::string_view source,
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
        nameId, fileId, fce::SymbolKind::Variable,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    // Step 3: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleRustMacro — macro_definition
// ═══════════════════════════════════════════════════════════════════════════

void HandleRustMacro(TSNode node, std::string_view source,
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
        nameId, fileId, fce::SymbolKind::Macro,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    // Step 3: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterRustHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "function_item",      13, true), HandleRustFunction);
    set.Register(ts_language_symbol_for_name(lang, "struct_item",        11, true), HandleRustStruct);
    set.Register(ts_language_symbol_for_name(lang, "enum_item",           9, true), HandleRustEnum);
    set.Register(ts_language_symbol_for_name(lang, "trait_item",         10, true), HandleRustTrait);
    set.Register(ts_language_symbol_for_name(lang, "impl_item",           9, true), HandleRustImpl);
    set.Register(ts_language_symbol_for_name(lang, "mod_item",            8, true), HandleRustMod);
    set.Register(ts_language_symbol_for_name(lang, "use_declaration",    15, true), HandleRustUse);
    set.Register(ts_language_symbol_for_name(lang, "type_item",           9, true), HandleRustTypeAlias);
    set.Register(ts_language_symbol_for_name(lang, "const_item",         10, true), HandleRustConst);
    set.Register(ts_language_symbol_for_name(lang, "static_item",        11, true), HandleRustStatic);
    set.Register(ts_language_symbol_for_name(lang, "macro_definition",   16, true), HandleRustMacro);
}

} // namespace fce::handlers
