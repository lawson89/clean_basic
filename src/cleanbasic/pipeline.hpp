#ifndef CLEANBASIC_PIPELINE_HPP
#define CLEANBASIC_PIPELINE_HPP

#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cb {

struct BuildTimings {
    double lexParseMs = 0.0;
    double moduleLoadMs = 0.0;
    double analyzeMs = 0.0;
    double emitMs = 0.0;
    double transpileTotalMs = 0.0;
    double pchBuildMs = 0.0;
    double cppCompileMs = 0.0;
    double totalMs = 0.0;
    bool pchUsed = false;
    bool pchCacheHit = false;
};

inline double elapsedMillis(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end
) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct CheckSourceResult {
    bool ok = false;
    AstArena arena;
    Program program;
    GlobalSymbolTable symbols;
    DiagnosticError error;
    BuildTimings timings{};
};

struct TranspileResult {
    bool ok = false;
    std::string cppCode;
    GlobalSymbolTable symbols;
    DiagnosticError error;
    BuildTimings timings{};
};

struct TargetSpec {
    bool ok = false;
    bool isCross = false;
    bool isWindows = false;
    std::string canonicalName;
    std::string zigTriple;
    std::string binaryExt;
    std::string errorMessage;
};

struct BuildOptions {
    std::string outputCppPath;
    bool compileBinary = false;
    std::string binaryOutput;
    std::string target;
    std::string compiler;
    std::string optLevel = "0";
    bool isRelease = false;
    bool usePch = true;
    bool rebuildPch = false;
    bool staticLink = false;
};

struct BuildFileResult {
    bool ok = false;
    std::string outputCppPath;
    std::string outputBinaryPath;
    std::string compileCommand;
    std::vector<std::string> linkerFlags;
    DiagnosticError error;
    BuildTimings timings{};
};

inline bool hostIsWindows() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

inline bool hostIsMacOs() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

inline TargetSpec resolveTargetSpec(std::string_view rawTarget) {
    TargetSpec spec{};
    if (rawTarget.empty() || rawTarget == "native" || rawTarget == "host") {
        spec.ok = true;
        spec.isCross = false;
        spec.isWindows = hostIsWindows();
        spec.canonicalName = "native";
        spec.zigTriple = "";
        if (spec.isWindows) {
            spec.binaryExt = ".exe";
        } else {
            spec.binaryExt = "";
        }
        return spec;
    }

    if (rawTarget == "linux" || rawTarget == "linux-x86_64" || rawTarget == "x86_64-linux" || rawTarget == "x86_64-linux-musl") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = false;
        spec.canonicalName = "linux-x86_64";
        spec.zigTriple = "x86_64-linux-musl";
        spec.binaryExt = "";
        return spec;
    }

    if (rawTarget == "linux-gnu" || rawTarget == "linux-x86_64-gnu" || rawTarget == "x86_64-linux-gnu") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = false;
        spec.canonicalName = "linux-x86_64-gnu";
        spec.zigTriple = "x86_64-linux-gnu";
        spec.binaryExt = "";
        return spec;
    }

    if (rawTarget == "linux-arm64" || rawTarget == "linux-aarch64" || rawTarget == "aarch64-linux" || rawTarget == "aarch64-linux-musl") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = false;
        spec.canonicalName = "linux-arm64";
        spec.zigTriple = "aarch64-linux-musl";
        spec.binaryExt = "";
        return spec;
    }

    if (rawTarget == "windows" || rawTarget == "win" || rawTarget == "windows-x86_64" ||
        rawTarget == "win64" || rawTarget == "x86_64-windows" || rawTarget == "x86_64-windows-gnu") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = true;
        spec.canonicalName = "windows-x86_64";
        spec.zigTriple = "x86_64-windows-gnu";
        spec.binaryExt = ".exe";
        return spec;
    }

    if (rawTarget == "windows-arm64" || rawTarget == "windows-aarch64" ||
        rawTarget == "aarch64-windows" || rawTarget == "aarch64-windows-gnu") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = true;
        spec.canonicalName = "windows-arm64";
        spec.zigTriple = "aarch64-windows-gnu";
        spec.binaryExt = ".exe";
        return spec;
    }

    if (rawTarget == "macos" || rawTarget == "macos-x86_64" || rawTarget == "darwin" ||
        rawTarget == "darwin-x86_64" || rawTarget == "x86_64-macos") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = false;
        spec.canonicalName = "macos-x86_64";
        spec.zigTriple = "x86_64-macos";
        spec.binaryExt = "";
        return spec;
    }

    if (rawTarget == "macos-arm64" || rawTarget == "macos-aarch64" || rawTarget == "darwin-arm64" ||
        rawTarget == "aarch64-macos") {
        spec.ok = true;
        spec.isCross = true;
        spec.isWindows = false;
        spec.canonicalName = "macos-arm64";
        spec.zigTriple = "aarch64-macos";
        spec.binaryExt = "";
        return spec;
    }

    spec.ok = false;
    spec.errorMessage = "Unsupported target '" + std::string(rawTarget) +
        "'. Supported targets: native, linux (linux-x86_64), linux-arm64, windows (windows-x86_64), windows-arm64, macos (macos-x86_64), macos-arm64.";
    return spec;
}

