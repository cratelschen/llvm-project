//===-- driver.cpp - Clang GCC-Compatible Driver --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the entry point to the clang driver; it is a thin wrapper
// for functionality in the Driver clang library.
//
//===----------------------------------------------------------------------===//

#include "clang/Driver/Driver.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/HeaderInclude.h"
#include "clang/Basic/Stack.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/ChainedDiagnosticConsumer.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/SerializedDiagnosticPrinter.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Config/llvm-config.h" // for LLVM_ON_UNIX
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/BuryPointer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include <memory>
#include <optional>
#include <set>
#include <system_error>

using namespace clang;
using namespace clang::driver;
using namespace llvm::opt;

std::string GetExecutablePath(const char *Argv0, bool CanonicalPrefixes) {
  if (!CanonicalPrefixes) {
    SmallString<128> ExecutablePath(Argv0);
    // Do a PATH lookup if Argv0 isn't a valid path.
    if (!llvm::sys::fs::exists(ExecutablePath))
      if (llvm::ErrorOr<std::string> P =
              llvm::sys::findProgramByName(ExecutablePath))
        ExecutablePath = *P;
    return std::string(ExecutablePath);
  }

  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void *P = (void *)(intptr_t)GetExecutablePath;
  return llvm::sys::fs::getMainExecutable(Argv0, P);
}

static const char *GetStableCStr(llvm::StringSet<> &SavedStrings, StringRef S) {
  return SavedStrings.insert(S).first->getKeyData();
}

extern int cc1_main(ArrayRef<const char *> Argv, const char *Argv0,
                    void *MainAddr);
extern int cc1as_main(ArrayRef<const char *> Argv, const char *Argv0,
                      void *MainAddr);
extern int cc1gen_reproducer_main(ArrayRef<const char *> Argv,
                                  const char *Argv0, void *MainAddr,
                                  const llvm::ToolContext &);

static void insertTargetAndModeArgs(const ParsedClangName &NameParts,
                                    SmallVectorImpl<const char *> &ArgVector,
                                    llvm::StringSet<> &SavedStrings) {
  // Put target and mode arguments at the start of argument list so that
  // arguments specified in command line could override them. Avoid putting
  // them at index 0, as an option like '-cc1' must remain the first.
  int InsertionPoint = 0;
  if (ArgVector.size() > 0)
    ++InsertionPoint;

  if (NameParts.DriverMode) {
    // Add the mode flag to the arguments.
    ArgVector.insert(ArgVector.begin() + InsertionPoint,
                     GetStableCStr(SavedStrings, NameParts.DriverMode));
  }

  if (NameParts.TargetIsValid) {
    const char *arr[] = {"-target",
                         GetStableCStr(SavedStrings, NameParts.TargetPrefix)};
    ArgVector.insert(ArgVector.begin() + InsertionPoint, std::begin(arr),
                     std::end(arr));
  }
}

static void getCLEnvVarOptions(std::string &EnvValue, llvm::StringSaver &Saver,
                               SmallVectorImpl<const char *> &Opts) {
  llvm::cl::TokenizeWindowsCommandLine(EnvValue, Saver, Opts);
  // The first instance of '#' should be replaced with '=' in each option.
  for (const char *Opt : Opts)
    if (char *NumberSignPtr = const_cast<char *>(::strchr(Opt, '#')))
      *NumberSignPtr = '=';
}

template <class T>
static T checkEnvVar(const char *EnvOptSet, const char *EnvOptFile,
                     std::string &OptFile) {
  const char *Str = ::getenv(EnvOptSet);
  if (!Str)
    return T{};

  T OptVal = Str;
  if (const char *Var = ::getenv(EnvOptFile))
    OptFile = Var;
  return OptVal;
}

