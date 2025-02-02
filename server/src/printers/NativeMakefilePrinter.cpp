#include <Paths.h>
#include <FeaturesFilter.h>
#include <algorithm>
#include "NativeMakefilePrinter.h"
#include "Synchronizer.h"

#include "building/RunCommand.h"
#include "environment/EnvironmentPaths.h"
#include "exceptions/UnImplementedException.h"
#include "RelativeMakefilePrinter.h"
#include "utils/ArgumentsUtils.h"
#include "utils/CompilationUtils.h"
#include "utils/DynamicLibraryUtils.h"
#include "utils/LinkerUtils.h"
#include "utils/SanitizerUtils.h"
#include "utils/StringUtils.h"

namespace printer {
    using namespace DynamicLibraryUtils;
    using StringUtils::stringFormat;

    static const std::string STUB_OBJECT_FILES_NAME = "STUB_OBJECT_FILES";
    static const std::string STUB_OBJECT_FILES = "$(STUB_OBJECT_FILES)";

    static const std::string FPIC_FLAG = "-fPIC";
    static const std::vector<std::string> SANITIZER_NEEDED_FLAGS = {
        "-g", "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls"
    };
    static const std::string STATIC_FLAG = "-static";
    static const std::string SHARED_FLAG = "-shared";
    static const std::string RELOCATE_FLAG = "-r";
    static const std::string OPTIMIZATION_FLAG = "-O0";
    static const std::unordered_set<std::string> UNSUPPORTED_FLAGS_AND_OPTIONS_TEST_MAKE = {
        // See https://gcc.gnu.org/onlinedocs/gcc/Option-Summary.html
        "-ansi",
        "-fallow-parameterless-variadic-functions",
        "-fallow-single-precision",
        "-fcond-mismatch",
        "-ffreestanding",
        "-fgnu89-inline",
        "-fhosted",
        "-flax-vector-conversions",
        "-fms-extensions",
        "-fno-asm",
        "-fno-builtin",
        "-fno-builtin-function",
        "-fgimple",
        "-fopenacc",
        "-fopenacc-dim",
        "-fopenacc-kernels",
        "-fopenmp",
        "-fopenmp-simd",
        "-fpermitted-flt-eval-methods",
        "-fplan9-extensions",
        "-fsigned-bitfields",
        "-fsigned-char",
        "-fsso-struct",
        "-funsigned-bitfields",
        "-funsigned-char",
        "-std",
    };

    static void eraseIfWlOnly(std::string &argument) {
        if (argument == "-Wl") {
            argument = "";
        }
    }

    static void removeLinkerFlag(std::string &argument, std::string const &flag) {
        auto options = StringUtils::split(argument, ',');
        size_t erased = CollectionUtils::erase_if(options, [&flag](std::string const &option) {
            return StringUtils::startsWith(option, flag);
        });
        if (erased == 0) {
            return;
        }
        argument = StringUtils::joinWith(options, ",");
        eraseIfWlOnly(argument);
    }

    // transforms -Wl,<arg>,<arg2>... to <arg> <arg2>...
    // https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-wl-arg-arg2
    static void transformCompilerFlagsToLinkerFlags(std::string &argument) {
        auto options = StringUtils::split(argument, ',');
        if (options.empty()) {
            return;
        }
        if (options.front() != "-Wl") {
            return;
        }
        CollectionUtils::erase(options, options.front());
        argument = StringUtils::joinWith(options, " ");
    }

    static void removeScriptFlag(std::string &argument) {
        removeLinkerFlag(argument, "--version-script");
    }

    static void removeSonameFlag(std::string &argument) {
        auto options = StringUtils::split(argument, ',');
        bool isSonameNext = false;
        std::vector<std::string> result;
        for (std::string const &option : options) {
            if (option == "-soname") {
                isSonameNext = true;
                continue;
            }
            if (isSonameNext) {
                isSonameNext = false;
                continue;
            }
            result.push_back(option);
        }
        argument = StringUtils::joinWith(result, ",");
        eraseIfWlOnly(argument);
    }

