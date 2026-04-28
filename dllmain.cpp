#include "patches.h"
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // InitializeEarly must run synchronously in DllMain to hook LoadLibrary
        // BEFORE the game has a chance to load LibOVRRT64_1.dll
        InitializeEarly(hModule);
        // Initialize starts the async thread for haptics/FOV/input hooks
        Initialize();
    }
    return TRUE;
}

// Dummy export to fool the game into loading this as dbgcore.dll
extern "C" __declspec(dllexport) void DetoursExportPlaceholder() { }