#include "Baked/Handlers/CppHandlers.hpp"
#include "Baked/Handlers/CommonHandlers.hpp"
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

// ═══════════════════════════════════════════════════════════════════════════
//  HandleClass (C++ class_specifier — includes Inherits edges)
// ═══════════════════════════════════════════════════════════════════════════

void HandleClass(TSNode node, std::string_view source,
                 uint32_t fileId, uint32_t parentId,
                 fce::StringPool& pool, fce::BakeResult& result,
                 std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode))
    {
        // Anonymous class — push only body children onto the stack
        TSNode body = ts_node_child_by_field_name(node, "body", 4);
        if (!ts_node_is_null(body))
        {
            uint32_t cc = ts_node_child_count(body);
            for (uint32_t i = cc; i-- > 0;)
                stack.push_back({ ts_node_child(body, i), parentId });
        }
        return;
    }

    // template_type (MyClass<int>) — extract only the "name" field
    if (std::strcmp(ts_node_type(nameNode), "template_type") == 0)
    {
        TSNode inner = ts_node_child_by_field_name(nameNode, "name", 4);
        if (!ts_node_is_null(inner))
            nameNode = inner;
    }

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

    // Step 4: base_class_clause — Inherits edge
    static constexpr uint32_t kMaxChildren = 256;
    const uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount && i < kMaxChildren; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (std::strcmp(ts_node_type(child), "base_class_clause") != 0) continue;

        const uint32_t bcCount = ts_node_child_count(child);
        for (uint32_t j = 0; j < bcCount; ++j)
        {
            TSNode bcChild = ts_node_child(child, j);
            if (!ts_node_is_named(bcChild)) continue;

            const char* bcType = ts_node_type(bcChild);
            std::string_view baseName;

            if (std::strcmp(bcType, "type_identifier") == 0 ||
                std::strcmp(bcType, "qualified_identifier") == 0)
            {
                baseName = NodeText(bcChild, source);
            }
            else if (std::strcmp(bcType, "template_type") == 0)
            {
                TSNode tn = ts_node_child_by_field_name(bcChild, "name", 4);
                if (!ts_node_is_null(tn))
                    baseName = NodeText(tn, source);
            }

            if (!baseName.empty())
            {
                uint32_t baseId = pool.Intern(baseName);
                result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
            }
        }
        break; // only one base_class_clause
    }

    // Step 5: Body children — parentClassNameId = this class
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t cc = ts_node_child_count(body);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleNamespace
// ═══════════════════════════════════════════════════════════════════════════

void HandleNamespace(TSNode node, std::string_view source,
                     uint32_t fileId, uint32_t /*parentId*/,
                     fce::StringPool& pool, fce::BakeResult& result,
                     std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    std::string_view name;
    if (!ts_node_is_null(nameNode))
    {
        if (std::strcmp(ts_node_type(nameNode), "nested_namespace_specifier") == 0)
        {
            static constexpr uint32_t kMaxNested = 32;
            const uint32_t nc = ts_node_child_count(nameNode);
            for (uint32_t i = nc; i-- > 0;)
            {
                if (i >= kMaxNested) continue;
                TSNode child = ts_node_child(nameNode, i);
                if (ts_node_is_named(child) &&
                    std::strcmp(ts_node_type(child), "namespace_identifier") == 0)
                {
                    name = NodeText(child, source);
                    break;
                }
            }
        }
        else
        {
            name = NodeText(nameNode, source);
        }
    }

    // Step 2: Push body children
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t cc = ts_node_child_count(body);
        for (uint32_t i = cc; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), 0 });
    }

    if (name.empty()) return;

    const uint32_t nameId = pool.Intern(name);

    // Step 3: Emit symbol
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Namespace,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleConcept (C++20)
// ═══════════════════════════════════════════════════════════════════════════

void HandleConcept(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t /*parentId*/,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Concept,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleAlias (using X = Y)
// ═══════════════════════════════════════════════════════════════════════════

void HandleAlias(TSNode node, std::string_view source,
                 uint32_t fileId, uint32_t /*parentId*/,
                 fce::StringPool& pool, fce::BakeResult& result,
                 std::vector<fce::BakeFrame>& /*stack*/)
{
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Alias,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleTemplateDeclaration (pass-through to children — no symbol emitted)
//
//  Wrapper node for template<...> class/struct/function.
//  Skips template_parameter_list and requires_clause,
//  pushes only actual declarations (class_specifier, function_definition, etc.) onto the stack.
// ═══════════════════════════════════════════════════════════════════════════

void HandleTemplateDeclaration(TSNode node, std::string_view /*source*/,
                                uint32_t /*fileId*/, uint32_t parentId,
                                fce::StringPool& /*pool*/, fce::BakeResult& /*result*/,
                                std::vector<fce::BakeFrame>& stack)
{
    static constexpr uint32_t kMaxTemplateChildren = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = cc; i-- > 0;)
    {
        if (i >= kMaxTemplateChildren) continue;
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        const char* ct = ts_node_type(child);
        if (std::strcmp(ct, "template_parameter_list") == 0) continue;
        if (std::strcmp(ct, "requires_clause") == 0) continue;
        stack.push_back({ child, parentId });
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterCppHandlers(HandlerSet& set, TSLanguage* lang)
{
    // Step 1: Register common handlers first
    RegisterCommonHandlers(set, lang);

    // Step 2: Add C++-specific handlers
    set.Register(ts_language_symbol_for_name(lang, "class_specifier", 15, true), HandleClass);
    set.Register(ts_language_symbol_for_name(lang, "namespace_definition", 20, true), HandleNamespace);
    set.Register(ts_language_symbol_for_name(lang, "namespace_alias_definition", 26, true), HandleNamespace);
    set.Register(ts_language_symbol_for_name(lang, "concept_definition", 18, true), HandleConcept);
    set.Register(ts_language_symbol_for_name(lang, "alias_declaration", 17, true), HandleAlias);
    set.Register(ts_language_symbol_for_name(lang, "template_declaration", 20, true), HandleTemplateDeclaration);
}

} // namespace fce::handlers