    NativeMakefilePrinter::NativeMakefilePrinter(
        utbot::ProjectContext projectContext,
        std::shared_ptr<BuildDatabase> buildDatabase,
        fs::path const &rootPath,
        fs::path primaryCompiler,
        CollectionUtils::FileSet const *stubSources,
        std::map<std::string, fs::path, std::function<bool(const std::string&, const std::string&)>> pathToShellVariable)
        : RelativeMakefilePrinter(pathToShellVariable),
          projectContext(std::move(projectContext)), buildDatabase(buildDatabase), rootPath(std::move(rootPath)),
          primaryCompiler(std::move(primaryCompiler)),
          primaryCxxCompiler(CompilationUtils::toCppCompiler(this->primaryCompiler)),
          primaryCompilerName(CompilationUtils::getCompilerName(this->primaryCompiler)),
          primaryCxxCompilerName(CompilationUtils::getCompilerName(primaryCxxCompiler)),
          cxxLinker(CompilationUtils::toCppLinker(primaryCxxCompiler)),

          pthreadFlag(CompilationUtils::getPthreadFlag(primaryCxxCompilerName)),
          coverageLinkFlags(StringUtils::joinWith(
              CompilationUtils::getCoverageLinkFlags(primaryCxxCompilerName), " ")),
          sanitizerLinkFlags(SanitizerUtils::getSanitizeLinkFlags(primaryCxxCompilerName)),

          buildDirectory(Paths::getUtbotBuildDir(projectContext)),
          dependencyDirectory(buildDirectory / "dependencies"),
          stubSources(stubSources) {

        artifacts = { getRelativePath(buildDirectory), getRelativePath(dependencyDirectory) };
        init();
    }

    void NativeMakefilePrinter::init() {

        declareAction(stringFormat("$(shell mkdir -p %s >/dev/null)", getRelativePath(buildDirectory)));
        declareAction(stringFormat("$(shell mkdir -p %s >/dev/null)",
                                   getRelativePath(dependencyDirectory)));
        declareTarget(FORCE, {}, {});

        comment("gtest");

        fs::path gtestBuildDirectory = getRelativePath(buildDirectory / "googletest");
        fs::path defaultPath = "default.c";
        std::vector<std::string> defaultGtestCompileCommandLine{ getRelativePathForLinker(primaryCxxCompiler), "-c", "-std=c++11",
                                                            FPIC_FLAG, defaultPath };
        utbot::CompileCommand defaultGtestCompileCommand{ defaultGtestCompileCommandLine,
                                                          getRelativePath(buildDirectory), defaultPath };
        gtestAllTargets(defaultGtestCompileCommand, gtestBuildDirectory);
        gtestMainTargets(defaultGtestCompileCommand, gtestBuildDirectory);
        comment("/gtest");
    }

    fs::path NativeMakefilePrinter::getTemporaryDependencyFile(fs::path const &file) {
        fs::path relativePath = fs::relative(file, projectContext.projectPath);
        return getRelativePath(dependencyDirectory) /
               Paths::addExtension(relativePath, ".Td");
    }

    fs::path NativeMakefilePrinter::getDependencyFile(fs::path const &file) {
        fs::path relativePath = fs::relative(file, projectContext.projectPath);
        return getRelativePath(dependencyDirectory) /
               Paths::addExtension(relativePath, ".d");
    }

    void NativeMakefilePrinter::gtestAllTargets(const utbot::CompileCommand &defaultCompileCommand,
                                                const fs::path &gtestBuildDir) {
        const fs::path gtestLib = Paths::getGtestLibPath();
        fs::path gtestAllSourceFile = gtestLib / "googletest" / "src" / "gtest-all.cc";
        fs::path gtestAllObjectFile = gtestBuildDir / "gtest-all.cc.o";

        auto gtestCompilationArguments = defaultCompileCommand;
        gtestCompilationArguments.setSourcePath(getRelativePath(gtestAllSourceFile));
        gtestCompilationArguments.setOutput(gtestAllObjectFile);
        gtestCompilationArguments.addFlagsToBegin(
            { stringFormat("-I%s", getRelativePath(gtestLib) / "googletest" / "include"),
              stringFormat("-I%s", getRelativePath(gtestLib) / "googletest") });

        declareTarget(gtestAllObjectFile, { gtestCompilationArguments.getSourcePath() },
                      { gtestCompilationArguments.toStringWithChangingDirectory() });
        declareShellVariable("GTEST_ALL", gtestAllObjectFile,
                             [&](const std::string& arg1, const std::string& arg2) {
            declareVariable(arg1, arg2);
        });

        artifacts.push_back(gtestAllObjectFile);
    }

