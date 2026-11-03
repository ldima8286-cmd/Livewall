#pragma once

#include "common.h"

// Initializes the log file in %LOCALAPPDATA%\LiveWall\livewall.log.
// Safe to call multiple times; failures only disable logging.
void LogInit();
void LogShutdown();

// printf-style logging, never throws. Output goes to the log file and to the
// debugger (OutputDebugString).
void LogPrintf(const char* fmt, ...);

// Logs an HRESULT with a human readable description.
void LogHr(HRESULT hr, const char* what);

const std::wstring& LogFilePath();
