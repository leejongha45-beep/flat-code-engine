#include "Baked/Handlers/PythonHandlers.hpp"
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
 * Extract the call target name from a call node.
 *
 * Python call: `function` field -> identifier or attribute.
 * attribute(obj.method) -> "method" (extracts only the last attribute field).
 */
std::string_view ExtractCallName(TSNode callNode, std::string_view source)
{
    TSNode funcNode = ts_node_child_by_field_name(callNode, "function", 8);
    if (ts_node_is_null(funcNode)) return {};

    const char* type = ts_node_type(funcNode);
    if (std::strcmp(type, "identifier") == 0)
        return NodeText(funcNode, source);

    if (std::strcmp(type, "attribute") == 0)
    {
        TSNode attr = ts_node_child_by_field_name(funcNode, "attribute", 9);
        if (!ts_node_is_null(attr))
            return NodeText(attr, source);
    }

    return {};
}

/**
 * Collect all call target names from call nodes in the subtree.
 * Traversed via DFS stack (no recursion — dev-guidelines).
 */
void CollectCallTargets(TSNode root, std::string_view source,
                        std::vector<std::string_view>& out)
{
    std::vector<TSNode> dfs;
    dfs.push_back(root);

    static constexpr uint32_t kMaxIter = 65536;
    for (uint32_t iter = 0; iter < kMaxIter && !dfs.empty(); ++iter)
    {
        TSNode cur = dfs.back();
        dfs.pop_back();

        if (std::strcmp(ts_node_type(cur), "call") == 0)
        {
            auto name = ExtractCallName(cur, source);
            if (!name.empty())
                out.push_back(name);
        }

        const uint32_t cc = ts_node_child_count(cur);
        for (uint32_t i = cc; i-- > 0;)
            dfs.push_back(ts_node_child(cur, i));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandlePyFunctionDef
// ═══════════════════════════════════════════════════════════════════════════

void HandlePyFunctionDef(TSNode node, std::string_view source,
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
                                     sig.back() == ':'))
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

    // Step 6: Edge — MemberOf (function inside a class)
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

    // Step 8: Return type — return_type (Python type hint)
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
//  HandlePyClassDef
// ═══════════════════════════════════════════════════════════════════════════

void HandlePyClassDef(TSNode node, std::string_view source,
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

    // Step 4: Inheritance — superclasses (argument_list) -> Inherits edge
    TSNode superclasses = ts_node_child_by_field_name(node, "superclasses", 12);
    if (!ts_node_is_null(superclasses))
    {
        static constexpr uint32_t kMaxBases = 64;
        const uint32_t cc = ts_node_child_count(superclasses);
        for (uint32_t i = 0; i < cc && i < kMaxBases; ++i)
        {
            TSNode child = ts_node_child(superclasses, i);
            if (!ts_node_is_named(child)) continue;

            const char* childType = ts_node_type(child);

            // identifier (simple inheritance), attribute (module.Class)
            std::string_view baseName;
            if (std::strcmp(childType, "identifier") == 0)
            {
                baseName = NodeText(child, source);
            }
            else if (std::strcmp(childType, "attribute") == 0)
            {
                TSNode attr = ts_node_child_by_field_name(child, "attribute", 9);
                if (!ts_node_is_null(attr))
                    baseName = NodeText(attr, source);
            }

            if (!baseName.empty())
            {
                uint32_t baseId = pool.Intern(baseName);
                result.edges.push_back(fce::BakedEdge{ nameId, baseId, fileId, fce::RelationKind::Inherits });
            }
        }
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
//  HandlePyImport (import x, y)
// ═══════════════════════════════════════════════════════════════════════════

void HandlePyImport(TSNode node, std::string_view source,
                    uint32_t fileId, uint32_t /*parentId*/,
                    fce::StringPool& pool, fce::BakeResult& result,
                    std::vector<fce::BakeFrame>& /*stack*/)
{
    // Iterate over dotted_name / aliased_import children of the import statement
    static constexpr uint32_t kMaxImports = 64;
    const uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && i < kMaxImports; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child)) continue;

        const char* childType = ts_node_type(child);
        std::string_view moduleName;

        if (std::strcmp(childType, "dotted_name") == 0)
        {
            moduleName = NodeText(child, source);
        }
        else if (std::strcmp(childType, "aliased_import") == 0)
        {
            TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
            if (!ts_node_is_null(nameNode))
                moduleName = NodeText(nameNode, source);
        }

        if (moduleName.empty()) continue;

        const uint32_t modId = pool.Intern(moduleName);
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
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandlePyImportFrom (from x import y)
// ═══════════════════════════════════════════════════════════════════════════

void HandlePyImportFrom(TSNode node, std::string_view source,
                        uint32_t fileId, uint32_t /*parentId*/,
                        fce::StringPool& pool, fce::BakeResult& result,
                        std::vector<fce::BakeFrame>& /*stack*/)
{
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    // module_name field -> dotted_name or relative_import
    TSNode modNode = ts_node_child_by_field_name(node, "module_name", 11);
    if (ts_node_is_null(modNode)) return;

    bool bIsRelative = (std::strcmp(ts_node_type(modNode), "relative_import") == 0);

    // Step 1: Register module name (relative imports use full text: ".", "..", "..parent", etc.)
    auto moduleName = NodeText(modNode, source);
    if (!moduleName.empty())
    {
        const uint32_t modId = pool.Intern(moduleName);
        result.symbols.push_back(fce::BakedSymbol{
            modId, fileId, fce::SymbolKind::Include,
            static_cast<int32_t>(startPt.row + 1),
            static_cast<int32_t>(endPt.row + 1),
            0, 0
        });
        result.edges.push_back(fce::BakedEdge{ fileId, modId, fileId, fce::RelationKind::Includes });
    }

    // Step 2: For relative imports, also register name fields as symbols (from . import sibling -> "sibling")
    if (bIsRelative)
    {
        static constexpr uint32_t kMaxNames = 64;
        const uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc && i < kMaxNames; ++i)
        {
            TSNode child = ts_node_child(node, i);
            if (!ts_node_is_named(child)) continue;

            const char* childType = ts_node_type(child);
            std::string_view importedName;

            if (std::strcmp(childType, "dotted_name") == 0 ||
                std::strcmp(childType, "identifier") == 0)
            {
                importedName = NodeText(child, source);
            }
            else if (std::strcmp(childType, "aliased_import") == 0)
            {
                TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                if (!ts_node_is_null(nameNode))
                    importedName = NodeText(nameNode, source);
            }

            // Skip module_name itself since it was already registered
            if (importedName.empty()) continue;
            if (importedName == moduleName) continue;

            const uint32_t nameId = pool.Intern(importedName);
            result.symbols.push_back(fce::BakedSymbol{
                nameId, fileId, fce::SymbolKind::Include,
                static_cast<int32_t>(startPt.row + 1),
                static_cast<int32_t>(endPt.row + 1),
                0, 0
            });
            result.edges.push_back(fce::BakedEdge{ fileId, nameId, fileId, fce::RelationKind::Includes });
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandlePyDecoratedDef (push the definition below @decorator onto the stack)
// ═══════════════════════════════════════════════════════════════════════════

void HandlePyDecoratedDef(TSNode node, std::string_view /*source*/,
                          uint32_t /*fileId*/, uint32_t parentId,
                          fce::StringPool& /*pool*/, fce::BakeResult& /*result*/,
                          std::vector<fce::BakeFrame>& stack)
{
    // definition field -> actual function_definition or class_definition
    TSNode defNode = ts_node_child_by_field_name(node, "definition", 10);
    if (!ts_node_is_null(defNode))
        stack.push_back({ defNode, parentId });
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandlePyAssignment (module/class-level variable)
// ═══════════════════════════════════════════════════════════════════════════

void HandlePyAssignment(TSNode node, std::string_view source,
                        uint32_t fileId, uint32_t parentId,
                        fce::StringPool& pool, fce::BakeResult& result,
                        std::vector<fce::BakeFrame>& /*stack*/)
{
    // left field -> only handle identifier (ignore tuple/subscript, etc.)
    TSNode leftNode = ts_node_child_by_field_name(node, "left", 4);
    if (ts_node_is_null(leftNode)) return;
    if (std::strcmp(ts_node_type(leftNode), "identifier") != 0) return;

    auto name = NodeText(leftNode, source);
    if (name.empty()) return;

    // Skip __xxx__ dunder variables (noise reduction)
    if (name.size() >= 4 && name[0] == '_' && name[1] == '_' &&
        name[name.size() - 1] == '_' && name[name.size() - 2] == '_')
        return;

    const uint32_t nameId = pool.Intern(name);

    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt   = ts_node_end_point(node);

    result.symbols.push_back(fce::BakedSymbol{
        nameId, fileId, fce::SymbolKind::Variable,
        static_cast<int32_t>(startPt.row + 1),
        static_cast<int32_t>(endPt.row + 1),
        0, parentId
    });

    // DefinedIn
    result.edges.push_back(fce::BakedEdge{ nameId, fileId, fileId, fce::RelationKind::DefinedIn });

    // MemberOf (class attribute)
    if (parentId != 0)
        result.edges.push_back(fce::BakedEdge{ nameId, parentId, fileId, fce::RelationKind::MemberOf });
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

namespace fce::handlers
{

void RegisterPythonHandlers(HandlerSet& set, TSLanguage* lang)
{
    set.Register(ts_language_symbol_for_name(lang, "function_definition",   19, true), HandlePyFunctionDef);
    set.Register(ts_language_symbol_for_name(lang, "class_definition",      16, true), HandlePyClassDef);
    set.Register(ts_language_symbol_for_name(lang, "import_statement",      16, true), HandlePyImport);
    set.Register(ts_language_symbol_for_name(lang, "import_from_statement", 21, true), HandlePyImportFrom);
    set.Register(ts_language_symbol_for_name(lang, "decorated_definition",  21, true), HandlePyDecoratedDef);
    set.Register(ts_language_symbol_for_name(lang, "assignment",            10, true), HandlePyAssignment);
}

} // namespace fce::handlers