    void NativeMakefilePrinter::gtestMainTargets(const utbot::CompileCommand &defaultCompileCommand,
                                                 const fs::path &gtestBuildDir) {
        const fs::path gtestLib = Paths::getGtestLibPath();
        fs::path gtestMainSourceFile = gtestLib / "googletest" / "src" / "gtest_main.cc";
        fs::path gtestMainObjectFile = gtestBuildDir / "gtest_main.cc.o";

        auto gtestCompilationArguments = defaultCompileCommand;
        gtestCompilationArguments.addFlagsToBegin(
                {stringFormat("-I%s", getRelativePath(gtestLib / "googletest" / "include")),
                stringFormat("-I%s", getRelativePath(gtestLib / "googletest"))});
        gtestCompilationArguments.setSourcePath(getRelativePath(gtestMainSourceFile));
        gtestCompilationArguments.setOutput(gtestMainObjectFile);
        declareTarget(gtestMainObjectFile, { gtestCompilationArguments.getSourcePath() },
                      { gtestCompilationArguments.toStringWithChangingDirectory() });
        declareShellVariable("GTEST_MAIN", gtestMainObjectFile,
                             [&](const std::string& arg1, const std::string& arg2) {
            declareVariable(arg1, arg2);
        });

        artifacts.push_back(gtestMainObjectFile);
    }

    void NativeMakefilePrinter::addCompileTarget(
        const fs::path &sourcePath,
        const fs::path &target,
        const BuildDatabase::ObjectFileInfo &compilationUnitInfo) {
        auto compileCommand = compilationUnitInfo.command;
        fs::path compiler = CompilationUtils::getBundledCompilerPath(
                CompilationUtils::getCompilerName(compileCommand.getCompiler()));
        fs::path cxxCompiler = CompilationUtils::toCppCompiler(compiler);
        auto compilerName = CompilationUtils::getCompilerName(compiler);
        compileCommand.setCompiler(getRelativePathForLinker(compiler));
        compileCommand.setSourcePath(getRelativePath(sourcePath));
        compileCommand.setOutput(getRelativePath(target));

        for (std::string& argument : compileCommand.getCommandLine()) {
            tryChangeToRelativePath(argument);
        }

        compileCommand.setOptimizationLevel(OPTIMIZATION_FLAG);
        compileCommand.addEnvironmentVariable("C_INCLUDE_PATH", "$UTBOT_LAUNCH_INCLUDE_PATH");
        compileCommand.addFlagToBegin(FPIC_FLAG);
        compileCommand.addFlagsToBegin(SANITIZER_NEEDED_FLAGS);
        compileCommand.addFlagsToBegin(
            CompilationUtils::getCoverageCompileFlags(primaryCompilerName));
        compileCommand.addFlagsToBegin(SanitizerUtils::getSanitizeCompileFlags(compilerName));

        fs::path temporaryDependencyFile = getTemporaryDependencyFile(sourcePath);
        fs::path dependencyFile = getDependencyFile(sourcePath);
        compileCommand.addFlagToBegin(
            stringFormat("-MT $@ -MMD -MP -MF %s", temporaryDependencyFile));
        compileCommand.addFlagToBegin(
            stringFormat("-iquote%s", getRelativePath(
                    compilationUnitInfo.getSourcePath().parent_path())));

        std::string makingDependencyDirectory =
            stringFormat("mkdir -p %s", dependencyFile.parent_path());
        std::string postCompileAction =
            stringFormat("mv -f %s %s", temporaryDependencyFile, dependencyFile);

        declareTarget(compileCommand.getOutput(), { compileCommand.getSourcePath(), dependencyFile },
                      { makingDependencyDirectory,
                        compileCommand.toStringWithChangingDirectoryToNew(
                                getRelativePath(compileCommand.getDirectory())),
                        postCompileAction });

        artifacts.push_back(compileCommand.getOutput());
    }

