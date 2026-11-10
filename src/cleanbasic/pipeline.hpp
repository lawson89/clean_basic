#ifndef CLEANBASIC_PIPELINE_HPP
#define CLEANBASIC_PIPELINE_HPP

#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cb {

struct CheckSourceResult {
    bool ok = false;
    AstArena arena;
    Program program;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

struct TranspileResult {
    bool ok = false;
    std::string cppCode;
    GlobalSymbolTable symbols;
    DiagnosticError error;
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
};

struct BuildFileResult {
    bool ok = false;
    std::string outputCppPath;
    std::string outputBinaryPath;
    std::string compileCommand;
    std::vector<std::string> linkerFlags;
    DiagnosticError error;
};

inline bool hostIsWindows() {
#if defined(_WIN32)
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

inline std::string buildCompileCommand(
    std::string_view compilerCmd,
    std::string_view cbRoot,
    std::string_view inputParentDir,
    std::string_view outCppPath,
    std::string_view binPath,
    const std::vector<std::string>& linkerFlags
) {
    const std::string q(1, '"');
    std::string cmd = std::string(compilerCmd) +
        " -O3 -std=c++17 -pthread" +
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
    const auto lexRes = tokenize(source, fileName);
    if (!lexRes.ok) {
        return CheckSourceResult{false, {}, {}, {}, lexRes.error};
    }

    auto parseRes = parseTokens(lexRes.tokens, fileName);
    if (!parseRes.ok) {
        return CheckSourceResult{false, {}, {}, {}, parseRes.error};
    }
    attachDocCommentsToDecls(parseRes.arena, parseRes.program.declarations, source);

    ModuleLoaderState loader{};
    loader.rootBaseDir = parentDirOf(fileName);
    loader.openBuffers = openBuffers;
    const std::string normRoot = normalizeRelPath(fileName);
    if (!normRoot.empty()) {
        loader.loadingPathStack.push_back(normRoot);
    }

    preloadStdStrPrelude(parseRes.arena, fileName, loader);
    if (loader.hasError) {
        return CheckSourceResult{false, {}, {}, {}, loader.error};
    }

    loadModuleDeclarationsRecursive(
        parseRes.arena,
        parseRes.program.declarations,
        "",
        fileName,
        loader
    );
    if (loader.hasError) {
        return CheckSourceResult{false, {}, {}, {}, loader.error};
    }
    parseRes.program.declarations = std::move(loader.mergedDecls);

    auto anaRes = analyzeProgram(parseRes.arena, parseRes.program);
    if (!anaRes.ok) {
        return CheckSourceResult{false, {}, {}, {}, anaRes.error};
    }

    return CheckSourceResult{
        true,
        std::move(parseRes.arena),
        std::move(parseRes.program),
        std::move(anaRes.symbols),
        {}
    };
}

inline TranspileResult transpileSource(
    std::string_view source,
    std::string_view fileName = "main.cb",
    const std::unordered_map<std::string, std::string>& openBuffers = {}
) {
    auto checkRes = checkSource(source, fileName, openBuffers);
    if (!checkRes.ok) {
        return TranspileResult{false, "", {}, checkRes.error};
    }

    std::string cppCode = emitProgram(checkRes.arena, checkRes.symbols, checkRes.program);
    return TranspileResult{true, std::move(cppCode), std::move(checkRes.symbols), {}};
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
        return res;
    }

    std::string outCpp = parentDir + "/build.cpp";
    if (!options.outputCppPath.empty()) {
        outCpp = options.outputCppPath;
    }
    std::ofstream outFile(outCpp);
    if (!outFile.is_open()) {
        BuildFileResult res{};
        res.ok = false;
        res.error = makeError("IOError", "Failed to open output file: " + outCpp, outCpp, 1, 1);
        return res;
    }
    outFile << transRes.cppCode;
    outFile.close();

    std::vector<std::string> linkerFlags;
    linkerFlags.reserve(transRes.symbols.linkLibs.size());
    for (const auto& lib : transRes.symbols.linkLibs) {
        linkerFlags.push_back("-l" + lib);
    }

    std::string binPath;
    std::string compileCmd;
    if (options.compileBinary) {
        binPath = resolveBinaryOutputPath(parentDir, options.binaryOutput, targetSpec);
        const std::string cbRoot = cleanbasicRootDir();
        const std::string compilerCmd = resolveCompilerCommand(targetSpec, options.compiler);
        compileCmd = buildCompileCommand(
            compilerCmd,
            cbRoot,
            parentDir,
            outCpp,
            binPath,
            linkerFlags
        );
        const int ret = std::system(compileCmd.c_str());
        if (ret != 0) {
            BuildFileResult res{};
            res.ok = false;
            res.outputCppPath = outCpp;
            res.outputBinaryPath = binPath;
            res.compileCommand = compileCmd;
            res.linkerFlags = linkerFlags;
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
    return res;
}

inline BuildFileResult buildFile(
    const std::string& inputPath,
    const std::string& outputPath = "",
    bool compileBinary = false,
    const std::string& binaryOutput = "",
    const std::string& target = "",
    const std::string& compiler = ""
) {
    BuildOptions opts{};
    opts.outputCppPath = outputPath;
    opts.compileBinary = compileBinary;
    opts.binaryOutput = binaryOutput;
    opts.target = target;
    opts.compiler = compiler;
    return buildFileWithOptions(inputPath, opts);
}

}  // namespace cb

#endif  // CLEANBASIC_PIPELINE_HPP