static bool SetBackdoorDriverOutputsFromEnvVars(Driver &TheDriver) {
  TheDriver.CCPrintOptions =
      checkEnvVar<bool>("CC_PRINT_OPTIONS", "CC_PRINT_OPTIONS_FILE",
                        TheDriver.CCPrintOptionsFilename);
  if (checkEnvVar<bool>("CC_PRINT_HEADERS", "CC_PRINT_HEADERS_FILE",
                        TheDriver.CCPrintHeadersFilename)) {
    TheDriver.CCPrintHeadersFormat = HIFMT_Textual;
    TheDriver.CCPrintHeadersFiltering = HIFIL_None;
  } else {
    std::string EnvVar = checkEnvVar<std::string>(
        "CC_PRINT_HEADERS_FORMAT", "CC_PRINT_HEADERS_FILE",
        TheDriver.CCPrintHeadersFilename);
    if (!EnvVar.empty()) {
      TheDriver.CCPrintHeadersFormat =
          stringToHeaderIncludeFormatKind(EnvVar.c_str());
      if (!TheDriver.CCPrintHeadersFormat) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var)
            << 0 << EnvVar;
        return false;
      }

      const char *FilteringStr = ::getenv("CC_PRINT_HEADERS_FILTERING");
      HeaderIncludeFilteringKind Filtering;
      if (!stringToHeaderIncludeFiltering(FilteringStr, Filtering)) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var)
            << 1 << FilteringStr;
        return false;
      }

      if ((TheDriver.CCPrintHeadersFormat == HIFMT_Textual &&
           Filtering != HIFIL_None) ||
          (TheDriver.CCPrintHeadersFormat == HIFMT_JSON &&
           Filtering != HIFIL_Only_Direct_System)) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var_combination)
            << EnvVar << FilteringStr;
        return false;
      }
      TheDriver.CCPrintHeadersFiltering = Filtering;
    }
  }

  TheDriver.CCLogDiagnostics =
      checkEnvVar<bool>("CC_LOG_DIAGNOSTICS", "CC_LOG_DIAGNOSTICS_FILE",
                        TheDriver.CCLogDiagnosticsFilename);
  TheDriver.CCPrintProcessStats =
      checkEnvVar<bool>("CC_PRINT_PROC_STAT", "CC_PRINT_PROC_STAT_FILE",
                        TheDriver.CCPrintStatReportFilename);
  TheDriver.CCPrintInternalStats =
      checkEnvVar<bool>("CC_PRINT_INTERNAL_STAT", "CC_PRINT_INTERNAL_STAT_FILE",
                        TheDriver.CCPrintInternalStatReportFilename);

  return true;
}

static void FixupDiagPrefixExeName(TextDiagnosticPrinter *DiagClient,
                                   const std::string &Path) {
  // If the clang binary happens to be named cl.exe for compatibility reasons,
  // use clang-cl.exe as the prefix to avoid confusion between clang and MSVC.
  StringRef ExeBasename(llvm::sys::path::stem(Path));
  if (ExeBasename.equals_insensitive("cl"))
    ExeBasename = "clang-cl";
  DiagClient->setPrefix(std::string(ExeBasename));
}