inline std::string resolveBinaryOutputPath(
    std::string_view parentDir,
    std::string_view explicitBinaryOutput,
    const TargetSpec& targetSpec
) {
    std::string binPath;
    if (!explicitBinaryOutput.empty()) {
        binPath = std::string(explicitBinaryOutput);
    } else {
        binPath = std::string(parentDir) + "/app";
    }
    if (targetSpec.isWindows) {
        const bool hasExeExt = binPath.size() >= 4 &&
            (binPath.substr(binPath.size() - 4) == ".exe" || binPath.substr(binPath.size() - 4) == ".EXE");
        if (!hasExeExt) {
            binPath += ".exe";
        }
    }
    return binPath;
}

inline std::string resolveCompilerCommand(
    const TargetSpec& targetSpec,
    std::string_view explicitCompiler
) {
    if (!explicitCompiler.empty()) {
        if (targetSpec.isCross && explicitCompiler == "zig c++") {
            return "zig c++ -target " + targetSpec.zigTriple;
        }
        return std::string(explicitCompiler);
    }
    if (targetSpec.isCross) {
        return "zig c++ -target " + targetSpec.zigTriple;
    }
    const std::string envCxx = getEnvVar("CXX");
    if (!envCxx.empty()) {
        return envCxx;
    }
#if defined(__APPLE__)
    return "clang++";
#else
    return "g++";
#endif
}

inline uint64_t fnv1aUpdate64(uint64_t hash, std::string_view data) {
    constexpr uint64_t fnvPrime = 1099511628211ULL;
    uint64_t h = hash;
    for (unsigned char c : data) {
        h ^= static_cast<uint64_t>(c);
        h *= fnvPrime;
    }
    return h;
}

inline std::string formatHex64(uint64_t val) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << val;
    return oss.str();
}

inline bool supportsBackendPch(std::string_view compilerCmd, const TargetSpec& targetSpec) {
    if (targetSpec.isCross) {
        return false;
    }
    if (compilerCmd.find("zig") != std::string_view::npos) {
        return false;
    }
    return true;
}

inline std::string buildBackendPchHeaderSource() {
    std::string content;
    content.reserve(CPP_RUNTIME_PREAMBLE.size() + 256);
    content += R"(// CleanBasic Auto-Generated Backend Precompiled Header
)";
    content += CPP_RUNTIME_PREAMBLE;
    content += R"(
#include "str/str_bridge.hpp"
#include "os/os_bridge.hpp"
#include "fs/fs_bridge.hpp"
#include "json/json_bridge.hpp"
)";
    return content;
}

inline std::string computeBackendPchHash(
    std::string_view compilerCmd,
    const TargetSpec& targetSpec,
    std::string_view optLevel,
    std::string_view cbRoot
) {
    uint64_t hash = 14695981039346656037ULL;
    hash = fnv1aUpdate64(hash, compilerCmd);
    hash = fnv1aUpdate64(hash, "|");
    hash = fnv1aUpdate64(hash, targetSpec.canonicalName);
    hash = fnv1aUpdate64(hash, "|");
    hash = fnv1aUpdate64(hash, optLevel);
    hash = fnv1aUpdate64(hash, "|");
    hash = fnv1aUpdate64(hash, CPP_RUNTIME_PREAMBLE);

    const std::vector<std::string> bridgeRelPaths = {
        "/stdlib/str/str_bridge.hpp",
        "/stdlib/os/os_bridge.hpp",
        "/stdlib/fs/fs_bridge.hpp",
        "/stdlib/json/json_bridge.hpp",
        "/vendor/json.hpp"
    };
    for (const auto& rel : bridgeRelPaths) {
        const std::string fullPath = std::string(cbRoot) + rel;
        std::error_code ecSize;
        std::error_code ecTime;
        const auto fsize = std::filesystem::file_size(fullPath, ecSize);
        const auto mtime = std::filesystem::last_write_time(fullPath, ecTime);
        if (!ecSize && !ecTime) {
            const auto mtimeTicks = mtime.time_since_epoch().count();
            hash = fnv1aUpdate64(hash, "|");
            hash = fnv1aUpdate64(hash, std::to_string(fsize));
            hash = fnv1aUpdate64(hash, ":");
            hash = fnv1aUpdate64(hash, std::to_string(mtimeTicks));
        } else {
            std::ifstream in(fullPath, std::ios::in | std::ios::binary);
            if (in.is_open()) {
                std::ostringstream ss;
                ss << in.rdbuf();
                hash = fnv1aUpdate64(hash, "|");
                hash = fnv1aUpdate64(hash, ss.str());
            }
        }
    }
    return formatHex64(hash);
}