    BuildResult NativeMakefilePrinter::addObjectFile(const fs::path &objectFile,
                                                     const std::string &suffixForParentOfStubs) {

        auto compilationUnitInfo = buildDatabase->getClientCompilationUnitInfo(objectFile);
        fs::path sourcePath = compilationUnitInfo->getSourcePath();

        fs::path pathToCompile;
        fs::path recompiledFile;
        BuildResult::Type buildResultType;
        BuildResult buildResult;
        if (CollectionUtils::contains(*stubSources, sourcePath)) {
            pathToCompile = Paths::sourcePathToStubPath(projectContext, sourcePath);
            recompiledFile = Paths::getRecompiledFile(projectContext, pathToCompile);
            buildResultType = BuildResult::Type::ALL_STUBS;
        } else {
            if (Paths::isCXXFile(sourcePath)) {
                pathToCompile = sourcePath;
            } else {
                pathToCompile = Paths::getWrapperFilePath(projectContext, sourcePath);
            }
            recompiledFile =
                Paths::getRecompiledFile(projectContext, compilationUnitInfo->getOutputFile());
            buildResultType = BuildResult::Type::NO_STUBS;
        }

        buildResult = { recompiledFile, buildResultType };

        addCompileTarget(pathToCompile, buildResult.output, *compilationUnitInfo);
        return buildResult;
    }

