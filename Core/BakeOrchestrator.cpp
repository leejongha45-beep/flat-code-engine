#include "Core/BakeOrchestrator.hpp"
#include "Baked/Preprocessor.hpp"
#include "Baked/SymbolBaker.hpp"
#include "Baked/Handlers/CommonHandlers.hpp"
#include "Baked/Handlers/CppHandlers.hpp"
#include "Baked/Handlers/PythonHandlers.hpp"
#include "Baked/Handlers/JavaHandlers.hpp"
#include "Baked/Handlers/JsTsHandlers.hpp"
#include "Baked/Handlers/RustHandlers.hpp"
#include "Baked/Handlers/GoHandlers.hpp"
#include "Baked/Handlers/CSharpHandlers.hpp"
#include "Baked/System/BakeSystem.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

// tree-sitter grammars
extern "C" { TSLanguage* tree_sitter_c(); }
extern "C" { TSLanguage* tree_sitter_cpp(); }
extern "C" { TSLanguage* tree_sitter_python(); }
extern "C" { TSLanguage* tree_sitter_java(); }
extern "C" { TSLanguage* tree_sitter_javascript(); }
extern "C" { TSLanguage* tree_sitter_typescript(); }
extern "C" { TSLanguage* tree_sitter_tsx(); }
extern "C" { TSLanguage* tree_sitter_rust(); }
extern "C" { TSLanguage* tree_sitter_go(); }
extern "C" { TSLanguage* tree_sitter_c_sharp(); }

// ─── File-local helpers ──────────────────────────────────────────────────────

namespace
{

/** Per-thread independent buffer (zero store access) */
struct ThreadLocal
{
    fce::StringPool pool;
    std::vector<fce::BakeResult> results;
};

/**
 * POD → store load (FlushSpawnQueue pattern).
 *
 * [MainThread] Called on a single thread after join.
 * 1. Reserve → 2. thread-local ID → global ID re-intern → store load → 3. RebuildIndices
 *
 * @param locals      ThreadLocal array filled by worker threads
 * @param store       Global SymbolStore
 * @param globalPool  Global StringPool
 * @return Number of processed files
 */
int FlushToStore(std::vector<ThreadLocal>& locals,
                 fce::SymbolStore& store,
                 fce::StringPool& globalPool)
{
    // 1. Total symbol/edge count → Reserve
    size_t totalSyms = 0, totalEdges = 0;
    for (const auto& tl : locals)
        for (const auto& br : tl.results)
        {
            totalSyms  += br.symbols.size();
            totalEdges += br.edges.size();
        }
    store.Reserve(totalSyms, totalEdges);

    // 2. Flush: thread-local ID → global ID re-intern → store load
    int processed = 0;
    for (auto& tl : locals)
    {
        const size_t poolSize = tl.pool.BytesUsed();

        for (auto& br : tl.results)
        {
            // Symbol Flush
            for (const auto& sym : br.symbols)
            {
                if (sym.nameId >= poolSize || sym.fileId >= poolSize)
                    continue;

                fce::BakedSymbol resolved;
                resolved.nameId    = globalPool.Intern(tl.pool.Resolve(sym.nameId));
                resolved.fileId    = globalPool.Intern(tl.pool.Resolve(sym.fileId));
                resolved.kind      = sym.kind;
                resolved.startLine = sym.startLine;
                resolved.endLine   = sym.endLine;
                resolved.sigId     = sym.sigId    ? globalPool.Intern(tl.pool.Resolve(sym.sigId))     : 0;
                resolved.parentId  = sym.parentId ? globalPool.Intern(tl.pool.Resolve(sym.parentId)) : 0;

                fce::BakeSystem::CreateSymbol(store, resolved);
            }

            // Edge Flush
            for (const auto& edge : br.edges)
            {
                if (edge.fromNameId >= poolSize ||
                    edge.toNameId   >= poolSize ||
                    edge.fileId     >= poolSize)
                    continue;

                fce::BakedEdge resolved;
                resolved.fromNameId = globalPool.Intern(tl.pool.Resolve(edge.fromNameId));
                resolved.toNameId   = globalPool.Intern(tl.pool.Resolve(edge.toNameId));
                resolved.fileId     = globalPool.Intern(tl.pool.Resolve(edge.fileId));
                resolved.relation   = edge.relation;

                fce::BakeSystem::CreateEdge(store, resolved);
            }

            ++processed;
        }
    }

    // 3. Build triple index (name, from, to)
    store.RebuildIndices();
    return processed;
}

} // anonymous namespace

