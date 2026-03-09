#include "Baked/Preprocessor.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

// ─── Platform popen/pclose ───────────────────────────────────────────────────
#ifdef _WIN32
    #define POPEN  _popen
    #define PCLOSE _pclose
    static constexpr const char* kStderrRedirect = " 2>nul";
#else
    #define POPEN  popen
    #define PCLOSE pclose
    static constexpr const char* kStderrRedirect = " 2>/dev/null";
#endif

// ─── Compiler profile implementation ─────────────────────────────────────────

namespace
{

/** Path separator normalization (\ -> /) — for linemarker matching */
std::string NormalizePath(const std::string& path)
{
    std::string result = path;
    for (char& c : result)
        if (c == '\\') c = '/';
    return result;
}

/** Check if a compiler binary is available */
bool IsCompilerAvailable(const char* pCompiler)
{
    if (!pCompiler) return false;

    std::string cmd;
#ifdef _WIN32
    cmd = "where ";
    cmd += pCompiler;
    cmd += " >nul 2>nul";
#else
    cmd = "which ";
    cmd += pCompiler;
    cmd += " >/dev/null 2>/dev/null";
#endif

    return std::system(cmd.c_str()) == 0;
}

// ─── gcc/clang shared: command building ──────────────────────────────────────

std::string BuildGccLikeCommand(const std::string& filePath,
                                 const fce::CompilerProfile& profile,
                                 const fce::PreprocessConfig& config,
                                 bool bIsCpp)
{
    std::string cmd;
    cmd.reserve(512);

    cmd += bIsCpp ? profile.cppCompiler : profile.cCompiler;
    cmd += " -E";

    for (const auto& inc : config.includePaths)
    {
        cmd += " -I\"";
        cmd += inc;
        cmd += '"';
    }
    for (const auto& def : config.defines)
    {
        cmd += " -D";
        cmd += def;
    }
    for (const auto& flag : config.extraFlags)
    {
        cmd += ' ';
        cmd += flag;
    }

    cmd += " \"";
    cmd += filePath;
    cmd += '"';
    cmd += kStderrRedirect;
    return cmd;
}

// ─── gcc/clang shared: linemarker parsing ────────────────────────────────────
// Format: # <num> "<filename>" [flags...]

bool ParseGccMarker(const char* pLine, size_t len,
                     long& outLineNum, std::string& outFile)
{
    if (len < 2 || pLine[0] != '#' || pLine[1] != ' ')
        return false;

    const char* pNumStart = pLine + 2;
    char* pNumEnd = nullptr;
    outLineNum = std::strtol(pNumStart, &pNumEnd, 10);

    if (!pNumEnd || pNumEnd <= pNumStart ||
        *pNumEnd != ' ' || *(pNumEnd + 1) != '"')
        return false;

    const char* pFnameStart = pNumEnd + 2;
    const char* pFnameEnd = pFnameStart;
    const char* pLineEnd = pLine + len;
    while (pFnameEnd < pLineEnd && *pFnameEnd != '"')
        ++pFnameEnd;

    outFile.assign(pFnameStart, pFnameEnd);
    return true;
}

// ─── MSVC: command building ──────────────────────────────────────────────────

std::string BuildMsvcCommand(const std::string& filePath,
                              const fce::CompilerProfile& profile,
                              const fce::PreprocessConfig& config,
                              bool bIsCpp)
{
    std::string cmd;
    cmd.reserve(512);

    cmd += profile.cCompiler;   // "cl" (same binary for C/C++)
    cmd += " /E";
    cmd += bIsCpp ? " /TP" : " /TC";   // /TP=C++, /TC=C forced

    for (const auto& inc : config.includePaths)
    {
        cmd += " /I\"";
        cmd += inc;
        cmd += '"';
    }
    for (const auto& def : config.defines)
    {
        cmd += " /D";
        cmd += def;
    }
    for (const auto& flag : config.extraFlags)
    {
        cmd += ' ';
        cmd += flag;
    }

    cmd += " \"";
    cmd += filePath;
    cmd += '"';
    cmd += kStderrRedirect;
    return cmd;
}

// ─── MSVC: linemarker parsing ────────────────────────────────────────────────
// Format: #line <num> "<filename>"

bool ParseMsvcMarker(const char* pLine, size_t len,
                      long& outLineNum, std::string& outFile)
{
    // "#line " = 6 characters
    if (len < 6 || std::strncmp(pLine, "#line ", 6) != 0)
        return false;

    const char* pNumStart = pLine + 6;
    char* pNumEnd = nullptr;
    outLineNum = std::strtol(pNumStart, &pNumEnd, 10);

    if (!pNumEnd || pNumEnd <= pNumStart ||
        *pNumEnd != ' ' || *(pNumEnd + 1) != '"')
        return false;

    const char* pFnameStart = pNumEnd + 2;
    const char* pFnameEnd = pFnameStart;
    const char* pLineEnd = pLine + len;
    while (pFnameEnd < pLineEnd && *pFnameEnd != '"')
        ++pFnameEnd;

    outFile.assign(pFnameStart, pFnameEnd);
    return true;
}

} // anonymous namespace

