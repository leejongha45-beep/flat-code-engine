#include "Baked/Handlers/CommonHandlers.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

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

// ─── C++ declarator utilities ────────────────────────────────────────────────

/**
 * Extract the leaf identifier from a declarator chain.
 * Unwraps wrappers like pointer_declarator / reference_declarator to return the final name.
 */
std::string ExtractNameFromDeclarator(TSNode declarator,
                                      const std::string& source)
{
    TSNode cur = declarator;
    static constexpr int32_t kMaxDepth = 10;
    for (int32_t depth = 0; depth < kMaxDepth && !ts_node_is_null(cur); ++depth)
    {
        std::string_view type = ts_node_type(cur);
        if (type == "identifier"       ||
            type == "type_identifier"  ||
            type == "field_identifier" ||
            type == "operator_name"    ||
            type == "destructor_name")
        {
            const uint32_t s = ts_node_start_byte(cur);
            const uint32_t e = ts_node_end_byte(cur);
            return source.substr(s, e - s);
        }

        if (type == "qualified_identifier")
        {
            TSNode nameField = ts_node_child_by_field_name(cur, "name", 4);
            if (!ts_node_is_null(nameField))
                cur = nameField;
            else
                break;
            continue;
        }

        if (type == "template_function")
        {
            TSNode nameField = ts_node_child_by_field_name(cur, "name", 4);
            if (!ts_node_is_null(nameField))
                cur = nameField;
            else
                break;
            continue;
        }

        if (type == "parenthesized_declarator")
        {
            const uint32_t c = ts_node_child_count(cur);
            bool descended = false;
            for (uint32_t i = 0; i < c; ++i)
            {
                TSNode ch = ts_node_child(cur, i);
                if (ts_node_is_named(ch)) { cur = ch; descended = true; break; }
            }
            if (!descended) break;
            continue;
        }

        TSNode inner = ts_node_child_by_field_name(cur, "declarator", 10);
        if (!ts_node_is_null(inner))
            cur = inner;
        else
            break;
    }

    return "";
}

/**
 * Check whether a declaration node contains a function_declarator.
 * Used to distinguish variable declarations from function declarations.
 */
bool HasFunctionDeclarator(TSNode declNode)
{
    TSNode cur = ts_node_child_by_field_name(declNode, "declarator", 10);

    static constexpr int32_t kMaxDepth = 6;
    for (int32_t depth = 0; depth < kMaxDepth && !ts_node_is_null(cur); ++depth)
    {
        if (std::string_view(ts_node_type(cur)) == "function_declarator")
            return true;

        TSNode inner = ts_node_child_by_field_name(cur, "declarator", 10);
        cur = inner;
    }

    return false;
}

/**
 * Extract the parent class name from the scope node of a qualified_identifier.
 * - template_type (Bar<float>) -> "Bar"  (strip angle brackets)
 * - namespace_identifier / qualified_identifier -> last component
 */
std::string ExtractScopeClass(TSNode scopeNode, const std::string& source)
{
    if (ts_node_is_null(scopeNode)) return "";

    if (std::string_view(ts_node_type(scopeNode)) == "template_type")
    {
        TSNode nameField = ts_node_child_by_field_name(scopeNode, "name", 4);
        if (!ts_node_is_null(nameField))
        {
            const uint32_t s = ts_node_start_byte(nameField);
            const uint32_t e = ts_node_end_byte(nameField);
            return source.substr(s, e - s);
        }
        return "";
    }

    const uint32_t s = ts_node_start_byte(scopeNode);
    const uint32_t e = ts_node_end_byte(scopeNode);
    std::string scope = source.substr(s, e - s);
    const size_t pos  = scope.rfind("::");
    return pos != std::string::npos ? scope.substr(pos + 2) : scope;
}

/**
 * Collect all type_identifiers under an AST node.
 * Explicit stack DFS, automatically excludes primitive_type, deduplicates results.
 */