// ─── BakeOrchestrator implementation ────────────────────────────────────────

namespace fce
{

void BakeOrchestrator::Initialize()
{
    // 1. C table: common handlers only
    handlers::RegisterCommonHandlers(m_cSet, tree_sitter_c());

    // 2. C++ table: common + C++-specific
    handlers::RegisterCppHandlers(m_cppSet, tree_sitter_cpp());

    // 3. Python table
    handlers::RegisterPythonHandlers(m_pySet, tree_sitter_python());

    // 4. Java table
    handlers::RegisterJavaHandlers(m_javaSet, tree_sitter_java());

    // 5. JavaScript table
    handlers::RegisterJsHandlers(m_jsSet, tree_sitter_javascript());

    // 6. TypeScript table
    handlers::RegisterTsHandlers(m_tsSet, tree_sitter_typescript());

    // 7. TSX table
    handlers::RegisterTsxHandlers(m_tsxSet, tree_sitter_tsx());

    // 8. Rust table
    handlers::RegisterRustHandlers(m_rustSet, tree_sitter_rust());

    // 9. Go table
    handlers::RegisterGoHandlers(m_goSet, tree_sitter_go());

    // 10. C# table
    handlers::RegisterCSharpHandlers(m_csharpSet, tree_sitter_c_sharp());

    // 11. Auto-detect compiler → auto-enable preprocessing
    m_pCompiler = CompilerProfile::Detect();
    if (m_pCompiler)
        m_ppConfig.enabled = true;
}

void BakeOrchestrator::Release()
{
    // 1. Reset HandlerSet tables (all function pointers to nullptr)
    m_cSet      = HandlerSet{};
    m_cppSet    = HandlerSet{};
    m_pySet     = HandlerSet{};
    m_javaSet   = HandlerSet{};
    m_jsSet     = HandlerSet{};
    m_tsSet     = HandlerSet{};
    m_tsxSet    = HandlerSet{};
    m_rustSet   = HandlerSet{};
    m_goSet     = HandlerSet{};
    m_csharpSet = HandlerSet{};

    // 2. Reset preprocessing config + compiler profile
    m_ppConfig = PreprocessConfig{};
    m_pCompiler = nullptr;

    // 3. Reset stats
    m_lastStats.Reset();
}

// ─── BakeLocalFiles ─────────────────────────────────────────────────────────
// ───────────────────────────────────────
// [MainThread] BakeLocalFiles
//   → Spawn N worker threads, distribute file chunks
//   → After join, FlushToStore (main thread only)
//
// [WorkerThread] Worker lambda
//   → Only accesses ThreadLocal(pool, results) — owned data
//   → HandlerSet pointers are read-only (const*)
//   → BakeStats accumulated via atomics — thread safe
//   → Direct access to store/globalPool strictly forbidden
// ───────────────────────────────────────

int BakeOrchestrator::BakeLocalFiles(
    const std::vector<std::string>& filePaths,
    SymbolStore& store, StringPool& pool)
{
    if (filePaths.empty()) return 0;

    m_lastStats.Reset();

    const uint32_t hwThreads   = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t threadCount = std::min(hwThreads,
                                          static_cast<uint32_t>(filePaths.size()));

    std::vector<ThreadLocal> threadLocals(threadCount);

    const size_t total     = filePaths.size();
    const size_t chunkBase = total / threadCount;
    const size_t chunkRem  = total % threadCount;

    // Capture HandlerSet pointers + preprocessing config + compiler profile (read-only, thread safe)
    const HandlerSet* pCppSet    = &m_cppSet;
    const HandlerSet* pCSet      = &m_cSet;
    const HandlerSet* pPySet     = &m_pySet;
    const HandlerSet* pJavaSet   = &m_javaSet;
    const HandlerSet* pJsSet     = &m_jsSet;
    const HandlerSet* pTsSet     = &m_tsSet;
    const HandlerSet* pTsxSet    = &m_tsxSet;
    const HandlerSet* pRustSet   = &m_rustSet;
    const HandlerSet* pGoSet     = &m_goSet;
    const HandlerSet* pCsSet     = &m_csharpSet;
    const PreprocessConfig ppConfig = m_ppConfig;
    const CompilerProfile* pCompiler = m_pCompiler;

    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    size_t offset = 0;
    for (uint32_t t = 0; t < threadCount; ++t)
    {
        const size_t chunkSize = chunkBase + (t < chunkRem ? 1 : 0);
        const size_t startIdx  = offset;
        const size_t endIdx    = offset + chunkSize;
        offset = endIdx;

        workers.emplace_back(
            [&filePaths, &threadLocals, t, startIdx, endIdx,
             pCppSet, pCSet, pPySet, pJavaSet, pJsSet, pTsSet, pTsxSet,
             pRustSet, pGoSet, pCsSet, &ppConfig, pCompiler, pStats = &m_lastStats]()
        {
            using Clock = std::chrono::high_resolution_clock;
            auto& tl = threadLocals[t];

            // ── Batch preprocessing (when compiler detected + enabled, C/C++ separated) ──
            BatchResult batchPP;
            if (ppConfig.enabled && pCompiler)
            {
                auto ppT0 = Clock::now();

                // Group files by language → batch preprocessing
                std::unordered_map<std::string, std::vector<std::string>> langGroups;
                for (size_t i = startIdx; i < endIdx; ++i)
                {
                    const std::string lang = DetectLang(filePaths[i]);
                    if (lang == "c" || lang == "cpp")
                        langGroups[lang].push_back(filePaths[i]);
                }

                for (auto& [lang, files] : langGroups)
                {
                    auto result = Preprocessor::PreprocessBatch(
                        files, ppConfig, *pCompiler, lang == "cpp");
                    batchPP.merge(std::move(result));
                }

                auto ppT1 = Clock::now();
                pStats->preprocessMs.fetch_add(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ppT1 - ppT0).count(),
                    std::memory_order_relaxed);
            }

            TSParser* pParser = ts_parser_new();
            if (!pParser) return;

            SymbolBaker baker;

            for (size_t i = startIdx; i < endIdx; ++i)
            {
                // 1. Select grammar by extension + Possess
                const std::string lang = DetectLang(filePaths[i]);
                const HandlerSet* pSet = nullptr;
                TSLanguage* pLang      = nullptr;
                if (lang == "python")
                {
                    pSet  = pPySet;
                    pLang = tree_sitter_python();
                }
                else if (lang == "c")
                {
                    pSet  = pCSet;
                    pLang = tree_sitter_c();
                }
                else if (lang == "java")
                {
                    pSet  = pJavaSet;
                    pLang = tree_sitter_java();
                }
                else if (lang == "javascript")
                {
                    pSet  = pJsSet;
                    pLang = tree_sitter_javascript();
                }
                else if (lang == "typescript")
                {
                    pSet  = pTsSet;
                    pLang = tree_sitter_typescript();
                }
                else if (lang == "tsx")
                {
                    pSet  = pTsxSet;
                    pLang = tree_sitter_tsx();
                }
                else if (lang == "rust")
                {
                    pSet  = pRustSet;
                    pLang = tree_sitter_rust();
                }
                else if (lang == "go")
                {
                    pSet  = pGoSet;
                    pLang = tree_sitter_go();
                }
                else if (lang == "csharp")
                {
                    pSet  = pCsSet;
                    pLang = tree_sitter_c_sharp();
                }
                else if (lang == "cpp")
                {
                    pSet  = pCppSet;
                    pLang = tree_sitter_cpp();
                }
                else
                {
                    continue; // Unknown extension — skip
                }

                baker.Possess(pSet);
                ts_parser_set_language(pParser, pLang);

                // 2. Batch result lookup or raw file read
                std::string source;
                std::vector<int32_t> lineMapping;

                auto ppIt = batchPP.find(filePaths[i]);
                if (ppIt != batchPP.end() && !ppIt->second.source.empty())
                {
                    source      = std::move(ppIt->second.source);
                    lineMapping = std::move(ppIt->second.lineMapping);
                    pStats->preprocessedCount.fetch_add(1, std::memory_order_relaxed);
                }

                if (source.empty())   // Preprocessing failed or disabled → raw file read
                {
                    auto rdT0 = Clock::now();
                    std::ifstream ifs(filePaths[i]);
                    if (!ifs) continue;
                    std::ostringstream buf;
                    buf << ifs.rdbuf();
                    source = buf.str();
                    lineMapping.clear();
                    auto rdT1 = Clock::now();
                    pStats->readMs.fetch_add(
                        std::chrono::duration_cast<std::chrono::milliseconds>(rdT1 - rdT0).count(),
                        std::memory_order_relaxed);
                    pStats->rawReadCount.fetch_add(1, std::memory_order_relaxed);
                }

                // 3. tree-sitter parsing → POD baking
                auto bkT0 = Clock::now();
                TSTree* pTree = ts_parser_parse_string(
                    pParser, nullptr, source.c_str(),
                    static_cast<uint32_t>(source.size()));
                if (!pTree) continue;

                TSNode root = ts_tree_root_node(pTree);

                // DFS dispatch: harvest symbols/edges via Possessed HandlerSet table
                BakeResult br = baker.BakeFile(root, source, filePaths[i], tl.pool);

                auto bkT1 = Clock::now();
                pStats->parseMs.fetch_add(
                    std::chrono::duration_cast<std::chrono::milliseconds>(bkT1 - bkT0).count(),
                    std::memory_order_relaxed);

                // 4. Line number reverse mapping (preprocessed files only)
                if (!lineMapping.empty())
                {
                    auto rmT0 = Clock::now();
                    const auto mapSize = static_cast<int32_t>(lineMapping.size());
                    for (auto& sym : br.symbols)
                    {
                        if (sym.startLine > 0 && sym.startLine <= mapSize)
                            sym.startLine = lineMapping[sym.startLine - 1];
                        if (sym.endLine > 0 && sym.endLine <= mapSize)
                            sym.endLine = lineMapping[sym.endLine - 1];
                    }
                    auto rmT1 = Clock::now();
                    pStats->remapMs.fetch_add(
                        std::chrono::duration_cast<std::chrono::milliseconds>(rmT1 - rmT0).count(),
                        std::memory_order_relaxed);
                }

                tl.results.push_back(std::move(br));
                ts_tree_delete(pTree);
            }

            ts_parser_delete(pParser);
        });
    }