// ─── CompilerProfile static instances + auto-detection ──────────────────────

namespace fce
{

const CompilerProfile CompilerProfile::kGcc {
    "gcc", "g++", BuildGccLikeCommand, ParseGccMarker
};

const CompilerProfile CompilerProfile::kClang {
    "clang", "clang++", BuildGccLikeCommand, ParseGccMarker
};

const CompilerProfile CompilerProfile::kMsvc {
    "cl", "cl", BuildMsvcCommand, ParseMsvcMarker
};

const CompilerProfile* CompilerProfile::Detect()
{
#ifdef _WIN32
    // Step 1: Windows — MSVC first, then clang, then gcc
    if (IsCompilerAvailable("cl"))    return &kMsvc;
    if (IsCompilerAvailable("clang")) return &kClang;
    if (IsCompilerAvailable("gcc"))   return &kGcc;
#else
    // Step 2: Unix — gcc first, then clang
    if (IsCompilerAvailable("gcc"))   return &kGcc;
    if (IsCompilerAvailable("clang")) return &kClang;
#endif
    return nullptr;
}

// ─── PreprocessFile ─────────────────────────────────────────────────────────

PreprocessResult Preprocessor::PreprocessFile(const std::string& filePath,
                                               const PreprocessConfig& config,
                                               const CompilerProfile& profile,
                                               bool bIsCpp)
{
    PreprocessResult result;

    if (!config.enabled || filePath.empty())
        return result;

    // Step 1: Build command (dispatched via profile function pointer)
    const std::string cmd = profile.BuildCommand(filePath, profile, config, bIsCpp);

    // Step 2: Capture stdout via popen
    FILE* pPipe = POPEN(cmd.c_str(), "r");
    if (!pPipe)
        return result;

    std::string rawOutput;
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), pPipe))
        rawOutput += buf;

    const int exitCode = PCLOSE(pPipe);

    if (exitCode != 0 || rawOutput.empty())
        return result;

    // Step 3: Parse and strip linemarkers (using profile parser)
    const std::string origBasename = [&filePath]() {
        const auto slashPos = filePath.find_last_of("/\\");
        return (slashPos != std::string::npos)
                   ? filePath.substr(slashPos + 1)
                   : filePath;
    }();

    result.source.reserve(rawOutput.size());

    int32_t currentOrigLine = 1;
    bool bInOrigFile = true;

    const char* pPtr = rawOutput.c_str();
    const char* pEnd = pPtr + rawOutput.size();

    while (pPtr < pEnd)
    {
        // Find end of current line
        const char* pLineEnd = pPtr;
        while (pLineEnd < pEnd && *pLineEnd != '\n')
            ++pLineEnd;

        const size_t lineLen = static_cast<size_t>(pLineEnd - pPtr);

        long markerLineNum = 0;
        std::string markerFile;

        if (profile.ParseMarker(pPtr, lineLen, markerLineNum, markerFile))
        {
            // linemarker — determine if this is the original file by basename comparison
            const auto mSlash = markerFile.find_last_of("/\\");
            const std::string markerBasename =
                (mSlash != std::string::npos)
                    ? markerFile.substr(mSlash + 1)
                    : markerFile;

            bInOrigFile = (markerBasename == origBasename);
            currentOrigLine = static_cast<int32_t>(markerLineNum);
        }
        else if (bInOrigFile)
        {
            // Code from the original file — append to source + record lineMapping
            result.source.append(pPtr, lineLen);
            result.source += '\n';
            result.lineMapping.push_back(currentOrigLine);
            ++currentOrigLine;
        }

        pPtr = (pLineEnd < pEnd) ? pLineEnd + 1 : pEnd;
    }

    // Treat empty source as failure
    if (result.source.empty())
    {
        result.lineMapping.clear();
        return result;
    }

    return result;
}