inline std::string resolveBackendPchDir(std::string_view cbRoot) {
    std::error_code ec;
    const std::filesystem::path primaryDir = std::filesystem::path(std::string(cbRoot)) / "build" / "pch";
    std::filesystem::create_directories(primaryDir, ec);
    if (!ec && std::filesystem::exists(primaryDir, ec)) {
        return primaryDir.string();
    }
    const std::filesystem::path fallbackDir = std::filesystem::temp_directory_path(ec) / "cleanbasic_pch";
    std::filesystem::create_directories(fallbackDir, ec);
    return fallbackDir.string();
}

struct PchPrepareResult {
    bool used = false;
    bool cacheHit = false;
    double buildMs = 0.0;
    std::string headerPath;
};

inline PchPrepareResult ensureBackendPch(
    std::string_view compilerCmd,
    const TargetSpec& targetSpec,
    std::string_view optLevel,
    std::string_view cbRoot,
    bool usePch,
    bool rebuildPch
) {
    PchPrepareResult res{};
    if (!usePch || !supportsBackendPch(compilerCmd, targetSpec)) {
        return res;
    }

    std::string resolvedOpt = "0";
    if (!optLevel.empty()) {
        resolvedOpt = std::string(optLevel);
    }

    const std::string pchDir = resolveBackendPchDir(cbRoot);
    const std::string hashHex = computeBackendPchHash(compilerCmd, targetSpec, resolvedOpt, cbRoot);
    const std::string sanitizedCc = sanitizeModuleId(compilerCmd);
    const std::string baseName = "cb_pch_" + sanitizedCc + "_O" + resolvedOpt + "_" + hashHex + ".hpp";
    const std::filesystem::path headerPath = std::filesystem::path(pchDir) / baseName;
    const std::filesystem::path gchPath = std::filesystem::path(headerPath.string() + ".gch");

    std::error_code ec;
    if (!rebuildPch && std::filesystem::exists(headerPath, ec) && std::filesystem::exists(gchPath, ec)) {
        res.used = true;
        res.cacheHit = true;
        res.headerPath = headerPath.string();
        return res;
    }

    const auto nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string uniqueSuffix = std::to_string(nowTicks);
    const std::filesystem::path tmpHeaderPath = std::filesystem::path(headerPath.string() + ".tmp." + uniqueSuffix);
    {
        std::ofstream outHeader(tmpHeaderPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!outHeader.is_open()) {
            return res;
        }
        outHeader << buildBackendPchHeaderSource();
        outHeader.close();
    }

    std::filesystem::rename(tmpHeaderPath, headerPath, ec);
    if (ec) {
        std::filesystem::remove(headerPath, ec);
        ec.clear();
        std::filesystem::rename(tmpHeaderPath, headerPath, ec);
        if (ec) {
            std::filesystem::remove(tmpHeaderPath, ec);
            return res;
        }
    }

    const std::filesystem::path tmpGchPath = std::filesystem::path(gchPath.string() + ".tmp." + uniqueSuffix);
    const std::string q(1, '"');
    const std::string pchCmd = std::string(compilerCmd) +
        " -x c++-header -O" + resolvedOpt + " -std=c++17 -pthread" +
        " -I" + q + std::string(cbRoot) + "/vendor" + q +
        " -I" + q + std::string(cbRoot) + "/stdlib" + q +
        " " + q + headerPath.string() + q +
        " -o " + q + tmpGchPath.string() + q;

    const auto tStart = std::chrono::steady_clock::now();
    const int ret = std::system(pchCmd.c_str());
    const auto tEnd = std::chrono::steady_clock::now();
    res.buildMs = elapsedMillis(tStart, tEnd);

    if (ret != 0 || !std::filesystem::exists(tmpGchPath, ec)) {
        std::filesystem::remove(tmpGchPath, ec);
        return res;
    }

    std::filesystem::rename(tmpGchPath, gchPath, ec);
    if (ec) {
        std::filesystem::remove(gchPath, ec);
        ec.clear();
        std::filesystem::rename(tmpGchPath, gchPath, ec);
        if (ec) {
            std::filesystem::remove(tmpGchPath, ec);
            return res;
        }
    }

    res.used = true;
    res.cacheHit = false;
    res.headerPath = headerPath.string();
    return res;
}

inline bool findExecutableInPath(std::string_view exeName, std::string_view pathEnv) {
    if (exeName.empty() || pathEnv.empty()) {
        return false;
    }
    char sep = ':';
    if (hostIsWindows()) {
        sep = ';';
    }
    size_t start = 0;
    while (start <= pathEnv.size()) {
        const size_t pos = pathEnv.find(sep, start);
        std::string_view dir;
        if (pos == std::string_view::npos) {
            dir = pathEnv.substr(start);
            start = pathEnv.size() + 1;
        } else {
            dir = pathEnv.substr(start, pos - start);
            start = pos + 1;
        }
        if (dir.empty()) {
            continue;
        }
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(std::string(dir)) / std::string(exeName);
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
            return true;
        }
    }
    return false;
}

