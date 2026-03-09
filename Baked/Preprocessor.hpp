#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fce
{

/**
 * Preprocessor project configuration — include paths, macro definitions, etc.
 *
 * Compiler selection is handled by CompilerProfile; only project-specific settings here.
 */
struct PreprocessConfig
{
    bool enabled = false;
    std::vector<std::string> includePaths;      ///< -I/path/to/kernel/include
    std::vector<std::string> defines;           ///< "__KERNEL__", "CONFIG_SMP"
    std::vector<std::string> extraFlags;        ///< Additional flags
    size_t batchSize = 50;                      ///< Batch mode: files per invocation
};

/**
 * Compiler profile — function pointer table bound to the Preprocessor.
 *
 * Same pattern as HandlerSet — compiler-specific command building + linemarker parsing
 * are separated via function pointers so Preprocessor logic stays compiler-agnostic.
 *
 * Read-only, therefore thread-safe.
 */
struct CompilerProfile
{
    /** Build preprocessing command — encapsulates per-compiler flag differences */
    using BuildCmdFn = std::string(*)(const std::string& filePath,
                                       const CompilerProfile& profile,
                                       const PreprocessConfig& config,
                                       bool bIsCpp);

    /**
     * Parse linemarker — determine if a line is a linemarker + extract line number/file.
     *
     * @param pLine       Pointer to line start
     * @param len         Line length
     * @param outLineNum  Extracted line number (output)
     * @param outFile     Extracted file path (output)
     * @return true if linemarker, false if regular code
     */
    using ParseMarkerFn = bool(*)(const char* pLine, size_t len,
                                   long& outLineNum, std::string& outFile);

    const char* cCompiler;       ///< C compiler binary ("gcc", "clang", "cl")
    const char* cppCompiler;     ///< C++ compiler binary ("g++", "clang++", "cl")
    BuildCmdFn BuildCommand;     ///< Command build function
    ParseMarkerFn ParseMarker;   ///< Linemarker parse function

    /** Auto-detect an available compiler on the system. Returns nullptr if none found */
    static const CompilerProfile* Detect();

    static const CompilerProfile kGcc;
    static const CompilerProfile kClang;
    static const CompilerProfile kMsvc;
};

/**
 * Preprocessing result.
 *
 * Returns the expanded source with a reverse mapping table from parsed linemarkers.
 * If source is an empty string, preprocessing failed — caller should fall back to raw source.
 */
struct PreprocessResult
{
    std::string source;                     ///< Expanded source with linemarkers removed
    std::vector<int32_t> lineMapping;       ///< lineMapping[i] = original line number for expanded source line (i+1)
};

/// File path -> preprocessing result map (for batch mode)
using BatchResult = std::unordered_map<std::string, PreprocessResult>;

/**
 * Lightweight preprocessor wrapper — popen-based compiler -E invocation.
 *
 * Binds a CompilerProfile for compiler-agnostic operation:
 *   source file -> profile.BuildCommand() -> popen -> profile.ParseMarker() -> reverse mapping
 *
 * Batch mode:
 *   N files bundled via #include into a temp file -> single compiler -E call -> split by linemarkers
 *   Process spawns reduced from 63K to ~1260 (with batchSize=50)
 */
namespace Preprocessor
{
    /**
     * Preprocess a single file -> expanded source + line number reverse mapping. Empty result on failure.
     *
     * @param filePath  Source file path to preprocess
     * @param config    Project settings (include paths, macros, etc.)
     * @param profile   Bound compiler profile
     * @param bIsCpp    If true, use C++ compiler
     */
    PreprocessResult PreprocessFile(const std::string& filePath,
                                    const PreprocessConfig& config,
                                    const CompilerProfile& profile,
                                    bool bIsCpp = false);

    /**
     * Batch preprocessing — bundle multiple files into a single compiler invocation.
     *
     * @param filePaths  List of file paths to preprocess
     * @param config     Project settings
     * @param profile    Bound compiler profile
     * @param bIsCpp     If true, use C++ compiler
     */
    BatchResult PreprocessBatch(const std::vector<std::string>& filePaths,
                                const PreprocessConfig& config,
                                const CompilerProfile& profile,
                                bool bIsCpp = false);
}

} // namespace fce
