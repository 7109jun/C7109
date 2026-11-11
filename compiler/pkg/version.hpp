// Single source of truth for the compiler identity (B2). `--version`, package/manifest checks, .cr7 lock files and
// .mp7 maps must all use these constants; never hard-code the strings elsewhere.
#pragma once

namespace c7pkg {
inline constexpr const char* kCompilerName = "C7109";
inline constexpr const char* kCompilerVersion = "1.0.0-alpha.1";
inline constexpr const char* kAbiId = "c7abi1";  // ABI v1 (LLVM fastcc), decided in stage 4
inline constexpr const char* kLanguageVersion = "1.0.0-draft";
}  // namespace c7pkg