    void NativeMakefilePrinter::addTestTarget(const fs::path &sourcePath) {
        auto compilationUnitInfo = buildDatabase->getClientCompilationUnitInfo(sourcePath);
        auto testCompilationCommand = compilationUnitInfo->command;
        testCompilationCommand.setCompiler(getRelativePathForLinker(primaryCxxCompiler));
        testCompilationCommand.setOptimizationLevel(OPTIMIZATION_FLAG);
        testCompilationCommand.removeCompilerFlagsAndOptions(
            UNSUPPORTED_FLAGS_AND_OPTIONS_TEST_MAKE);
        testCompilationCommand.removeIncludeFlags();
        const fs::path gtestLib = Paths::getGtestLibPath();
        testCompilationCommand.addFlagToBegin(stringFormat("-I%s", getRelativePath(gtestLib / "googletest" / "include")));
        if (Paths::isCXXFile(sourcePath)) {
            testCompilationCommand.addFlagToBegin(stringFormat("-I%s", getRelativePath(Paths::getAccessPrivateLibPath())));
        }
        testCompilationCommand.addFlagToBegin(FPIC_FLAG);
        testCompilationCommand.addFlagsToBegin(SANITIZER_NEEDED_FLAGS);

        fs::path testSourcePath = Paths::sourcePathToTestPath(projectContext, sourcePath);
        fs::path compilationDirectory = compilationUnitInfo->getDirectory();
        fs::path testObjectDir = Paths::getTestObjectDir(projectContext);
        fs::path testSourceRelativePath = fs::relative(testSourcePath, projectContext.testDirPath);
        fs::path testObjectPathRelative = getRelativePath(
                testObjectDir / Paths::addExtension(testSourceRelativePath, ".o"));
        testCompilationCommand.setOutput(
                testObjectPathRelative);
        testCompilationCommand.setSourcePath(
                getRelativePath(testSourcePath));


        declareTarget(testCompilationCommand.getOutput(), {testCompilationCommand.getSourcePath().string() },
                      { testCompilationCommand.toStringWithChangingDirectoryToNew(
                              getRelativePath(testCompilationCommand.getDirectory())) });

        artifacts.push_back(testCompilationCommand.getOutput());

        auto rootLinkUnitInfo = buildDatabase->getClientLinkUnitInfo(rootPath);
        fs::path testExecutablePath = getTestExecutablePath(sourcePath);

        std::vector<std::string> filesToLink{ "$(GTEST_MAIN)", "$(GTEST_ALL)", testCompilationCommand.getOutput(),
                                             getRelativePath(
                                                     sharedOutput.value()) };
        if (rootLinkUnitInfo->commands.front().isArchiveCommand()) {
            std::vector<std::string> dynamicLinkCommandLine{ getRelativePathForLinker(cxxLinker), "$(LDFLAGS)",
                                                            pthreadFlag, coverageLinkFlags,
                                                            sanitizerLinkFlags, "-o",
                                                            getRelativePath(
                                                                    testExecutablePath) };
            CollectionUtils::extend(dynamicLinkCommandLine, filesToLink);
            dynamicLinkCommandLine.push_back(
                    getLibraryDirectoryFlag(getRelativePath(
                            sharedOutput.value().parent_path())));
            utbot::LinkCommand dynamicLinkCommand{dynamicLinkCommandLine, getRelativePath(buildDirectory) };
            declareTarget(getRelativePath(testExecutablePath), filesToLink,
                          { dynamicLinkCommand.toStringWithChangingDirectory() });
        } else {
            utbot::LinkCommand dynamicLinkCommand = rootLinkUnitInfo->commands.front();
            dynamicLinkCommand.setLinker(cxxLinker);
            dynamicLinkCommand.setOutput(testExecutablePath);
            dynamicLinkCommand.erase_if([&](std::string const &argument) {
                return CollectionUtils::contains(rootLinkUnitInfo->files, argument) ||
                       argument == SHARED_FLAG ||
                       StringUtils::startsWith(argument, libraryDirOption) ||
                       StringUtils::startsWith(argument, linkFlag);
            });
            for (std::string &argument : dynamicLinkCommand.getCommandLine()) {
                removeScriptFlag(argument);
                removeSonameFlag(argument);
            }
            dynamicLinkCommand.setOptimizationLevel(OPTIMIZATION_FLAG);
            dynamicLinkCommand.addFlagsToBegin(
                { pthreadFlag, coverageLinkFlags, sanitizerLinkFlags });
            std::copy_if(rootLinkUnitInfo->files.begin(), rootLinkUnitInfo->files.end(), std::back_inserter(filesToLink),
                         [](const fs::path& path) {return Paths::isLibraryFile(path);});
            for(std::string& file : filesToLink) {
                if (CollectionUtils::contains(buildResults, fs::path(file))) {
                    file = buildResults.at(fs::path(file)).output.string();
                }
            }

            for (std::string &file : filesToLink) {
                tryChangeToRelativePath(file);
            }

            dynamicLinkCommand.addFlagsToBegin(filesToLink);
            dynamicLinkCommand.addFlagToBegin(
                    getLibraryDirectoryFlag(getRelativePath(
                            sharedOutput.value().parent_path())));
            dynamicLinkCommand.addFlagToBegin("$(LDFLAGS)");

            dynamicLinkCommand.setLinker(getRelativePathForLinker(cxxLinker));
            dynamicLinkCommand.setOutput(
                    getRelativePath(testExecutablePath));

            declareTarget(dynamicLinkCommand.getOutput(), filesToLink,
                          {dynamicLinkCommand.toStringWithChangingDirectoryToNew(
                                  getRelativePath(dynamicLinkCommand.getDirectory())) });
        }

        artifacts.push_back(getRelativePath(testExecutablePath));
    }
    fs::path NativeMakefilePrinter::getTestExecutablePath(const fs::path &sourcePath) const {
        return Paths::removeExtension(
            Paths::removeExtension(Paths::getRecompiledFile(projectContext, sourcePath)));
    }

