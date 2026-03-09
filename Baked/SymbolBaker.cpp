#include "Baked/SymbolBaker.hpp"

namespace fce
{

// ════════════════════════════════════════════════════════════════════════════
//  BakeFile — Main DFS (explicit stack, no recursion)
//
//  Dispatch: m_possessed->table[ts_node_symbol(node)]
//  - Registered handler -> invoke (including template_declaration)
//  - nullptr -> continue traversing children
// ════════════════════════════════════════════════════════════════════════════

BakeResult SymbolBaker::BakeFile(
    TSNode root,
    std::string_view source,
    std::string_view filePath,
    StringPool& pool) const
{
    BakeResult result;

    const uint32_t filePathId = pool.Intern(filePath);

    // Step 1: Register file symbol
    result.symbols.push_back(BakedSymbol{
        filePathId, filePathId, SymbolKind::File, 0, 0, 0, 0
    });

    // Step 2: Explicit stack DFS
    std::vector<BakeFrame> stack;
    const uint32_t childCount = ts_node_child_count(root);
    for (uint32_t i = childCount; i-- > 0;)
        stack.push_back({ ts_node_child(root, i), 0 });

    static constexpr size_t kMaxIterations = 2'000'000;  // TODO: Log when upper bound is reached (after logging system is built)
    size_t iterations = 0;

    while (!stack.empty() && iterations++ < kMaxIterations)
    {
        BakeFrame frame = stack.back();
        stack.pop_back();

        const TSSymbol sym = ts_node_symbol(frame.node);

        // Step 3: Function pointer table dispatch (O(1))
        HandleFn fn = m_possessed ? (*m_possessed)[sym] : nullptr;

        if (fn)
        {
            fn(frame.node, source, filePathId,
               frame.parentClassNameId, pool, result, stack);
        }
        else
        {
            // Unregistered node — continue traversing children
            const uint32_t cc = ts_node_child_count(frame.node);
            for (uint32_t i = cc; i-- > 0;)
                stack.push_back({ ts_node_child(frame.node, i),
                                  frame.parentClassNameId });
        }
    }

    return result;
}

} // namespace fce