inline std::string detectFastLinkerFlag(std::string_view compilerCmd) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)compilerCmd;
    return "";
#else
    if (compilerCmd.empty() || compilerCmd.find("zig") != std::string_view::npos) {
        return "";
    }
    static std::unordered_map<std::string, std::string> cache;
    const std::string key(compilerCmd);
    const auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    const std::string pathEnv = getEnvVar("PATH");
    std::string detectedFlag;
    if (findExecutableInPath("mold", pathEnv)) {
        const std::string probeCmd = key + " -fuse-ld=mold -Wl,--version >/dev/null 2>&1";
        if (std::system(probeCmd.c_str()) == 0) {
            detectedFlag = "-fuse-ld=mold";
        }
    }
    if (detectedFlag.empty() && findExecutableInPath("ld.lld", pathEnv)) {
        const std::string probeCmd = key + " -fuse-ld=lld -Wl,--version >/dev/null 2>&1";
        if (std::system(probeCmd.c_str()) == 0) {
            detectedFlag = "-fuse-ld=lld";
        }
    }

    cache[key] = detectedFlag;
    return detectedFlag;
#endif
}

inline bool isMacOsTarget(const TargetSpec& targetSpec) {
    if (!targetSpec.isCross) {
        return hostIsMacOs();
    }
    if (targetSpec.canonicalName.rfind("macos", 0) == 0) {
        return true;
    }
    if (targetSpec.zigTriple.find("macos") != std::string::npos) {
        return true;
    }
    return false;
}

inline std::string resolvePortableRuntimeFlags(
    std::string_view compilerCmd,
    const TargetSpec& targetSpec,
    bool staticLink
) {
    if (isMacOsTarget(targetSpec)) {
        return "";
    }
    if (staticLink) {
        return "-static";
    }
    if (compilerCmd.empty() || compilerCmd.find("zig") != std::string_view::npos) {
        return "";
    }
    if (targetSpec.isWindows) {
        return "-static-libstdc++ -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic";
    }
#if defined(_WIN32) || defined(__APPLE__)
    return "-static-libstdc++ -static-libgcc";
#else
    static std::unordered_map<std::string, std::string> cache;
    const std::string key(compilerCmd);
    const auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    std::string detectedFlags;
    const std::string probeCmd = key + " -static-libstdc++ -static-libgcc -Wl,--version >/dev/null 2>&1";
    if (std::system(probeCmd.c_str()) == 0) {
        detectedFlags = "-static-libstdc++ -static-libgcc";
    }
    cache[key] = detectedFlags;
    return detectedFlags;
#endif
}

inline std::string buildCompileCommand(
    std::string_view compilerCmd,
    std::string_view cbRoot,
    std::string_view inputParentDir,
    std::string_view outCppPath,
    std::string_view binPath,
    const std::vector<std::string>& linkerFlags,
    std::string_view optLevel = "0",
    std::string_view pchHeaderPath = "",
    bool staticLink = false,
    const TargetSpec& targetSpec = resolveTargetSpec("native")
) {
    std::string resolvedOpt = "0";
    if (!optLevel.empty()) {
        resolvedOpt = std::string(optLevel);
    }
    std::string fastLdFlags;
    const std::string fastLd = detectFastLinkerFlag(compilerCmd);
    if (!fastLd.empty()) {
        fastLdFlags = " " + fastLd;
    }
    std::string runtimeFlags;
    const std::string resolvedRuntime = resolvePortableRuntimeFlags(compilerCmd, targetSpec, staticLink);
    if (!resolvedRuntime.empty()) {
        runtimeFlags = " " + resolvedRuntime;
    }
    const std::string q(1, '"');
    std::string pchFlags;
    if (!pchHeaderPath.empty()) {
        pchFlags = " -Winvalid-pch -include " + q + std::string(pchHeaderPath) + q;
    }
    std::string cmd = std::string(compilerCmd) +
        " -O" + resolvedOpt + " -std=c++17 -pthread" +
        fastLdFlags +
        runtimeFlags +
        pchFlags +
        " -I" + q + std::string(cbRoot) + "/vendor" + q +
        " -I" + q + std::string(cbRoot) + "/stdlib" + q +
        " -I" + q + std::string(inputParentDir) + q +
        " " + q + std::string(outCppPath) + q +
        " -o " + q + std::string(binPath) + q;
    for (const auto& flag : linkerFlags) {
        cmd += " " + flag;
    }
    return cmd;
}

struct ModuleLoaderState {
    std::string rootBaseDir = ".";
    std::unordered_map<std::string, std::string> openBuffers;
    std::vector<std::string> loadingStack;
    std::vector<std::string> loadingPathStack;
    std::unordered_set<std::string> loadedModules;
    std::vector<DeclId> mergedDecls;
    DiagnosticError error;
    bool hasError = false;
};
/**
 * Module Loading and Pipeline Orchestration
 * 
 * loadModuleDeclarationsRecursive performs a topological DFS of the module graph,
 * deduplicating diamond dependencies via loader.loadedModules and detecting
 * circular imports via loader.loadingStack and loader.loadingPathStack.
 * 
 * loadAndParseModuleFile encapsulates the candidate resolution (relative to 
 * importer vs. root), in-memory overlay check, file I/O, tokenization, 
 * and arena-based parsing.
 */