// ─── PreprocessBatch ────────────────────────────────────────────────────────

BatchResult Preprocessor::PreprocessBatch(
    const std::vector<std::string>& filePaths,
    const PreprocessConfig& config,
    const CompilerProfile& profile,
    bool bIsCpp)
{
    BatchResult results;
    if (!config.enabled || filePaths.empty())
        return results;

    const size_t batchSz = std::max(size_t(1), config.batchSize);
    const std::string tmpExt = bIsCpp ? ".cpp" : ".c";

    // Normalized path -> original path reverse mapping (for linemarker matching)
    std::unordered_map<std::string, std::string> normToOrig;
    normToOrig.reserve(filePaths.size());
    for (const auto& fp : filePaths)
        normToOrig[NormalizePath(fp)] = fp;

    namespace fs = std::filesystem;
    const auto tmpDir = fs::temp_directory_path();
    static std::atomic<uint64_t> s_counter{0};

    for (size_t bStart = 0; bStart < filePaths.size(); bStart += batchSz)
    {
        const size_t bEnd = std::min(bStart + batchSz, filePaths.size());

        // Step 1: Create temp batch file: #include "abs_path" x N
        const auto batchId = s_counter.fetch_add(1, std::memory_order_relaxed);
        const auto tmpFile = tmpDir / ("fce_batch_" + std::to_string(batchId) + tmpExt);

        {
            std::ofstream ofs(tmpFile);
            if (!ofs) continue;
            for (size_t i = bStart; i < bEnd; ++i)
                ofs << "#include \"" << NormalizePath(filePaths[i]) << "\"\n";
        }

        // Step 2: Single compiler -E invocation (dispatched via profile function pointer)
        const std::string cmd = profile.BuildCommand(tmpFile.string(), profile, config, bIsCpp);
        FILE* pPipe = POPEN(cmd.c_str(), "r");
        if (!pPipe)
        {
            fs::remove(tmpFile);
            continue;
        }

        // Step 3: Streaming linemarker parsing (using profile parser)
        PreprocessResult* pCurrent = nullptr;
        int32_t currentOrigLine = 1;

        std::string line;
        char buf[8192];

        while (std::fgets(buf, sizeof(buf), pPipe))
        {
            line += buf;
            // If fgets didn't read up to '\n', the line is still incomplete
            if (line.empty() || line.back() != '\n')
                continue;

            // Strip trailing newline/cr
            size_t len = line.size();
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                --len;

            long markerLineNum = 0;
            std::string markerFile;

            if (profile.ParseMarker(line.c_str(), len, markerLineNum, markerFile))
            {
                const std::string markerNorm = NormalizePath(markerFile);
                auto it = normToOrig.find(markerNorm);
                if (it != normToOrig.end())
                {
                    pCurrent = &results[it->second];
                    currentOrigLine = static_cast<int32_t>(markerLineNum);
                }
                else
                {
                    pCurrent = nullptr;   // Header, etc. — skip
                }
            }
            else if (pCurrent)
            {
                pCurrent->source.append(line.c_str(), len);
                pCurrent->source += '\n';
                pCurrent->lineMapping.push_back(currentOrigLine);
                ++currentOrigLine;
            }

            line.clear();
        }

        // Last line (in case it ended without a newline)
        if (!line.empty() && pCurrent)
        {
            size_t len = line.size();
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                --len;
            if (len > 0)
            {
                pCurrent->source.append(line.c_str(), len);
                pCurrent->source += '\n';
                pCurrent->lineMapping.push_back(currentOrigLine);
            }
        }

        PCLOSE(pPipe);
        fs::remove(tmpFile);
    }

    return results;
}

} // namespace fce
