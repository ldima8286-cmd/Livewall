#pragma once

#include "common.h"

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

// %LOCALAPPDATA%\LiveWall, created on first use. Empty string on failure.
const std::wstring& GetAppDataDir();
std::wstring GetLocalAppDataDir();
std::wstring GetModuleDir();
std::wstring GetModulePath();

std::wstring FileNameOf(const std::wstring& path);
std::wstring ExtensionOf(const std::wstring& path);  // lower case, with the dot
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);

bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);

std::string Format(const char* fmt, ...);
std::string HrToString(HRESULT hr);