std::vector<std::string> ExtractTypeIdentifiers(TSNode node,
                                                 const std::string& source)
{
    std::vector<std::string> types;
    if (ts_node_is_null(node)) return types;

    std::vector<TSNode> stack;
    stack.reserve(64);
    stack.push_back(node);

    static constexpr int32_t kMaxIterations = 500;

    for (int32_t iter = 0; iter < kMaxIterations && !stack.empty(); ++iter)
    {
        const TSNode current = stack.back();
        stack.pop_back();

        const std::string_view type = ts_node_type(current);

        if (type == "type_identifier")
        {
            const uint32_t s = ts_node_start_byte(current);
            const uint32_t e = ts_node_end_byte(current);
            types.push_back(source.substr(s, e - s));
            continue;
        }

        const uint32_t childCount = ts_node_child_count(current);
        for (uint32_t i = childCount; i > 0; --i)
        {
            TSNode child = ts_node_child(current, i - 1);
            if (ts_node_is_named(child))
                stack.push_back(child);
        }
    }

    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

/**
 * Traverse the function body AST to extract call target function names.
 * Iterative traversal using an explicit stack, deduplicates results.
 */
std::vector<std::string> ExtractCallTargets(TSNode bodyNode,
                                             const std::string& source)
{
    std::vector<std::string> targets;
    if (ts_node_is_null(bodyNode)) return targets;

    std::vector<TSNode> stack;
    stack.reserve(256);
    stack.push_back(bodyNode);

    static constexpr int32_t kMaxIterations = 10000;

    for (int32_t iter = 0; iter < kMaxIterations && !stack.empty(); ++iter)
    {
        const TSNode current = stack.back();
        stack.pop_back();

        const std::string_view type = ts_node_type(current);

        if (type == "call_expression")
        {
            TSNode funcNode = ts_node_child_by_field_name(current, "function", 8);
            if (!ts_node_is_null(funcNode))
            {
                const std::string_view funcType = ts_node_type(funcNode);
                std::string name;

                if (funcType == "identifier")
                {
                    const uint32_t s = ts_node_start_byte(funcNode);
                    const uint32_t e = ts_node_end_byte(funcNode);
                    name = source.substr(s, e - s);
                }
                else if (funcType == "field_expression")
                {
                    TSNode field = ts_node_child_by_field_name(funcNode, "field", 5);
                    if (!ts_node_is_null(field))
                    {
                        const uint32_t s = ts_node_start_byte(field);
                        const uint32_t e = ts_node_end_byte(field);
                        name = source.substr(s, e - s);
                    }
                }
                else if (funcType == "qualified_identifier")
                {
                    TSNode nameField = ts_node_child_by_field_name(funcNode, "name", 4);
                    if (!ts_node_is_null(nameField))
                    {
                        const uint32_t s = ts_node_start_byte(nameField);
                        const uint32_t e = ts_node_end_byte(nameField);
                        name = source.substr(s, e - s);
                    }
                }

                if (!name.empty())
                    targets.push_back(std::move(name));
            }
        }

        const uint32_t childCount = ts_node_child_count(current);
        for (uint32_t i = childCount; i > 0; --i)
        {
            TSNode child = ts_node_child(current, i - 1);
            if (ts_node_is_named(child))
                stack.push_back(child);
        }
    }

    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleFunctionDef
// ═══════════════════════════════════════════════════════════════════════════

void HandleFunctionDef(TSNode node, std::string_view source,
                       uint32_t fileId, uint32_t parentId,
                       fce::StringPool& pool, fce::BakeResult& result,
                       std::vector<fce::BakeFrame>& /*stack*/)
{
    // Step 1: Walk the declarator chain to find function_declarator
    TSNode declNode = ts_node_child_by_field_name(node, "declarator", 10);
    if (ts_node_is_null(declNode)) return;

    TSNode funcDecl = declNode;
    static constexpr int32_t kMaxDeclDepth = 8;
    for (int32_t depth = 0; depth < kMaxDeclDepth; ++depth)
    {
        if (std::strcmp(ts_node_type(funcDecl), "function_declarator") == 0)
            break;
        TSNode child = ts_node_child_by_field_name(funcDecl, "declarator", 10);
        if (ts_node_is_null(child)) break;
        funcDecl = child;
    }
    if (std::strcmp(ts_node_type(funcDecl), "function_declarator") != 0)
        return;

    // Step 2: Name extraction
    TSNode nameDeclNode = ts_node_child_by_field_name(funcDecl, "declarator", 10);
    std::string nameStr = ExtractNameFromDeclarator(
        nameDeclNode, std::string(source));
    if (nameStr.empty()) return;

    const uint32_t nameId = pool.Intern(nameStr);

    // Step 3: Out-of-class definition (Foo::bar) — extract parent class from scope
    uint32_t effectiveParent = parentId;
    if (effectiveParent == 0 && !ts_node_is_null(nameDeclNode) &&
        std::string_view(ts_node_type(nameDeclNode)) == "qualified_identifier")
    {
        TSNode scopeNode = ts_node_child_by_field_name(nameDeclNode, "scope", 5);
        std::string scopeClass = ExtractScopeClass(scopeNode, std::string(source));
        if (!scopeClass.empty())
            effectiveParent = pool.Intern(scopeClass);
    }

    // Step 4: Signature — from function start to body start
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
                                     sig.back() == '\r' || sig.back() == '\t'))
                sig.remove_suffix(1);
            sigId = pool.Intern(sig);
        }
    }

    // Step 5: Line information
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // Step 6: Emit symbol
    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Function,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        sigId, effectiveParent
    });

    // Step 7: Edge — DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // Step 8: Edge — MemberOf
    if (effectiveParent != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, effectiveParent, fileId, fce::RelationKind::MemberOf });

    // Step 9: Call relation — Calls
    if (!ts_node_is_null(bodyNode))
    {
        auto calls = ExtractCallTargets(bodyNode, std::string(source));
        for (auto& callee : calls)
        {
            uint32_t calleeId = pool.Intern(callee);
            result.edges.push_back(fce::BakedEdge{ nameId, calleeId, fileId, fce::RelationKind::Calls });
        }
    }

    // Step 10: Return type — Returns
    TSNode typeNode = ts_node_child_by_field_name(node, "type", 4);
    if (!ts_node_is_null(typeNode))
    {
        auto types = ExtractTypeIdentifiers(typeNode, std::string(source));
        for (auto& t : types)
        {
            uint32_t tid = pool.Intern(t);
            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::Returns });
        }
    }

    // Step 11: Parameter type — TypedAs
    TSNode params = ts_node_child_by_field_name(funcDecl, "parameters", 10);
    if (!ts_node_is_null(params))
    {
        auto types = ExtractTypeIdentifiers(params, std::string(source));
        for (auto& t : types)
        {
            uint32_t tid = pool.Intern(t);
            result.edges.push_back(fce::BakedEdge{ nameId, tid, fileId, fce::RelationKind::TypedAs });
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleStruct (struct / union)
// ═══════════════════════════════════════════════════════════════════════════

void HandleStruct(TSNode node, std::string_view source,
                  uint32_t fileId, uint32_t parentId,
                  fce::StringPool& pool, fce::BakeResult& result,
                  std::vector<fce::BakeFrame>& stack)
{
    // Step 1: Name extraction
    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(nameNode))
    {
        // Anonymous struct — push only body children onto the stack
        TSNode body = ts_node_child_by_field_name(node, "body", 4);
        if (!ts_node_is_null(body))
        {
            uint32_t cc = ts_node_child_count(body);
            for (uint32_t i = cc; i-- > 0;)
                stack.push_back({ ts_node_child(body, i), parentId });
        }
        return;
    }

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

    // Step 4: base_class_clause — Inherits edge (C++ struct inheritance)
    {
        static constexpr uint32_t kMaxBC = 256;
        const uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc && i < kMaxBC; ++i)
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
                    TSNode inner = ts_node_child_by_field_name(bcChild, "name", 4);
                    if (!ts_node_is_null(inner))
                        baseName = NodeText(inner, source);
                }

                if (!baseName.empty())
                {
                    uint32_t baseId = pool.Intern(baseName);
                    result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
                }
            }
        }
    }

    // Step 5: Push body children with parentClass = this struct's name
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t cc2 = ts_node_child_count(body);
        for (uint32_t i = cc2; i-- > 0;)
            stack.push_back({ ts_node_child(body, i), nameId });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleEnum
// ═══════════════════════════════════════════════════════════════════════════

void HandleEnum(TSNode node, std::string_view source,
                uint32_t fileId, uint32_t parentId,
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
        nameId, fileId, fce::SymbolKind::Enum,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleMacro (#define)
// ═══════════════════════════════════════════════════════════════════════════

void HandleMacro(TSNode node, std::string_view source,
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
        nameId, fileId, fce::SymbolKind::Macro,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleInclude (#include)
// ═══════════════════════════════════════════════════════════════════════════

void HandleInclude(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t /*parentId*/,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& /*stack*/)
{
    TSNode pathNode = ts_node_child_by_field_name(node, "path", 4);
    if (ts_node_is_null(pathNode)) return;

    auto pathText = NodeText(pathNode, source);
    if (pathText.size() < 3) return;

    // Strip quotes/angle brackets
    if ((pathText.front() == '"' && pathText.back() == '"') ||
        (pathText.front() == '<' && pathText.back() == '>'))
    {
        pathText = pathText.substr(1, pathText.size() - 2);
    }
    if (pathText.empty()) return;

    const uint32_t pathId = pool.Intern(pathText);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        pathId, fileId, fce::SymbolKind::Include,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, 0
    });

    result.edges.push_back(fce::BakedEdge{ fileId, pathId, fileId, fce::RelationKind::Includes });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleTypedef
// ═══════════════════════════════════════════════════════════════════════════

void HandleTypedef(TSNode node, std::string_view source,
                   uint32_t fileId, uint32_t parentId,
                   fce::StringPool& pool, fce::BakeResult& result,
                   std::vector<fce::BakeFrame>& stack)
{
    TSNode declNode = ts_node_child_by_field_name(node, "declarator", 10);
    if (ts_node_is_null(declNode)) return;

    std::string nameStr = ExtractNameFromDeclarator(
        declNode, std::string(source));
    if (nameStr.empty()) return;

    const uint32_t nameId = pool.Intern(nameStr);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Alias,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // If a nested struct/union/enum exists, push it onto the stack
    TSNode typeNode = ts_node_child_by_field_name(node, "type", 4);
    if (!ts_node_is_null(typeNode))
    {
        const char* tt = ts_node_type(typeNode);
        if (std::strcmp(tt, "struct_specifier") == 0 ||
            std::strcmp(tt, "union_specifier") == 0 ||
            std::strcmp(tt, "enum_specifier") == 0)
        {
            stack.push_back({ typeNode, parentId });
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleDeclaration (function declaration / variable declaration)
// ═══════════════════════════════════════════════════════════════════════════

void HandleDeclaration(TSNode node, std::string_view source,
                       uint32_t fileId, uint32_t parentId,
                       fce::StringPool& pool, fce::BakeResult& result,
                       std::vector<fce::BakeFrame>& /*stack*/)
{
    bool bIsFunc = HasFunctionDeclarator(node);

    TSNode declNode = ts_node_child_by_field_name(node, "declarator", 10);
    if (ts_node_is_null(declNode)) return;

    if (bIsFunc)
    {
        // Case 1: Function declaration (prototype)
        TSNode funcDecl = declNode;
        static constexpr int32_t kMaxDeclDepth = 6;
        for (int32_t depth = 0; depth < kMaxDeclDepth; ++depth)
        {
            const char* t = ts_node_type(funcDecl);
            if (std::strcmp(t, "function_declarator") == 0) break;
            TSNode child = ts_node_child_by_field_name(funcDecl, "declarator", 10);
            if (ts_node_is_null(child)) break;
            funcDecl = child;
        }
        if (std::strcmp(ts_node_type(funcDecl), "function_declarator") != 0)
            return;

        TSNode nameDeclNode = ts_node_child_by_field_name(funcDecl, "declarator", 10);
        if (ts_node_is_null(nameDeclNode)) return;

        std::string nameStr = ExtractNameFromDeclarator(
            nameDeclNode, std::string(source));
        if (nameStr.empty()) return;

        const uint32_t nameId = pool.Intern(nameStr);

        // Signature: full declaration text (excluding semicolon)
        auto fullText = NodeText(node, source);
        if (!fullText.empty() && fullText.back() == ';')
            fullText.remove_suffix(1);
        uint32_t sigId = pool.Intern(fullText);

        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt   = ts_node_end_point(node);

        result.symbols.push_back(fce::BakedSymbol{
            nameId, fileId, fce::SymbolKind::Function,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            sigId, parentId
        });

        result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

        if (parentId != 0)
            result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
    }
    else
    {
        // Case 2: Variable declaration — handle multiple declarators (e.g. float x, y, z;)
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt   = ts_node_end_point(node);
        const std::string srcStr(source);

        const uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(node, i);
            if (!ts_node_is_named(child)) continue;

            const char* childType = ts_node_type(child);
            // Skip type specifiers and storage qualifiers
            if (std::strcmp(childType, "primitive_type") == 0 ||
                std::strcmp(childType, "sized_type_specifier") == 0 ||
                std::strcmp(childType, "type_identifier") == 0 ||
                std::strcmp(childType, "qualified_identifier") == 0 ||
                std::strcmp(childType, "template_type") == 0 ||
                std::strcmp(childType, "storage_class_specifier") == 0 ||
                std::strcmp(childType, "type_qualifier") == 0 ||
                std::strcmp(childType, "struct_specifier") == 0 ||
                std::strcmp(childType, "union_specifier") == 0 ||
                std::strcmp(childType, "enum_specifier") == 0 ||
                std::strcmp(childType, "placeholder_type_specifier") == 0)
                continue;

            std::string nameStr = ExtractNameFromDeclarator(child, srcStr);
            if (nameStr.empty()) continue;

            const uint32_t nameId = pool.Intern(nameStr);

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
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterCommonHandlers(HandlerSet& set, TSLanguage* lang)
{
    // struct / union
    set.Register(ts_language_symbol_for_name(lang, "struct_specifier", 16, true), HandleStruct);
    set.Register(ts_language_symbol_for_name(lang, "union_specifier",  15, true), HandleStruct);

    // function
    set.Register(ts_language_symbol_for_name(lang, "function_definition", 19, true), HandleFunctionDef);

    // enum
    set.Register(ts_language_symbol_for_name(lang, "enum_specifier", 14, true), HandleEnum);

    // macro
    set.Register(ts_language_symbol_for_name(lang, "preproc_def", 11, true), HandleMacro);
    set.Register(ts_language_symbol_for_name(lang, "preproc_function_def", 20, true), HandleMacro);

    // include
    set.Register(ts_language_symbol_for_name(lang, "preproc_include", 15, true), HandleInclude);

    // typedef
    set.Register(ts_language_symbol_for_name(lang, "type_definition", 15, true), HandleTypedef);

    // declaration (function prototype / variable)
    set.Register(ts_language_symbol_for_name(lang, "declaration", 11, true), HandleDeclaration);
}

} // namespace fce::handlers