    for (auto& w : workers)
        if (w.joinable()) w.join();

    return FlushToStore(threadLocals, store, pool);
}

// ─── RebakeFiles (incremental update) ────────────────────────────────────────

int BakeOrchestrator::RebakeFiles(
    const std::vector<std::string>& filePaths,
    SymbolStore& store, StringPool& pool)
{
    // 1. Remove existing symbols
    RemoveFiles(filePaths, store, pool);

    // 2. Re-bake only these files
    return BakeLocalFiles(filePaths, store, pool);
}

void BakeOrchestrator::RemoveFiles(
    const std::vector<std::string>& filePaths,
    SymbolStore& store, StringPool& pool)
{
    for (const auto& path : filePaths)
    {
        uint32_t fileId = pool.Find(path);
        if (fileId != 0)
            store.RemoveFile(fileId);
    }
}

// ─── DetectLang ─────────────────────────────────────────────────────────────

std::string BakeOrchestrator::DetectLang(const std::string& filePath)
{
    const size_t dotPos = filePath.rfind('.');
    if (dotPos == std::string::npos) return "";

    std::string ext = filePath.substr(dotPos + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "c")   return "c";
    if (ext == "cpp" || ext == "cxx" || ext == "cc" ||
        ext == "h"   || ext == "hpp" || ext == "hxx")
        return "cpp";
    if (ext == "py")    return "python";
    if (ext == "java")  return "java";
    if (ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx")
        return "javascript";
    if (ext == "ts" || ext == "mts")
        return "typescript";
    if (ext == "tsx")   return "tsx";
    if (ext == "rs")    return "rust";
    if (ext == "go")    return "go";
    if (ext == "cs")    return "csharp";

    return "";
}

} // namespace fce
