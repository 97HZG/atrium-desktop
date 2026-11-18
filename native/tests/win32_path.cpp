#include "win32_path.h"

#include <iostream>
#include <string>

using namespace atrium;

namespace {

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

bool CheckFoldCase()
{
    return Require(FoldCase(L"").empty(), "empty string stays empty") &&
           Require(FoldCase(L"C:\\Program Files\\Atrium\\Atrium.EXE") == L"c:\\program files\\atrium\\atrium.exe", "ASCII path folds to lowercase") &&
           Require(FoldCase(L"ÄÖÜ") == L"äöü", "non-ASCII Latin letters fold to lowercase") &&
           Require(FoldCase(L"АБВГД") == L"абвгд", "Cyrillic letters fold to lowercase");
}

bool CheckPaths()
{
    const std::wstring self = ModulePath();
    return Require(!self.empty(), "ModulePath returns a non-empty path") &&
           Require(FoldCase(self).ends_with(L".exe"), "ModulePath of test executable ends in .exe") &&
           Require(FoldCase(ProcessImagePath(GetCurrentProcessId())) == FoldCase(self), "ProcessImagePath(pid) matches ModulePath()") &&
           Require(FoldCase(ProcessImagePath(GetCurrentProcess())) == FoldCase(self), "ProcessImagePath(HANDLE) matches ModulePath()") &&
           Require(ProcessImagePath(HANDLE{}).empty(), "ProcessImagePath(null) returns empty");
}

} // namespace

int main()
{
    if (!CheckFoldCase() || !CheckPaths())
        return 1;
    std::cout << "win32 path: all checks passed\n";
    return 0;
}