    NativeMakefilePrinter::NativeMakefilePrinter(const NativeMakefilePrinter &baseMakefilePrinter,
                                                 const fs::path &sourcePath)
        : RelativeMakefilePrinter(baseMakefilePrinter.pathToShellVariable),
          projectContext(baseMakefilePrinter.projectContext),
          buildDatabase(baseMakefilePrinter.buildDatabase),
          rootPath(baseMakefilePrinter.rootPath),
          primaryCompiler(baseMakefilePrinter.primaryCompiler),
          primaryCxxCompiler(baseMakefilePrinter.primaryCxxCompiler),
          primaryCompilerName(baseMakefilePrinter.primaryCompilerName),
          primaryCxxCompilerName(baseMakefilePrinter.primaryCxxCompilerName),
          cxxLinker(baseMakefilePrinter.cxxLinker), pthreadFlag(baseMakefilePrinter.pthreadFlag),
          coverageLinkFlags(baseMakefilePrinter.coverageLinkFlags),
          sanitizerLinkFlags(baseMakefilePrinter.sanitizerLinkFlags),
          buildDirectory(baseMakefilePrinter.buildDirectory),
          dependencyDirectory(baseMakefilePrinter.dependencyDirectory),
          artifacts(baseMakefilePrinter.artifacts),
          buildResults(baseMakefilePrinter.buildResults),
          sharedOutput(baseMakefilePrinter.sharedOutput) {
        resetStream();
        ss << baseMakefilePrinter.ss.str();

        addTestTarget(sourcePath);

        fs::path testExecutablePath = getTestExecutablePath(sourcePath);

        auto rootLinkUnitInfo = buildDatabase->getClientLinkUnitInfo(rootPath);

        fs::path coverageInfoBinary = sharedOutput.value();
        if (!Paths::isLibraryFile(coverageInfoBinary)) {
            coverageInfoBinary = testExecutablePath.string();
        }

        declareTarget("bin", { FORCE }, { stringFormat("echo %s",
                                                       getRelativePath(coverageInfoBinary)) });

        utbot::RunCommand testRunCommand{ { getRelativePath(testExecutablePath), "$(GTEST_FLAGS)" },
                                          getRelativePath(buildDirectory) };
        testRunCommand.addEnvironmentVariable("PATH", "$$PATH:$(pwd)");
        if (primaryCompilerName == CompilationUtils::CompilerName::GCC) {
            testRunCommand.addEnvironmentVariable("LD_PRELOAD",
                                                  getRelativePath(Paths::getAsanLibraryPath()).string() + ":${LD_PRELOAD}");
        }
        testRunCommand.addEnvironmentVariable(SanitizerUtils::UBSAN_OPTIONS_NAME,
                                              SanitizerUtils::UBSAN_OPTIONS_VALUE);
        testRunCommand.addEnvironmentVariable(SanitizerUtils::ASAN_OPTIONS_NAME,
                                              SanitizerUtils::ASAN_OPTIONS_VALUE);

        declareTarget("build", { getRelativePath(testExecutablePath) }, {});
        declareTarget("run", { "build" },
                      { testRunCommand.toStringWithChangingDirectory() });

        close();
    }

