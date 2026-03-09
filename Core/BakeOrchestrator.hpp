#pragma once

#include "Core/IFceObject.hpp"
#include "Baked/Handlers/HandlerSet.hpp"
#include "Baked/Preprocessor.hpp"
#include "Baked/StringPool.hpp"
#include "Baked/Store/SymbolStore.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace fce
{

/**
 * BakeLocalFiles execution statistics — for profiling bottlenecks.
 * Accumulated atomically by worker threads → read from main thread.
 */
struct BakeStats
{
    std::atomic<int64_t> preprocessMs{0};     ///< Total batch preprocessing time (sum across all threads)
    std::atomic<int64_t> parseMs{0};          ///< Total tree-sitter parsing+baking time
    std::atomic<int64_t> readMs{0};           ///< Total raw file read time
    std::atomic<int64_t> remapMs{0};          ///< Total line number reverse-mapping time
    std::atomic<int32_t> preprocessedCount{0};///< Number of successfully preprocessed files
    std::atomic<int32_t> rawReadCount{0};     ///< Number of raw-read fallback files

    void Reset()
    {
        preprocessMs.store(0);
        parseMs.store(0);
        readMs.store(0);
        remapMs.store(0);
        preprocessedCount.store(0);
        rawReadCount.store(0);
    }
};

/**
 * Multi-threaded baking orchestrator (IFceObject implementation).
 *
 * Owns HandlerSets for each supported language and has each worker thread Possess a SymbolBaker for parsing.
 *
 * Lifecycle:
 *   Initialize() — initialize HandlerSets (register per-language tree-sitter handlers)
 *   Release()    — clear HandlerSet tables + reset stats
 */
class BakeOrchestrator : public IFceObject
{
public:
    /** Initialize all HandlerSets (register per-language tree-sitter symbols) */
    void Initialize() override;

    /** Clear HandlerSet tables + reset stats */
    void Release() override;

    /** C preprocessor config (gcc -E -P). Applied only in BakeLocalFiles. */
    inline void SetPreprocessConfig(const PreprocessConfig& config) { m_ppConfig = config; }
    inline const PreprocessConfig& GetPreprocessConfig() const { return m_ppConfig; }

    /** Stats from the last BakeLocalFiles run */
    inline const BakeStats& GetLastStats() const { return m_lastStats; }

    /**
     * Bake local files in parallel → load into store.
     * Auto-selects grammar by extension (Possess/UnPossess).
     *
     * @param filePaths  List of file paths to bake
     * @param store      Global SymbolStore where symbols/edges are loaded
     * @param pool       Global StringPool for string interning
     * @return Number of files processed
     */
    int BakeLocalFiles(const std::vector<std::string>& filePaths,
                       SymbolStore& store, StringPool& pool);

    /**
     * Remove existing symbols for specific files → re-bake (incremental update).
     *
     * @param filePaths  List of file paths to re-bake
     * @param store   Global SymbolStore
     * @param pool    Global StringPool
     * @return Number of files processed
     */
    int RebakeFiles(const std::vector<std::string>& filePaths,
                    SymbolStore& store, StringPool& pool);

    /**
     * Remove only the symbols/edges for specific files (fileId-based deletion from store).
     *
     * @param filePaths  List of file paths to remove
     * @param store   Global SymbolStore
     * @param pool    Global StringPool
     */
    void RemoveFiles(const std::vector<std::string>& filePaths,
                     SymbolStore& store, StringPool& pool);

    /** Extension → language string ("c", "cpp", "python", "java", "javascript", "typescript", "tsx", "rust", "go", "csharp", or "") */
    static std::string DetectLang(const std::string& filePath);

protected:
    HandlerSet m_cppSet;
    HandlerSet m_cSet;
    HandlerSet m_pySet;
    HandlerSet m_javaSet;
    HandlerSet m_jsSet;
    HandlerSet m_tsSet;
    HandlerSet m_tsxSet;
    HandlerSet m_rustSet;
    HandlerSet m_goSet;
    HandlerSet m_csharpSet;
    PreprocessConfig m_ppConfig;
    const CompilerProfile* m_pCompiler = nullptr;   ///< Possessed compiler profile (nullptr = preprocessing disabled)
    BakeStats m_lastStats;
};

} // namespace fce
