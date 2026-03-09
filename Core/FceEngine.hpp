#pragma once

#include "Core/IFceObject.hpp"
#include "Baked/StringPool.hpp"
#include "Baked/Store/SymbolStore.hpp"
#include "Symbol/RelationKind.hpp"
#include "Core/QueryEngine.hpp"
#include "Core/BakeOrchestrator.hpp"
#include "Core/QuerySystem.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace fce
{

    /**
     * FCE top-level facade (IFceObject implementation).
     *
     * Lifecycle:
     *   FceEngine() → Initialize() → IndexLocalFiles() → Query() → Release()
     */
    class FceEngine : public IFceObject
    {
    public:
        FceEngine();
        ~FceEngine() override;

        // ─ Lifecycle (IFceObject)
        void Initialize() override;
        void Update(float dt) override;
        void Release()    override;

        // ─ Indexing API (→ BakeOrchestrator delegation)
        int IndexLocalFiles(const std::vector<std::string>& filePaths);

        /** Remove and re-parse specific files (incremental update) */
        int ReindexFiles(const std::vector<std::string>& filePaths);

        /** Remove symbols/edges for specific files */
        void RemoveFiles(const std::vector<std::string>& filePaths);

        // ─ Preprocess config + stats (→ BakeOrchestrator delegation)
        void SetPreprocessConfig(const PreprocessConfig& config);
        const BakeStats& GetBakeStats() const;

        // ─ Query API (→ QuerySystem delegation)
        QueryResult Query(const std::string& name,
                         int bfsDepth = 2,
                         RelationMask filter = kAllRelations,
                         bool reverse = false) const;
        SmartQueryResult SmartQuery(const std::string& name,
                                    const DensityConfig& densityConfig = DensityConfig::Default()) const;
        std::vector<SymbolEntry> Lookup(const std::string& name) const;
        std::size_t SymbolCount() const;

        // ─ Pincer extraction API (file I/O, static)
        static SnippetResult PincerExtract(
            const std::string& filePath,
            int startLine, int endLine,
            const std::string& targetName,
            int radius = 7);

        // ─ Introspection (thin JSON conversion — kept here)
        std::string GetFileTree() const;
        std::string GetFileSymbols(const std::string& path) const;
        std::string SearchSymbols(const std::string& prefix, int limit) const;

    protected:
        /** Lock on every public method entry — callers need not manage an external mutex */
        mutable std::mutex m_mutex;

        // ─ SOA data (DOD)
        SymbolStore m_store;
        StringPool  m_pool;

        // ─ Subsystems
        BakeOrchestrator m_baker;
        QuerySystem      m_query;

        // ─ Internal helper (runs without lock — called by public methods after locking)
        int IndexLocalFilesNoLock(const std::vector<std::string>& filePaths);

        // ─ Cached file list for Update() re-indexing
        std::vector<std::string> m_lastLocalFiles;
    };

} // namespace fce