    BuildResult
    NativeMakefilePrinter::addLinkTargetRecursively(const fs::path &unitFile,
                                                    const std::string &suffixForParentOfStubs,
                                                    bool hasParent,
                                                    bool transformExeToLib) {
        if (CollectionUtils::contains(buildResults, unitFile)) {
            return buildResults.at(unitFile);
        }
        if (Paths::isObjectFile(unitFile)) {
            auto buildResult = addObjectFile(unitFile, suffixForParentOfStubs);
            return buildResults[unitFile] = buildResult;
        }

        auto linkUnitInfo = buildDatabase->getClientLinkUnitInfo(unitFile);
        BuildResult::Type unitType = BuildResult::Type::NONE;
        CollectionUtils::MapFileTo<fs::path> fileMapping;
        auto unitBuildResults = CollectionUtils::transformTo<std::vector<BuildResult>>(
            linkUnitInfo->files, [&](fs::path const &dependency) {
                BuildResult buildResult =
                    addLinkTargetRecursively(dependency, suffixForParentOfStubs, true, transformExeToLib);
                unitType |= buildResult.type;
                fileMapping[dependency] = buildResult.output;
                return buildResult;
            });

        auto dependenciesAbsolutePaths = CollectionUtils::transformTo<CollectionUtils::FileSet>(
            unitBuildResults, [](BuildResult const &buildResult) { return buildResult.output; });

        auto dependencies = CollectionUtils::transformTo<std::vector<fs::path>>(
                dependenciesAbsolutePaths, [&](fs::path const &path) { return getRelativePath(path); });

        bool isExecutable = !Paths::isLibraryFile(unitFile);

        fs::path recompiledFile =
            Paths::getRecompiledFile(projectContext, linkUnitInfo->getOutput());
        if (isExecutable && !transformExeToLib) {
            recompiledFile = Paths::isObjectFile(recompiledFile) ?
                             recompiledFile : Paths::addExtension(recompiledFile, ".o");
        } else if (Paths::isSharedLibraryFile(unitFile) || isExecutable) {
            recompiledFile = getSharedLibrary(recompiledFile);
        }
        recompiledFile = LinkerUtils::applySuffix(recompiledFile, unitType, suffixForParentOfStubs);

        if (isExecutable || Paths::isSharedLibraryFile(unitFile)) {
            sharedOutput = recompiledFile;
        }

        auto commandActions =
            CollectionUtils::transform(linkUnitInfo->commands, [&](utbot::LinkCommand linkCommand) {
                linkCommand.erase(STATIC_FLAG);
                linkCommand.setOutput(recompiledFile);
                for (std::string &argument : linkCommand.getCommandLine()) {
                    if (CollectionUtils::contains(linkUnitInfo->files, argument)) {
                        argument = fileMapping.at(argument);
                    }
                }
                if (!linkCommand.isArchiveCommand()) {
                    if (isExecutable && !transformExeToLib) {
                        linkCommand.setLinker(Paths::getLd());
                        for (std::string &argument : linkCommand.getCommandLine()) {
                            transformCompilerFlagsToLinkerFlags(argument);
                        }
                    } else {
                        linkCommand.setLinker(CompilationUtils::getBundledCompilerPath(
                                CompilationUtils::getCompilerName(linkCommand.getLinker())));
                    }
                    std::vector <std::string> libraryDirectoriesFlags;
                    for (std::string &argument : linkCommand.getCommandLine()) {
                        removeScriptFlag(argument);
                        removeSonameFlag(argument);
                        auto optionalLibraryAbsolutePath =
                                getLibraryAbsolutePath(argument, linkCommand.getDirectory());
                        if (optionalLibraryAbsolutePath.has_value()) {
                            const fs::path &absolutePath = optionalLibraryAbsolutePath.value();
                            if (Paths::isSubPathOf(projectContext.buildDir(), absolutePath)) {
                                fs::path recompiledDir =
                                        Paths::getRecompiledFile(projectContext, absolutePath);
                                std::string directoryFlag = getLibraryDirectoryFlag(recompiledDir);
                                libraryDirectoriesFlags.push_back(directoryFlag);
                            }
                        }
                    }
                    linkCommand.addFlagsToBegin(libraryDirectoriesFlags);
                    if (!isExecutable || transformExeToLib) {
                        linkCommand.addFlagsToBegin({"-Wl,--allow-multiple-definition",
                                                     coverageLinkFlags, sanitizerLinkFlags, "-Wl,--whole-archive"});
                        if (linkCommand.isSharedLibraryCommand()) {
                            linkCommand.addFlagToEnd(STUB_OBJECT_FILES);
                            dependencies.emplace_back(STUB_OBJECT_FILES);
                        }
                        linkCommand.addFlagToEnd("-Wl,--no-whole-archive");
                        linkCommand.setOptimizationLevel(OPTIMIZATION_FLAG);
                    }
                    linkCommand.addFlagToBegin("$(LDFLAGS)");
                    if (isExecutable) {
                        linkCommand.addFlagToBegin(transformExeToLib ? SHARED_FLAG : RELOCATE_FLAG);
                    }
                }

                linkCommand.setLinker(getRelativePathForLinker(linkCommand.getLinker()));

                for (std::string &argument : linkCommand.getCommandLine()) {
                    tryChangeToRelativePath(argument);
                }

                const fs::path relativeDir = getRelativePath(linkCommand.getDirectory());

                if (isExecutable && !transformExeToLib) {
                    return stringFormat("%s && objcopy --redefine-sym main=main__ %s",
                                        linkCommand.toStringWithChangingDirectoryToNew(relativeDir),
                                        linkCommand.getOutput().string());
                }
                return linkCommand.toStringWithChangingDirectoryToNew(relativeDir);
            });

        std::string recompiledFileRelative = getRelativePath(recompiledFile);
        std::string removeAction = stringFormat("rm -f %s", recompiledFileRelative);
        std::vector<std::string> actions{ removeAction };
        CollectionUtils::extend(actions, std::move(commandActions));

        declareTarget(recompiledFileRelative, dependencies, actions);

        artifacts.push_back(recompiledFileRelative);

        if (!hasParent && Paths::isStaticLibraryFile(unitFile)) {
            sharedOutput = LinkerUtils::applySuffix(getSharedLibrary(linkUnitInfo->getOutput()), unitType, suffixForParentOfStubs);
            auto sharedOutputRelative = getRelativePath(
                    sharedOutput.value());
            std::vector<std::string> sharedLinkCommandLine{
                getRelativePathForLinker(primaryCompiler),      "$(LDFLAGS)",
                SHARED_FLAG,          coverageLinkFlags,
                sanitizerLinkFlags,   "-o",
                sharedOutputRelative, "-Wl,--whole-archive",
                recompiledFileRelative,       "-Wl,--allow-multiple-definition",
                STUB_OBJECT_FILES,    "-Wl,--no-whole-archive"
            };
            utbot::LinkCommand sharedLinkCommand{ sharedLinkCommandLine, getRelativePath(buildDirectory) };
            declareTarget(sharedOutputRelative, { recompiledFileRelative, STUB_OBJECT_FILES },
                          { sharedLinkCommand.toStringWithChangingDirectory() });


            artifacts.push_back(sharedOutputRelative);
        }
        return buildResults[unitFile] = { recompiledFile, unitType };
    }