inline bool isInStack(const std::vector<std::string>& stack, std::string_view item) {
    for (const auto& s : stack) {
        if (s == item) {
            return true;
        }
    }
    return false;
}

inline std::vector<std::string> buildModuleCandidatePaths(
    std::string_view currentDir,
    std::string_view rootBaseDir,
    std::string_view impTarget
) {
    std::vector<std::string> candidates;
    if (isStdImportTarget(impTarget)) {
        const std::string stdSub = stripCbExt(impTarget.substr(4));
        const std::string cbRoot = cleanbasicRootDir();
        candidates.push_back(cbRoot + "/stdlib/" + stdSub + "/" + stdSub + ".cb");
        candidates.push_back(cbRoot + "/stdlib/" + stdSub + ".cb");
        candidates.push_back("stdlib/" + stdSub + "/" + stdSub + ".cb");
        candidates.push_back("stdlib/" + stdSub + ".cb");
    } else {
        candidates.push_back(std::string(currentDir) + "/" + std::string(impTarget));
        candidates.push_back(std::string(rootBaseDir) + "/" + std::string(impTarget));
        candidates.push_back(std::string(impTarget));
    }
    return candidates;
}

inline std::optional<std::string> findInMemorySource(
    const std::unordered_map<std::string, std::string>& openBuffers,
    std::string_view candidatePath
) {
    if (openBuffers.empty()) {
        return std::nullopt;
    }
    const std::string exactKey(candidatePath);
    const auto exactIt = openBuffers.find(exactKey);
    if (exactIt != openBuffers.end()) {
        return exactIt->second;
    }
    const std::string normCandidate = normalizeRelPath(candidatePath);
    if (!normCandidate.empty()) {
        const auto normIt = openBuffers.find(normCandidate);
        if (normIt != openBuffers.end()) {
            return normIt->second;
        }
    }
    for (const auto& kv : openBuffers) {
        if (!normCandidate.empty() && normalizeRelPath(kv.first) == normCandidate) {
            return kv.second;
        }
    }
    return std::nullopt;
}

struct ModuleLoadResult {
    bool found = false;
    size_t candidateIndex = 0;
    std::string source;
    std::string candidatePath;
    Program program;
    DiagnosticError error;
    bool parseOk = false;
};

inline ModuleLoadResult loadAndParseModuleFile(
    AstArena& arena,
    const std::vector<std::string>& candidates,
    const std::unordered_map<std::string, std::string>& openBuffers
) {
    ModuleLoadResult res;
    for (size_t i = 0; i < candidates.size(); ++i) {
        res.candidatePath = candidates[i];
        auto memSource = findInMemorySource(openBuffers, res.candidatePath);
        std::ifstream modIn;
        if (!memSource.has_value()) {
            modIn.open(res.candidatePath);
        }
        if (memSource.has_value() || modIn.is_open()) {
            res.found = true;
            res.candidateIndex = i;
            if (memSource.has_value()) {
                res.source = *memSource;
            } else {
                std::ostringstream ss;
                ss << modIn.rdbuf();
                res.source = ss.str();
            }
            if (modIn.is_open()) modIn.close();

            const auto lex = tokenize(res.source, res.candidatePath);
            if (!lex.ok) {
                res.error = lex.error;
                return res;
            }
            auto parse = parseTokensWithArena(lex.tokens, std::move(arena), res.candidatePath);
            arena = std::move(parse.arena);
            if (!parse.ok) {
                res.error = parse.error;
                return res;
            }
            res.parseOk = true;
            res.program = parse.program;
            return res;
        }
    }
    return res;
}