// Cratels:执行-cc1的具体操作
static int ExecuteCC1Tool(SmallVectorImpl<const char *> &ArgV,
                          const llvm::ToolContext &ToolContext) {
  // If we call the cc1 tool from the clangDriver library (through
  // Driver::CC1Main), we need to clean up the options usage count. The options
  // are currently global, and they might have been used previously by the
  // driver.
  llvm::cl::ResetAllOptionOccurrences();

  // Cratels:将ExpansionContext类型的对象ECtx分配在堆上,其内存管理交给BumpPtrAllocator来管理
  llvm::BumpPtrAllocator A;
  llvm::cl::ExpansionContext ECtx(A, llvm::cl::TokenizeGNUCommandLine);

  // Cratels:再次展开 response file并将解析出来的option放进 ArgV 中
  // Cratels:第一次展开出来了-cc1,然后后续依然有response
  // file的可能,因此需要二次展开,
  if (llvm::Error Err = ECtx.expandResponseFiles(ArgV)) {
    llvm::errs() << toString(std::move(Err)) << '\n';
    return 1;
  }

  // Cratels:获取正在调用的工具名,比如clang
  StringRef Tool = ArgV[1];

  // Cratels:GetExecutablePath是一个方法,用了获得可执行文件的真实路径,我们不在这里直接将其调用,而是以参数的形式传入具体的方法中去调用
  // Cratels: 详细请看本目录 函数指针转换.md
  // Cratels:函数指针转换为通用指针,便于传参，用于找到真正执行的二进制文件路径
  void *GetExecutablePathVP = (void *)(intptr_t)GetExecutablePath;

  // clang-format off
  // Cratels:使用该 option 时 clang driver退化为 clang 前端，而不是作为完整的编译器运行。只进行前端的一些 action，包括打印 AST 等 action
  // clang-format on
  if (Tool == "-cc1")
    return cc1_main(ArrayRef(ArgV).slice(1), ArgV[0], GetExecutablePathVP);

  // Cratels:处理汇编文件
  if (Tool == "-cc1as")
    return cc1as_main(ArrayRef(ArgV).slice(2), ArgV[0], GetExecutablePathVP);

  // Cratels:处理代码复现
  if (Tool == "-cc1gen-reproducer")
    return cc1gen_reproducer_main(ArrayRef(ArgV).slice(2), ArgV[0],
                                  GetExecutablePathVP, ToolContext);
  // Reject unknown tools.
  llvm::errs()
      << "error: unknown integrated tool '" << Tool << "'. "
      << "Valid tools include '-cc1', '-cc1as' and '-cc1gen-reproducer'.\n";
  return 1;
}
// Cratels:
// Cratels:clang driver的真正入口.main方法在build目录,是有cmake自动生成的
// Cratels:
// Cratels:clang本身也是一个tool,因此这里参数类名为ToolContext
int clang_main(int Argc, char **Argv, const llvm::ToolContext &ToolContext) {
  noteBottomOfStack();

  // Cratels:设置,并未调用
  llvm::setBugReportMsg("PLEASE submit a bug report to " BUG_REPORT_URL
                        " and include the crash backtrace, preprocessed "
                        "source, and associated run script.\n");

  // Cratels:保存所有命令行参数
  SmallVector<const char *, 256> Args(Argv, Argv + Argc);

  // clang-format off
// Cratels:FixupStandardFileDescriptors确保所有的标准文件描述符(标准输入,标准输出以及标准错误)在使用前被正确配置了.
// Cratels:只有当target为可执行文件的程序可以调用该方法,target为库的程序不应该调用它.因为库的该属性应该有调用方程序来决定.
  // clang-format on
  if (llvm::sys::Process::FixupStandardFileDescriptors())
    return 1;

  // clang-format off
  // Cratels:初始化所有的target.如果在cmake configure的时候指定了target,这只会初始化对应的target,否则就初始化默认的targets;
  // Cratels:cmake获得命令行中的指定宏的值,然后根据这些值生成def文件,def文件又被其他源码引入使用(include),从而使得command option影响源码的作用
  // clang-format on
  llvm::InitializeAllTargets();

  // clang-format off
  // Cratels:BumpPtrAllocator 是一个内存分配器，它在垃圾收集器（garbage collector）的堆（heap）上工作。这种类型的分配器通常用于实现自动内存管理，其中垃圾收集器负责释放不再使用的内存。
  // Cratels: BumpPtrAllocator 的工作原理是使用一个“bump pointer”来跟踪当前的内存分配位置。
  // Cratels:每当分配新内存时，它会从当前位置开始分配，并自动更新 bump pointer 以指向下一个可用的内存位置。这种方式简化了内存分配，因为不需要在每次分配时搜索堆以找到可用的内存块。
  // clang-format on
  llvm::BumpPtrAllocator A;
  // Cratels:将字符串传入上面分配的内存中,实现字符串的内存自动化管理
  llvm::StringSaver Saver(A);

  // Cratels:获取命令行执行的可执行文件名
  const char *ProgName =
      ToolContext.NeedsPrependArg ? ToolContext.PrependArg : ToolContext.Path;

  // clang-format off
  // Cratels:clang-cl 是一个Clang驱动器的另一个命令行接口的选择，被设计用来兼容Visual C++ 的编译器cl.exe
  // Cratels:clang --driver-mode可以用来指定 clang driver 的工作模式
  // Cratels:There are two ways to put clang in CL compatibility mode: ProgName is either clang-cl or cl, or --driver-mode=cl is on the command line
  // clang-format on
  bool ClangCLMode =
      IsClangCL(getDriverMode(ProgName, llvm::ArrayRef(Args).slice(1)));

  // clang-format off
  // Cratels:On some systems (such as older UNIX systems and certain Windows variants) command-lines have relatively limited lengths.
  // Cratels: Windows compilers therefore support "response files". These files are mentioned on the command-line (using the "@file") syntax.
  // Cratels: The compiler reads these files and inserts the contents into argv, thereby working around the command-line length limits.
  // Cratels:一些编译环境（比如 windows）对命令行的长度有限制，不能超过 127字符，但是现实环境中确实存在很长命令行的需求，此时可以使用 response file 来绕过这个限制
  // Cratels:详细信息请看：https://learn.microsoft.com/en-us/cpp/build/reference/at-specify-a-compiler-response-file?view=msvc-170
  // Cratels:见本目录的 response_file.md文档
  // Cratels:这里的操作就是展开 response file 并将其内容写入 Args 里面
  // clang-format on
  if (llvm::Error Err = expandResponseFiles(Args, ClangCLMode, A)) {
    llvm::errs() << toString(std::move(Err)) << '\n';
    return 1;
  }

  // Handle -cc1 integrated tools.
  // Cratels:指定-cc1系列 option时的处理逻辑
  if (Args.size() >= 2 && StringRef(Args[1]).starts_with("-cc1"))
    return ExecuteCC1Tool(Args, ToolContext);

  // clang-format off
  // Cratels:完整编译流程
  // clang-format on

  // clang-format off
  // Cratels:CanonicalPrefixes控制 clang-cl 场景下文件的相对路径前缀，是为了适配 windows 以及 gcc 而设置的 option，暂时不深入研究
  // clang-format on
  // Handle options that need handling before the real command line parsing in
  // Driver::BuildCompilation()
  bool CanonicalPrefixes = true;
  for (int i = 1, size = Args.size(); i < size; ++i) {
    // Skip end-of-line response file markers
    if (Args[i] == nullptr)
      continue;
    if (StringRef(Args[i]) == "-canonical-prefixes")
      CanonicalPrefixes = true;
    else if (StringRef(Args[i]) == "-no-canonical-prefixes")
      CanonicalPrefixes = false;
  }

  // Handle CL and _CL_ which permits additional command line options to be
  // prepended or appended.
  if (ClangCLMode) {
    // Arguments in "CL" are prepended.
    std::optional<std::string> OptCL = llvm::sys::Process::GetEnv("CL");
    if (OptCL) {
      SmallVector<const char *, 8> PrependedOpts;
      getCLEnvVarOptions(*OptCL, Saver, PrependedOpts);

      // Insert right after the program name to prepend to the argument list.
      Args.insert(Args.begin() + 1, PrependedOpts.begin(), PrependedOpts.end());
    }
    // Arguments in "_CL_" are appended.
    std::optional<std::string> Opt_CL_ = llvm::sys::Process::GetEnv("_CL_");
    if (Opt_CL_) {
      SmallVector<const char *, 8> AppendedOpts;
      getCLEnvVarOptions(*Opt_CL_, Saver, AppendedOpts);

      // Insert at the end of the argument list to append.
      Args.append(AppendedOpts.begin(), AppendedOpts.end());
    }
  }

  llvm::StringSet<> SavedStrings;
  // Handle CCC_OVERRIDE_OPTIONS, used for editing a command line behind the
  // scenes.
  if (const char *OverrideStr = ::getenv("CCC_OVERRIDE_OPTIONS")) {
    // FIXME: Driver shouldn't take extra initial argument.
    driver::applyOverrideOptions(Args, OverrideStr, SavedStrings,
                                 &llvm::errs());
  }

  // clang-format off
  // Cratels:获取真正的可执行文件路径:clang链接到clang-20上面
  // clang-format on
  std::string Path = GetExecutablePath(ToolContext.Path, CanonicalPrefixes);

  // Whether the cc1 tool should be called inside the current process, or if we
  // should spawn a new clang subprocess (old behavior).
  // Not having an additional process saves some execution time of Windows,
  // and makes debugging and profiling easier.
  bool UseNewCC1Process = CLANG_SPAWN_CC1;
  for (const char *Arg : Args)
    UseNewCC1Process = llvm::StringSwitch<bool>(Arg)
                           .Case("-fno-integrated-cc1", true)
                           .Case("-fintegrated-cc1", false)
                           .Default(UseNewCC1Process);

  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts =
      CreateAndPopulateDiagOpts(Args);

  // clang-format off
  // Cratels:诊断信息打印器
  // clang-format on
  TextDiagnosticPrinter *DiagClient =
      new TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
  FixupDiagPrefixExeName(DiagClient, ProgName);

  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());

  DiagnosticsEngine Diags(DiagID, &*DiagOpts, DiagClient);

  if (!DiagOpts->DiagnosticSerializationFile.empty()) {
    auto SerializedConsumer =
        clang::serialized_diags::create(DiagOpts->DiagnosticSerializationFile,
                                        &*DiagOpts, /*MergeChildRecords=*/true);
    Diags.setClient(new ChainedDiagnosticConsumer(
        Diags.takeClient(), std::move(SerializedConsumer)));
  }

  // Cratels:处理warning信息
  ProcessWarningOptions(Diags, *DiagOpts, /*ReportDiags=*/false);

  // clang-format off
  // Cratels:新建 Driver 实例，Driver 就是 clang 驱动器所对应的类，包括一次完整编译的所有信息
  // Cratels:这里给出了执行文件路径 path，目标系统的三元组信息以及诊断信息
  // clang-format on
  Driver TheDriver(Path, llvm::sys::getDefaultTargetTriple(), Diags);

  // Cratels:为何会从名字去出发找target与model信息?
  auto TargetAndMode = ToolChain::getTargetAndModeFromProgramName(ProgName);
  TheDriver.setTargetAndMode(TargetAndMode);
  // If -canonical-prefixes is set, GetExecutablePath will have resolved Path
  // to the llvm driver binary, not clang. In this case, we need to use
  // PrependArg which should be clang-*. Checking just CanonicalPrefixes is
  // safe even in the normal case because PrependArg will be null so
  // setPrependArg will be a no-op.
  if (ToolContext.NeedsPrependArg || CanonicalPrefixes)
    TheDriver.setPrependArg(ToolContext.PrependArg);

  insertTargetAndModeArgs(TargetAndMode, Args, SavedStrings);

  if (!SetBackdoorDriverOutputsFromEnvVars(TheDriver))
    return 1;

  if (!UseNewCC1Process) {
    TheDriver.CC1Main = [ToolContext](SmallVectorImpl<const char *> &ArgV) {
      return ExecuteCC1Tool(ArgV, ToolContext);
    };
    // Ensure the CC1Command actually catches cc1 crashes
    llvm::CrashRecoveryContext::Enable();
  }

  // Cratels:在BuildCompilation中进行了命令行的扩充,从原始的输入命令扩展到完整地调用命令行
  std::unique_ptr<Compilation> C(TheDriver.BuildCompilation(Args));

  // Cratels:Job代指的是一个编译过程,比如现在编译器前端使用clang进行编译得到obj文件,然后使用ld进行链接得到可执行文件.这就是两个Job.
  for (auto job : C->getJobs()) {
    llvm::outs() << job.getExecutable() << "\n";
    llvm::outs() << job.getArguments().size() << "\n";
  }

  Driver::ReproLevel ReproLevel = Driver::ReproLevel::OnCrash;
  if (Arg *A = C->getArgs().getLastArg(options::OPT_gen_reproducer_eq)) {
    auto Level =
        llvm::StringSwitch<std::optional<Driver::ReproLevel>>(A->getValue())
            .Case("off", Driver::ReproLevel::Off)
            .Case("crash", Driver::ReproLevel::OnCrash)
            .Case("error", Driver::ReproLevel::OnError)
            .Case("always", Driver::ReproLevel::Always)
            .Default(std::nullopt);
    if (!Level) {
      llvm::errs() << "Unknown value for " << A->getSpelling() << ": '"
                   << A->getValue() << "'\n";
      return 1;
    }
    ReproLevel = *Level;
  }
  if (!!::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"))
    ReproLevel = Driver::ReproLevel::Always;

  int Res = 1;
  bool IsCrash = false;
  Driver::CommandStatus CommandStatus = Driver::CommandStatus::Ok;
  // Pretend the first command failed if ReproStatus is Always.
  const Command *FailingCommand = nullptr;
  if (!C->getJobs().empty())
    FailingCommand = &*C->getJobs().begin();
  if (C && !C->containsError()) {
    SmallVector<std::pair<int, const Command *>, 4> FailingCommands;
    Res = TheDriver.ExecuteCompilation(*C, FailingCommands);

    for (const auto &P : FailingCommands) {
      int CommandRes = P.first;
      FailingCommand = P.second;
      if (!Res)
        Res = CommandRes;

      // If result status is < 0, then the driver command signalled an error.
      // If result status is 70, then the driver command reported a fatal error.
      // On Windows, abort will return an exit code of 3.  In these cases,
      // generate additional diagnostic information if possible.
      IsCrash = CommandRes < 0 || CommandRes == 70;
#ifdef _WIN32
      IsCrash |= CommandRes == 3;
#endif
#if LLVM_ON_UNIX
      // When running in integrated-cc1 mode, the CrashRecoveryContext returns
      // the same codes as if the program crashed. See section "Exit Status for
      // Commands":
      // https://pubs.opengroup.org/onlinepubs/9699919799/xrat/V4_xcu_chap02.html
      IsCrash |= CommandRes > 128;
#endif
      CommandStatus =
          IsCrash ? Driver::CommandStatus::Crash : Driver::CommandStatus::Error;
      if (IsCrash)
        break;
    }
  }

  // Print the bug report message that would be printed if we did actually
  // crash, but only if we're crashing due to FORCE_CLANG_DIAGNOSTICS_CRASH.
  if (::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"))
    llvm::dbgs() << llvm::getBugReportMsg();
  if (FailingCommand != nullptr &&
      TheDriver.maybeGenerateCompilationDiagnostics(CommandStatus, ReproLevel,
                                                    *C, *FailingCommand))
    Res = 1;

  Diags.getClient()->finish();

  if (!UseNewCC1Process && IsCrash) {
    // When crashing in -fintegrated-cc1 mode, bury the timer pointers, because
    // the internal linked list might point to already released stack frames.
    llvm::BuryPointer(llvm::TimerGroup::aquireDefaultGroup());
  } else {
    // If any timers were active but haven't been destroyed yet, print their
    // results now.  This happens in -disable-free mode.
    llvm::TimerGroup::printAll(llvm::errs());
    llvm::TimerGroup::clearAll();
  }

#ifdef _WIN32
  // Exit status should not be negative on Win32, unless abnormal termination.
  // Once abnormal termination was caught, negative status should not be
  // propagated.
  if (Res < 0)
    Res = 1;
#endif

  // If we have multiple failing commands, we return the result of the first
  // failing command.
  return Res;
}