    fs::path NativeMakefilePrinter::getSharedLibrary(const fs::path &filePath) {
        fs::path output = CompilationUtils::removeSharedLibraryVersion(filePath);
        fs::path sharedLibrary = Paths::isSharedLibraryFile(output)
                                     ? output
                                     : Paths::addPrefix(Paths::addExtension(output, ".so"), "lib");
        return sharedLibrary;
    }

    void NativeMakefilePrinter::addStubs(const CollectionUtils::FileSet &stubsSet) {
        auto stubObjectFiles = CollectionUtils::transformTo<CollectionUtils::FileSet>(
            Synchronizer::dropHeaders(stubsSet), [this](fs::path const &stub) {
                fs::path sourcePath = Paths::stubPathToSourcePath(projectContext, stub);
                fs::path stubBuildFilePath =
                    Paths::getStubBuildFilePath(projectContext, sourcePath);
                auto compilationUnitInfo = buildDatabase->getClientCompilationUnitInfo(sourcePath);
                fs::path target = Paths::getRecompiledFile(projectContext, stub);
                addCompileTarget(stub, target, *compilationUnitInfo);
                return target;
            });
        auto stubObjectFilesRelative = CollectionUtils::transformTo<CollectionUtils::FileSet>(
            stubObjectFiles, [this](fs::path const &stub) {
                return getRelativePath(stub);
            });
        declareVariable(STUB_OBJECT_FILES_NAME, StringUtils::joinWith(stubObjectFilesRelative, " "));
    }

    void NativeMakefilePrinter::close() {
        declareTarget("clean", {},
                      { stringFormat("rm -rf %s", StringUtils::joinWith(artifacts, " ")) });
        auto allDependencies = stringFormat("%s/%%.d", getRelativePath(dependencyDirectory));
        auto includeDependencies = stringFormat("%s/*.d", getRelativePath(dependencyDirectory));
        auto includeTemporaryDependencies = stringFormat("%s/*.Td", getRelativePath(dependencyDirectory));
        ss << ".PRECIOUS: " << allDependencies << "\n";
        ss << allDependencies << ": ;\n";
        ss << "\n";
        ss << "-include " << includeTemporaryDependencies << " " << includeDependencies << "\n";
        ss << "\n";
    }

    void NativeMakefilePrinter::addLinkTargetRecursively(const fs::path &unitFile,
                                                         const std::string &suffixForParentOfStubs,
                                                         bool exeToLib) {
        addLinkTargetRecursively(unitFile, suffixForParentOfStubs, false, exeToLib);
    }

    void NativeMakefilePrinter::tryChangeToRelativePath(std::string& argument) const {
        if (StringUtils::startsWith(argument, "/")) {
            argument = getRelativePath(argument);
            return;
        }
        // if in -I flag
        if (argument.length() >= 3 && StringUtils::startsWith(argument, "-I")) {
            argument = "-I" +
                    getRelativePath(argument.substr(2)).string();
        }
    }
}