inline void loadModuleDeclarationsRecursive(
    AstArena& arena,
    const std::vector<DeclId>& declIds,
    std::string_view currentModuleId,
    std::string_view currentFilePath,
    ModuleLoaderState& loader
) {
    if (loader.hasError) {
        return;
    }

    const std::string currentDir = parentDirOf(currentFilePath);

    for (const DeclId did : declIds) {
        if (did >= arena.decls.size()) {
            continue;
        }
        arena.decls[did].moduleId = std::string(currentModuleId);
        arena.decls[did].fileName = std::string(currentFilePath);

        if (auto* imp = arena.getDeclMut<ImportDecl>(did)) {
            const std::string impTarget = imp->target;
            const bool isCbFile = impTarget.size() > 3 && impTarget.substr(impTarget.size() - 3) == ".cb";
            const bool isStdMod = isStdImportTarget(impTarget);
            if (imp->kind == "module" && (isCbFile || isStdMod)) {
                std::string targetModuleId = resolveModuleId(currentModuleId, impTarget);
                const std::vector<std::string> candidates = buildModuleCandidatePaths(currentDir, loader.rootBaseDir, impTarget);
                
                // Check for existence and circularity before parsing to avoid redundant work
                std::string candidatePath = candidates.front();
                bool found = false;
                for (size_t cIdx = 0; cIdx < candidates.size(); ++cIdx) {
                    if (findInMemorySource(loader.openBuffers, candidates[cIdx]).has_value() || std::ifstream(candidates[cIdx]).is_open()) {
                        candidatePath = candidates[cIdx];
                        if (!isStdMod && cIdx > 0) targetModuleId = resolveModuleId("", impTarget);
                        found = true;
                        break;
                    }
                }
                imp->resolvedModuleId = targetModuleId;
                const std::string normCandidate = normalizeRelPath(candidatePath);

                if (found && (isInStack(loader.loadingStack, targetModuleId) || isInStack(loader.loadingPathStack, normCandidate))) {
                    loader.hasError = true;
                    loader.error = makeError("ImportError", "Circular module import detected when importing '" + impTarget + "' (module '" + targetModuleId + "').",
                        std::string(currentFilePath), arena.decls[did].line, arena.decls[did].col);
                    return;
                }

                if (found && loader.loadedModules.find(targetModuleId) == loader.loadedModules.end()) {
                    auto loadRes = loadAndParseModuleFile(arena, candidates, loader.openBuffers);
                    if (!loadRes.found) return; // Should not happen given 'found' check
                    if (!loadRes.parseOk) {
                        loader.hasError = true;
                        loader.error = loadRes.error;
                        return;
                    }
                    attachDocCommentsToDecls(arena, loadRes.program.declarations, loadRes.source);
                    loader.loadingStack.push_back(targetModuleId);
                    loader.loadingPathStack.push_back(normCandidate);
                    loadModuleDeclarationsRecursive(arena, loadRes.program.declarations, targetModuleId, loadRes.candidatePath, loader);
                    loader.loadingPathStack.pop_back();
                    loader.loadingStack.pop_back();
                    if (loader.hasError) return;
                    loader.loadedModules.insert(targetModuleId);
                }
            }
        }
        loader.mergedDecls.push_back(did);
    }
}

inline void preloadStdStrPrelude(
    AstArena& arena,
    std::string_view rootFileName,
    ModuleLoaderState& loader
) {
    const std::string normRoot = normalizeRelPath(rootFileName);
    if (normRoot == "stdlib/str/str.cb" ||
        (normRoot.size() >= 18 && normRoot.substr(normRoot.size() - 18) == "/stdlib/str/str.cb")) {
        return;
    }
    const std::string targetModuleId = "std/str";
    if (loader.loadedModules.find(targetModuleId) != loader.loadedModules.end()) {
        return;
    }
    const std::vector<std::string> candidates = buildModuleCandidatePaths(loader.rootBaseDir, loader.rootBaseDir, targetModuleId);
    auto loadRes = loadAndParseModuleFile(arena, candidates, loader.openBuffers);
    if (!loadRes.found) return;
    if (!loadRes.parseOk) {
        loader.hasError = true;
        loader.error = loadRes.error;
        return;
    }
    attachDocCommentsToDecls(arena, loadRes.program.declarations, loadRes.source);
    const std::string normCandidate = normalizeRelPath(loadRes.candidatePath);
    loader.loadingStack.push_back(targetModuleId);
    loader.loadingPathStack.push_back(normCandidate);
    loadModuleDeclarationsRecursive(arena, loadRes.program.declarations, targetModuleId, loadRes.candidatePath, loader);
    loader.loadingPathStack.pop_back();
    loader.loadingStack.pop_back();
    if (!loader.hasError) {
        loader.loadedModules.insert(targetModuleId);
    }
}

