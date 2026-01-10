/**
 * main.c - Entry point for the Slate application
 * Handles application initialization and message loop
 */

#include "slate.h"

/**
 * Main entry point for the application
 */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine); // We'll use GetCommandLineW instead

    // Parse command line arguments
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const TCHAR* pInitialFile = NULL;

    if (argv != NULL && argc >= 2) {
        pInitialFile = argv[1]; // First argument after exe name
    }

    // Initialize the application
    if (!InitializeApplication(hInstance)) {
        if (argv) LocalFree(argv);
        return -1;
    }

    // If a file was passed, open it
    if (pInitialFile != NULL) {
        LoadFile(&g_app, pInitialFile);
    }

    if (argv) LocalFree(argv);

    // Message loop
    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}