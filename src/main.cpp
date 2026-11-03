#include "app.h"

#include <shellapi.h>

#ifndef LIVEWALL_CONSOLE

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    App app;
    const int result = app.Run(instance, argc, argv);

    if (argv) LocalFree(argv);
    return result;
}

#else

int wmain(int argc, wchar_t** argv) {
    App app;
    return app.Run(GetModuleHandleW(nullptr), argc, argv);
}

#endif