inline CheckSourceResult checkSource(
    std::string_view source,
    std::string_view fileName = "main.cb",
    const std::unordered_map<std::string, std::string>& openBuffers = {}
) {
    BuildTimings timings{};
    const auto t0 = std::chrono::steady_clock::now();

    const auto lexRes = tokenize(source, fileName);
    if (!lexRes.ok) {
        const auto t1 = std::chrono::steady_clock::now();
        timings.lexParseMs = elapsedMillis(t0, t1);
        timings.transpileTotalMs = timings.lexParseMs;
        timings.totalMs = timings.lexParseMs;
        return CheckSourceResult{false, {}, {}, {}, lexRes.error, timings};
    }

    auto parseRes = parseTokens(lexRes.tokens, fileName);
    if (!parseRes.ok) {
        const auto t1 = std::chrono::steady_clock::now();
        timings.lexParseMs = elapsedMillis(t0, t1);
        timings.transpileTotalMs = timings.lexParseMs;
        timings.totalMs = timings.lexParseMs;
        return CheckSourceResult{false, {}, {}, {}, parseRes.error, timings};
    }
    attachDocCommentsToDecls(parseRes.arena, parseRes.program.declarations, source);
    const auto tLexParseEnd = std::chrono::steady_clock::now();
    timings.lexParseMs = elapsedMillis(t0, tLexParseEnd);

    ModuleLoaderState loader{};
    loader.rootBaseDir = parentDirOf(fileName);
    loader.openBuffers = openBuffers;
    const std::string normRoot = normalizeRelPath(fileName);
    if (!normRoot.empty()) {
        loader.loadingPathStack.push_back(normRoot);
    }

    preloadStdStrPrelude(parseRes.arena, fileName, loader);
    if (loader.hasError) {
        const auto tModEnd = std::chrono::steady_clock::now();
        timings.moduleLoadMs = elapsedMillis(tLexParseEnd, tModEnd);
        timings.transpileTotalMs = timings.lexParseMs + timings.moduleLoadMs;
        timings.totalMs = timings.transpileTotalMs;
        return CheckSourceResult{false, {}, {}, {}, loader.error, timings};
    }

    loadModuleDeclarationsRecursive(
        parseRes.arena,
        parseRes.program.declarations,
        "",
        fileName,
        loader
    );
    const auto tModEnd = std::chrono::steady_clock::now();
    timings.moduleLoadMs = elapsedMillis(tLexParseEnd, tModEnd);
    if (loader.hasError) {
        timings.transpileTotalMs = timings.lexParseMs + timings.moduleLoadMs;
        timings.totalMs = timings.transpileTotalMs;
        return CheckSourceResult{false, {}, {}, {}, loader.error, timings};
    }
    parseRes.program.declarations = std::move(loader.mergedDecls);

    auto anaRes = analyzeProgram(parseRes.arena, parseRes.program);
    const auto tAnalyzeEnd = std::chrono::steady_clock::now();
    timings.analyzeMs = elapsedMillis(tModEnd, tAnalyzeEnd);
    timings.transpileTotalMs = timings.lexParseMs + timings.moduleLoadMs + timings.analyzeMs;
    timings.totalMs = timings.transpileTotalMs;
    if (!anaRes.ok) {
        return CheckSourceResult{false, {}, {}, {}, anaRes.error, timings};
    }

    return CheckSourceResult{
        true,
        std::move(parseRes.arena),
        std::move(parseRes.program),
        std::move(anaRes.symbols),
        {},
        timings
    };
}

inline TranspileResult transpileSource(
    std::string_view source,
    std::string_view fileName = "main.cb",
    const std::unordered_map<std::string, std::string>& openBuffers = {}
) {
    auto checkRes = checkSource(source, fileName, openBuffers);
    if (!checkRes.ok) {
        return TranspileResult{false, "", {}, checkRes.error, checkRes.timings};
    }

    const auto tEmitStart = std::chrono::steady_clock::now();
    std::string cppCode = emitProgram(checkRes.arena, checkRes.symbols, checkRes.program);
    const auto tEmitEnd = std::chrono::steady_clock::now();

    BuildTimings timings = checkRes.timings;
    timings.emitMs = elapsedMillis(tEmitStart, tEmitEnd);
    timings.transpileTotalMs = timings.lexParseMs + timings.moduleLoadMs + timings.analyzeMs + timings.emitMs;
    timings.totalMs = timings.transpileTotalMs;
    return TranspileResult{true, std::move(cppCode), std::move(checkRes.symbols), {}, timings};
}

