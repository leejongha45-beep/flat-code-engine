#include "Baked/Handlers/GoHandlers.hpp"
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
 * Go call: `function` field -> identifier or selector_expression.
 * selector_expression -> "field" field text (e.g., obj.Method -> "Method").
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
                else if (std::strcmp(type, "selector_expression") == 0)
                {
                    TSNode field = ts_node_child_by_field_name(funcNode, "field", 5);
                    if (!ts_node_is_null(field))
                    {
                        auto name = NodeText(field, source);
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
//  HandleGoFunction
// ═══════════════════════════════════════════════════════════════════════════

void HandleGoFunction(TSNode node, std::string_view source,
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

    // Step 2: Signature — from function start to body start, trim trailing { and whitespace
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
        sigId, 0
    });

    // Step 5: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 6: Call relation — Calls
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

    // Step 7: Return type — field "result"
    TSNode retNode = ts_node_child_by_field_name(node, "result", 6);
    if (!ts_node_is_null(retNode))
    {
        const char* retType = ts_node_type(retNode);
        if (std::strcmp(retType, "parameter_list") == 0)
        {
            // Multiple returns: (int, error) — iterate to find type_identifier nodes
            const uint32_t cc = ts_node_child_count(retNode);
            for (uint32_t i = 0; i < cc; ++i)
            {
                TSNode child = ts_node_child(retNode, i);
                if (!ts_node_is_named(child)) continue;
                // Each child may be a parameter_declaration with a type field
                // or directly a type_identifier
                const char* childType = ts_node_type(child);
                if (std::strcmp(childType, "type_identifier") == 0)
                {
                    auto typeText = NodeText(child, source);
                    if (!typeText.empty())
                    {
                        uint32_t tid = pool.Intern(typeText);
                        result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
                    }
                }
                else if (std::strcmp(childType, "parameter_declaration") == 0)
                {
                    TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
                    if (!ts_node_is_null(typeNode) && std::strcmp(ts_node_type(typeNode), "type_identifier") == 0)
                    {
                        auto typeText = NodeText(typeNode, source);
                        if (!typeText.empty())
                        {
                            uint32_t tid = pool.Intern(typeText);
                            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
                        }
                    }
                }
            }
        }
        else if (std::strcmp(retType, "type_identifier") == 0)
        {
            // Single return type
            auto typeText = NodeText(retNode, source);
            if (!typeText.empty())
            {
                uint32_t tid = pool.Intern(typeText);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
            }
        }
    }

    // Step 8: Parameter types — TypedAs
    TSNode paramsNode = ts_node_child_by_field_name(node, "parameters", 10);
    if (!ts_node_is_null(paramsNode))
    {
        const uint32_t cc = ts_node_child_count(paramsNode);
        for (uint32_t i = 0; i < cc; ++i)
        {
            TSNode child = ts_node_child(paramsNode, i);
            if (!ts_node_is_named(child)) continue;
            if (std::strcmp(ts_node_type(child), "parameter_declaration") != 0) continue;

            TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
            if (ts_node_is_null(typeNode)) continue;
            if (std::strcmp(ts_node_type(typeNode), "type_identifier") == 0)
            {
                auto typeText = NodeText(typeNode, source);
                if (!typeText.empty())
                {
                    uint32_t tid = pool.Intern(typeText);
                    result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGoMethod
// ═══════════════════════════════════════════════════════════════════════════

void HandleGoMethod(TSNode node, std::string_view source,
                    uint32_t fileId, uint32_t /*parentId*/,
                    fce::StringPool& pool, fce::BakeResult& result,
                    std::vector<fce::BakeFrame>& /*stack*/)
{
    // Step 1: Name extraction (field_identifier for methods)
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode)) return;

    auto name = NodeText(nameNode, source);
    if (name.empty()) return;
    const uint32_t nameId = pool.Intern(name);

    // Step 2: Signature — from function start to body start, trim trailing { and whitespace
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

    // Step 3: Receiver extraction — field "receiver" -> parameter_list -> parameter_declaration -> type
    uint32_t receiverId = 0;
    TSNode receiverNode = ts_node_child_by_field_name(node, "receiver", 8);
    if (!ts_node_is_null(receiverNode))
    {
        // receiver is a parameter_list, iterate to find parameter_declaration
        const uint32_t rcc = ts_node_child_count(receiverNode);
        for (uint32_t i = 0; i < rcc; ++i)
        {
            TSNode child = ts_node_child(receiverNode, i);
            if (!ts_node_is_named(child)) continue;
            if (std::strcmp(ts_node_type(child), "parameter_declaration") != 0) continue;

            TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
            if (ts_node_is_null(typeNode)) continue;

            const char* typeType = ts_node_type(typeNode);
            if (std::strcmp(typeType, "pointer_type") == 0)
            {
                // *MyStruct -> get child type_identifier
                const uint32_t tcc = ts_node_child_count(typeNode);
                for (uint32_t j = 0; j < tcc; ++j)
                {
                    TSNode inner = ts_node_child(typeNode, j);
                    if (ts_node_is_named(inner) && std::strcmp(ts_node_type(inner), "type_identifier") == 0)
                    {
                        auto recvName = NodeText(inner, source);
                        if (!recvName.empty())
                            receiverId = pool.Intern(recvName);
                        break;
                    }
                }
            }
            else if (std::strcmp(typeType, "type_identifier") == 0)
            {
                auto recvName = NodeText(typeNode, source);
                if (!recvName.empty())
                    receiverId = pool.Intern(recvName);
            }
            break; // Only one receiver in Go
        }
    }

    // Step 4: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 5: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Function,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, receiverId
    });

    // Step 6: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 7: Edge — MemberOf (method -> receiver type)
    if (receiverId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, receiverId, fileId, fce::RelationKind::MemberOf });

    // Step 8: Call relation — Calls
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

    // Step 9: Return type — field "result"
    TSNode retNode = ts_node_child_by_field_name(node, "result", 6);
    if (!ts_node_is_null(retNode))
    {
        const char* retType = ts_node_type(retNode);
        if (std::strcmp(retType, "parameter_list") == 0)
        {
            const uint32_t cc = ts_node_child_count(retNode);
            for (uint32_t i = 0; i < cc; ++i)
            {
                TSNode child = ts_node_child(retNode, i);
                if (!ts_node_is_named(child)) continue;
                const char* childType = ts_node_type(child);
                if (std::strcmp(childType, "type_identifier") == 0)
                {
                    auto typeText = NodeText(child, source);
                    if (!typeText.empty())
                    {
                        uint32_t tid = pool.Intern(typeText);
                        result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
                    }
                }
                else if (std::strcmp(childType, "parameter_declaration") == 0)
                {
                    TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
                    if (!ts_node_is_null(typeNode) && std::strcmp(ts_node_type(typeNode), "type_identifier") == 0)
                    {
                        auto typeText = NodeText(typeNode, source);
                        if (!typeText.empty())
                        {
                            uint32_t tid = pool.Intern(typeText);
                            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
                        }
                    }
                }
            }
        }
        else if (std::strcmp(retType, "type_identifier") == 0)
        {
            auto typeText = NodeText(retNode, source);
            if (!typeText.empty())
            {
                uint32_t tid = pool.Intern(typeText);
                result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
            }
        }
    }

    // Step 10: Parameter types — TypedAs
    TSNode paramsNode = ts_node_child_by_field_name(node, "parameters", 10);
    if (!ts_node_is_null(paramsNode))
    {
        const uint32_t cc = ts_node_child_count(paramsNode);
        for (uint32_t i = 0; i < cc; ++i)
        {
            TSNode child = ts_node_child(paramsNode, i);
            if (!ts_node_is_named(child)) continue;
            if (std::strcmp(ts_node_type(child), "parameter_declaration") != 0) continue;

            TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
            if (ts_node_is_null(typeNode)) continue;
            if (std::strcmp(ts_node_type(typeNode), "type_identifier") == 0)
            {
                auto typeText = NodeText(typeNode, source);
                if (!typeText.empty())
                {
                    uint32_t tid = pool.Intern(typeText);
                    result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGoTypeDecl
// ═══════════════════════════════════════════════════════════════════════════

void HandleGoTypeDecl(TSNode node, std::string_view source,
                      uint32_t fileId, uint32_t /*parentId*/,
                      fce::StringPool& pool, fce::BakeResult& result,
                      std::vector<fce::BakeFrame>& /*stack*/)
{
    // type_declaration may contain one or more type_spec children
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "type_spec") != 0) continue;

        // Extract type name
        TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
        if (ts_node_is_null(nameNode)) continue;

        auto name = NodeText(nameNode, source);
        if (name.empty()) continue;
        const uint32_t nameId = pool.Intern(name);

        // Extract underlying type
        TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
        if (ts_node_is_null(typeNode)) continue;

        const char* underlyingType = ts_node_type(typeNode);
        TSPoint startPt = ts_node_start_point(child);
        TSPoint endPt   = ts_node_end_point(child);

        if (std::strcmp(underlyingType, "struct_type") == 0)
        {
            // ─── Struct ───
            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Struct,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, 0
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

            // Embedded structs (anonymous fields) -> Inherits edge
            TSNode fieldList = ts_node_child_by_field_name(typeNode, "fields", 6);
            if (ts_node_is_null(fieldList))
            {
                // Try iterating children directly for field_declaration_list
                const uint32_t scc = ts_node_child_count(typeNode);
                for (uint32_t j = 0; j < scc; ++j)
                {
                    TSNode sChild = ts_node_child(typeNode, j);
                    if (ts_node_is_named(sChild) && std::strcmp(ts_node_type(sChild), "field_declaration_list") == 0)
                    {
                        fieldList = sChild;
                        break;
                    }
                }
            }

            if (!ts_node_is_null(fieldList))
            {
                const uint32_t fcc = ts_node_child_count(fieldList);
                for (uint32_t j = 0; j < fcc; ++j)
                {
                    TSNode fieldDecl = ts_node_child(fieldList, j);
                    if (!ts_node_is_named(fieldDecl)) continue;
                    if (std::strcmp(ts_node_type(fieldDecl), "field_declaration") != 0) continue;

                    // Check if this is an anonymous/embedded field:
                    // No "name" field but has a "type" field
                    TSNode fieldName = ts_node_child_by_field_name(fieldDecl, "name", 4);
                    if (!ts_node_is_null(fieldName)) continue; // Named field, skip

                    TSNode fieldType = ts_node_child_by_field_name(fieldDecl, "type", 4);
                    if (ts_node_is_null(fieldType)) continue;

                    const char* ftType = ts_node_type(fieldType);
                    std::string_view embeddedName;
                    if (std::strcmp(ftType, "type_identifier") == 0)
                    {
                        embeddedName = NodeText(fieldType, source);
                    }
                    else if (std::strcmp(ftType, "pointer_type") == 0)
                    {
                        // *EmbeddedType
                        const uint32_t ptcc = ts_node_child_count(fieldType);
                        for (uint32_t k = 0; k < ptcc; ++k)
                        {
                            TSNode inner = ts_node_child(fieldType, k);
                            if (ts_node_is_named(inner) && std::strcmp(ts_node_type(inner), "type_identifier") == 0)
                            {
                                embeddedName = NodeText(inner, source);
                                break;
                            }
                        }
                    }
                    else if (std::strcmp(ftType, "qualified_type") == 0)
                    {
                        // pkg.Type — use full text
                        embeddedName = NodeText(fieldType, source);
                    }

                    if (!embeddedName.empty())
                    {
                        uint32_t embId = pool.Intern(embeddedName);
                        result.edges.push_back(fce::BakedEdge{ nameId, embId, fileId, fce::RelationKind::Inherits });
                    }
                }
            }
        }
        else if (std::strcmp(underlyingType, "interface_type") == 0)
        {
            // ─── Interface ───
            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Interface,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, 0
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

            // Embedded interfaces — iterate children of interface body
            // type_identifier or qualified_type = embedded interface -> Inherits
            const uint32_t icc = ts_node_child_count(typeNode);
            for (uint32_t j = 0; j < icc; ++j)
            {
                TSNode iChild = ts_node_child(typeNode, j);
                if (!ts_node_is_named(iChild)) continue;

                const char* iChildType = ts_node_type(iChild);
                if (std::strcmp(iChildType, "type_identifier") == 0 ||
                    std::strcmp(iChildType, "qualified_type") == 0)
                {
                    auto embName = NodeText(iChild, source);
                    if (!embName.empty())
                    {
                        uint32_t embId = pool.Intern(embName);
                        result.edges.push_back(fce::BakedEdge{ nameId, embId, fileId, fce::RelationKind::Inherits });
                    }
                }
            }
        }
        else
        {
            // ─── Alias ───
            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Alias,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, 0
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGoImport
// ═══════════════════════════════════════════════════════════════════════════

void HandleGoImport(TSNode node, std::string_view source,
                    uint32_t fileId, uint32_t /*parentId*/,
                    fce::StringPool& pool, fce::BakeResult& result,
                    std::vector<fce::BakeFrame>& /*stack*/)
{
    // import_declaration may contain a single import_spec or an import_spec_list
    static constexpr uint32_t kMaxImports = 64;

    // Helper lambda to process a single import_spec
    auto processImportSpec = [&](TSNode spec)
    {
        TSNode pathNode = ts_node_child_by_field_name(spec, "path", 4);
        if (ts_node_is_null(pathNode)) return;

        auto pathText = NodeText(pathNode, source);
        if (pathText.empty()) return;

        // Strip quotes from interpreted_string_literal: "fmt" -> fmt
        if (pathText.size() >= 2 && pathText.front() == '"' && pathText.back() == '"')
            pathText = pathText.substr(1, pathText.size() - 2);
        if (pathText.empty()) return;

        const uint32_t pathId = pool.Intern(pathText);
        TSPoint startPt = ts_node_start_point(spec);
        TSPoint endPt   = ts_node_end_point(spec);

        result.symbols.push_back(fce::BakedSymbol{
            pathId, fileId, fce::SymbolKind::Include,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            0, 0
        });

        result.edges.push_back(fce::BakedEdge{ fileId, pathId, fileId, fce::RelationKind::Includes });
    };

    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxImports; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;

        const char* childType = ts_node_type(child);
        if (std::strcmp(childType, "import_spec") == 0)
        {
            processImportSpec(child);
        }
        else if (std::strcmp(childType, "import_spec_list") == 0)
        {
            const uint32_t lcc = ts_node_child_count(child);
            for (uint32_t j = 0; j < lcc && j < kMaxImports; ++j)
            {
                TSNode spec = ts_node_child(child, j);
                if (!ts_node_is_named(spec)) continue;
                if (std::strcmp(ts_node_type(spec), "import_spec") == 0)
                    processImportSpec(spec);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGoConst
// ═══════════════════════════════════════════════════════════════════════════

void HandleGoConst(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t /*parentId*/,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    // const_declaration may contain const_spec children directly or in a block
    static constexpr uint32_t kMaxSpecs = 128;
    const uint32_t cc = ts_node_child_count(node);

    auto processConstSpec = [&](TSNode spec)
    {
        // Find the first identifier child as the name
        const uint32_t scc = ts_node_child_count(spec);
        for (uint32_t j = 0; j < scc; ++j)
        {
            TSNode child = ts_node_child(spec, j);
            if (!ts_node_is_named(child)) continue;
            if (std::strcmp(ts_node_type(child), "identifier") != 0) continue;

            auto name = NodeText(child, source);
            if (name.empty()) continue;
            const uint32_t nameId = pool.Intern(name);

            TSPoint startPt = ts_node_start_point(spec);
            TSPoint endPt   = ts_node_end_point(spec);

            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Variable,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, 0
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
            break; // Only first identifier
        }
    };

    for (uint32_t i = 0; i < cc && i < kMaxSpecs; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "const_spec") == 0)
            processConstSpec(child);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGoVar
// ═══════════════════════════════════════════════════════════════════════════

void HandleGoVar(TSNode node, std::string_view source,
                 uint32_t fileId, uint32_t /*parentId*/,
                 fce::StringPool& pool, fce::BakeResult& result,
                 std::vector<fce::BakeFrame>& /*stack*/)
{
    // var_declaration may contain var_spec children
    static constexpr uint32_t kMaxSpecs = 128;
    const uint32_t cc = ts_node_child_count(node);

    auto processVarSpec = [&](TSNode spec)
    {
        // Find the first identifier child as the name
        const uint32_t scc = ts_node_child_count(spec);
        for (uint32_t j = 0; j < scc; ++j)
        {
            TSNode child = ts_node_child(spec, j);
            if (!ts_node_is_named(child)) continue;
            if (std::strcmp(ts_node_type(child), "identifier") != 0) continue;

            auto name = NodeText(child, source);
            if (name.empty()) continue;
            const uint32_t nameId = pool.Intern(name);

            TSPoint startPt = ts_node_start_point(spec);
            TSPoint endPt   = ts_node_end_point(spec);

            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Variable,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, 0
            });

            result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
            break; // Only first identifier
        }
    };

    for (uint32_t i = 0; i < cc && i < kMaxSpecs; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;
        if (std::strcmp(ts_node_type(child), "var_spec") == 0)
            processVarSpec(child);
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterGoHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "function_declaration", 20, true), HandleGoFunction);
    set.Register(ts_language_symbol_for_name(lang, "method_declaration",   18, true), HandleGoMethod);
    set.Register(ts_language_symbol_for_name(lang, "type_declaration",     16, true), HandleGoTypeDecl);
    set.Register(ts_language_symbol_for_name(lang, "import_declaration",   18, true), HandleGoImport);
    set.Register(ts_language_symbol_for_name(lang, "const_declaration",    17, true), HandleGoConst);
    set.Register(ts_language_symbol_for_name(lang, "var_declaration",      15, true), HandleGoVar);
}

} // namespace fce::handlers
