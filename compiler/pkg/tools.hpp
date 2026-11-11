#pragma once
namespace c7pkg {
// `c7c tool <sub> ...`: developer commands that expose the package-layer primitives for testing against reference tools.
int runTool(int argc, char** argv);
// `c7c <pack|verify|install|lock|build-project ...>`: user commands of the build system (stage 5).
bool isProjectCommand(int argc, char** argv);
int runProjectCommand(int argc, char** argv);
}  // namespace c7pkg