inline BuildFileResult buildFileWithOptions(
    const std::string& inputPath,
    const BuildOptions& options
) {
    const TargetSpec targetSpec = resolveTargetSpec(options.target);
    if (!targetSpec.ok) {
        BuildFileResult res{};
        res.ok = false;
        res.error = makeError("TargetError", targetSpec.errorMessage, inputPath, 1, 1);
        return res;
    }

    std::ifstream inFile(inputPath);
    if (!inFile.is_open()) {
        BuildFileResult res{};
        res.ok = false;
        res.error = makeError("FileNotFoundError", "Source file not found: " + inputPath, inputPath, 1, 1);
        return res;
    }

    std::ostringstream sstr;
    sstr << inFile.rdbuf();
    inFile.close();
    const std::string source = sstr.str();

    const std::string parentDir = parentDirOf(inputPath);

    const TranspileResult transRes = transpileSource(source, inputPath);
    if (!transRes.ok) {
        BuildFileResult res{};
        res.ok = false;
        res.error = transRes.error;
        res.timings = transRes.timings;
        return res;
    }

    BuildTimings timings = transRes.timings;

    std::string outCpp = parentDir + "/build.cpp";
    if (!options.outputCppPath.empty()) {
        outCpp = options.outputCppPath;
    }
    std::ofstream outFile(outCpp);
    if (!outFile.is_open()) {
        BuildFileResult res{};
        res.ok = false;
        res.error = makeError("IOError", "Failed to open output file: " + outCpp, outCpp, 1, 1);
        res.timings = timings;
        return res;
    }
    outFile << transRes.cppCode;
    outFile.close();

    std::vector<std::string> linkerFlags;
    linkerFlags.reserve(transRes.symbols.linkLibs.size());
    for (const auto& lib : transRes.symbols.linkLibs) {
        linkerFlags.push_back("-l" + lib);
    }

    std::string optLevel = options.optLevel;
    if (optLevel.empty()) {
        if (options.isRelease) {
            optLevel = "3";
        } else {
            optLevel = "0";
        }
    }

    std::string binPath;
    std::string compileCmd;
    if (options.compileBinary) {
        binPath = resolveBinaryOutputPath(parentDir, options.binaryOutput, targetSpec);
        const std::string cbRoot = cleanbasicRootDir();
        const std::string compilerCmd = resolveCompilerCommand(targetSpec, options.compiler);
        const PchPrepareResult pchRes = ensureBackendPch(
            compilerCmd,
            targetSpec,
            optLevel,
            cbRoot,
            options.usePch,
            options.rebuildPch
        );
        timings.pchBuildMs = pchRes.buildMs;
        timings.pchUsed = pchRes.used;
        timings.pchCacheHit = pchRes.cacheHit;

        compileCmd = buildCompileCommand(
            compilerCmd,
            cbRoot,
            parentDir,
            outCpp,
            binPath,
            linkerFlags,
            optLevel,
            pchRes.headerPath,
            options.staticLink,
            targetSpec
        );
        const auto tCompileStart = std::chrono::steady_clock::now();
        int ret = std::system(compileCmd.c_str());
        const auto tCompileEnd = std::chrono::steady_clock::now();
        timings.cppCompileMs = elapsedMillis(tCompileStart, tCompileEnd);

        if (ret != 0 && pchRes.used) {
            std::error_code ec;
            std::filesystem::remove(pchRes.headerPath + ".gch", ec);
            timings.pchUsed = false;
            timings.pchCacheHit = false;
            compileCmd = buildCompileCommand(
                compilerCmd,
                cbRoot,
                parentDir,
                outCpp,
                binPath,
                linkerFlags,
                optLevel,
                "",
                options.staticLink,
                targetSpec
            );
            const auto tRetryStart = std::chrono::steady_clock::now();
            ret = std::system(compileCmd.c_str());
            const auto tRetryEnd = std::chrono::steady_clock::now();
            timings.cppCompileMs += elapsedMillis(tRetryStart, tRetryEnd);
        }

        if (ret != 0 && options.staticLink && !targetSpec.isCross && !targetSpec.isWindows && !isMacOsTarget(targetSpec)) {
            std::string retryPchHeader;
            if (timings.pchUsed) {
                retryPchHeader = pchRes.headerPath;
            }
            compileCmd = buildCompileCommand(
                compilerCmd,
                cbRoot,
                parentDir,
                outCpp,
                binPath,
                linkerFlags,
                optLevel,
                retryPchHeader,
                false,
                targetSpec
            );
            const auto tFallbackStart = std::chrono::steady_clock::now();
            ret = std::system(compileCmd.c_str());
            const auto tFallbackEnd = std::chrono::steady_clock::now();
            timings.cppCompileMs += elapsedMillis(tFallbackStart, tFallbackEnd);
        }

        timings.totalMs = timings.transpileTotalMs + timings.pchBuildMs + timings.cppCompileMs;
        if (ret != 0) {
            BuildFileResult res{};
            res.ok = false;
            res.outputCppPath = outCpp;
            res.outputBinaryPath = binPath;
            res.compileCommand = compileCmd;
            res.linkerFlags = linkerFlags;
            res.timings = timings;
            res.error = makeError(
                "CompilationError",
                "C++ compilation failed with code " + std::to_string(ret) + " (command: " + compilerCmd + ")",
                outCpp,
                1,
                1
            );
            return res;
        }
    }

    BuildFileResult res{};
    res.ok = true;
    res.outputCppPath = outCpp;
    res.outputBinaryPath = binPath;
    res.compileCommand = compileCmd;
    res.linkerFlags = std::move(linkerFlags);
    res.timings = timings;
    return res;
}

inline BuildFileResult buildFile(
    const std::string& inputPath,
    const std::string& outputPath = "",
    bool compileBinary = false,
    const std::string& binaryOutput = "",
    const std::string& target = "",
    const std::string& compiler = "",
    const std::string& optLevel = "0",
    bool isRelease = false,
    bool usePch = true,
    bool rebuildPch = false,
    bool staticLink = false
) {
    BuildOptions opts{};
    opts.outputCppPath = outputPath;
    opts.compileBinary = compileBinary;
    opts.binaryOutput = binaryOutput;
    opts.target = target;
    opts.compiler = compiler;
    opts.optLevel = optLevel;
    opts.isRelease = isRelease;
    opts.usePch = usePch;
    opts.rebuildPch = rebuildPch;
    opts.staticLink = staticLink;
    return buildFileWithOptions(inputPath, opts);
}

}  // namespace cb

#endif  // CLEANBASIC_PIPELINE_HPP
