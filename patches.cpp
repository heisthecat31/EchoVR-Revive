#include "patches.h"
#include "detours.h" 
#include <windows.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <shlwapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "shlwapi.lib")

float g_HapticStrength = 1.4f;   
float g_FovMultiplier  = 1.0f; 
bool  g_SteamVRMode    = false;  // When true, redirect LibOVR to Revive
std::wstring g_RevivePath;       // Custom Revive installation path

// =============================================================
// OVR CONSTANTS & STRUCTURES
// =============================================================
const unsigned int OVR_BUTTON_A         = 0x00000001;
const unsigned int OVR_BUTTON_B         = 0x00000002;
const unsigned int OVR_BUTTON_RTHUMB    = 0x00000004;
const unsigned int OVR_BUTTON_RSHOULDER = 0x00000008;
const unsigned int OVR_BUTTON_X         = 0x00000100;
const unsigned int OVR_BUTTON_Y         = 0x00000200;
const unsigned int OVR_BUTTON_LTHUMB    = 0x00000400;
const unsigned int OVR_BUTTON_LSHOULDER = 0x00000800;
const unsigned int OVR_BUTTON_ENTER     = 0x00100000; 
const int ovrControllerType_LTouch = 0x0001;
const int ovrControllerType_RTouch = 0x0002;
const int ovrControllerType_Touch  = 0x0003;

typedef int ovrResult;
typedef void* ovrSession;
typedef int ovrControllerType;
typedef int ovrHmdType; 

struct ovrVector2f { float x, y; };
struct ovrSizei { int w, h; };
struct ovrFovPort { float UpTan; float DownTan; float LeftTan; float RightTan; };

struct ovrHapticsBuffer {
    const void* Samples;
    int SamplesCount;
    int SubmitMode;
};

struct ovrHmdDesc {
    ovrHmdType Type;
    char _pad0[4]; 
    char ProductName[64];
    char Manufacturer[64];
    short VendorId;
    short ProductId;
    char SerialNumber[24];
    short FirmwareMajor;
    short FirmwareMinor;
    unsigned int AvailableHmdCaps;
    unsigned int DefaultHmdCaps;
    unsigned int AvailableTrackingCaps;
    unsigned int DefaultTrackingCaps;
    ovrFovPort DefaultEyeFov[2];
    ovrFovPort MaxEyeFov[2];
    ovrSizei Resolution;
    float DisplayRefreshRate;
    char _pad1[4]; 
};

struct ovrInputState {
    double TimeInSeconds;
    unsigned int Buttons;
    unsigned int Touches;
    float IndexTrigger[2];
    float HandTrigger[2];
    ovrVector2f Thumbstick[2];
    int ControllerType;
    float IndexTriggerNoDeadzone[2];
    float HandTriggerNoDeadzone[2];
    ovrVector2f ThumbstickNoDeadzone[2];
    float IndexTriggerRaw[2];
    float HandTriggerRaw[2];
    ovrVector2f ThumbstickRaw[2];
};

typedef ovrResult(__cdecl* pf_SetControllerVibration)(ovrSession, ovrControllerType, float, float);
typedef ovrResult(__cdecl* pf_SubmitControllerVibration)(ovrSession, ovrControllerType, const ovrHapticsBuffer*);
typedef ovrHmdDesc(__cdecl* pf_GetHmdDesc)(ovrHmdDesc*, ovrSession);
typedef ovrResult(__cdecl* pf_GetInputState)(ovrSession, ovrControllerType, ovrInputState*);

pf_SetControllerVibration Real_SetControllerVibration = nullptr;
pf_SubmitControllerVibration Real_SubmitControllerVibration = nullptr;
pf_GetHmdDesc Real_GetHmdDesc = nullptr;
pf_GetInputState Real_GetInputState = nullptr;

// =============================================================
// STEAMVR / REVIVE INTEGRATION
// =============================================================

// Check if a process is running by name
static bool IsProcessRunning(const wchar_t* processName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Launch SteamVR and wait for it to be ready
static void EnsureSteamVRRunning() {
    if (IsProcessRunning(L"vrserver.exe")) return; // Already running

    // Try launching via Steam protocol
    ShellExecuteW(NULL, L"open", L"steam://run/250820", NULL, NULL, SW_SHOWNORMAL);

    // Wait up to 30 seconds for vrserver.exe to appear
    for (int i = 0; i < 300; i++) {
        if (IsProcessRunning(L"vrserver.exe")) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Inject Revive into the process
static void InjectRevive() {
    std::vector<std::wstring> pathsToCheck;
    
    // 1. Check custom path from config
    if (!g_RevivePath.empty()) {
        pathsToCheck.push_back(g_RevivePath + L"\\LibReviveXR64.dll");
        pathsToCheck.push_back(g_RevivePath + L"\\LibRevive64.dll");
    }
    
    // 2. Check standard Revive install paths
    pathsToCheck.push_back(L"C:\\Program Files\\Revive\\LibReviveXR64.dll");
    pathsToCheck.push_back(L"C:\\Program Files\\Revive\\LibRevive64.dll");
    pathsToCheck.push_back(L"C:\\Program Files (x86)\\Revive\\LibReviveXR64.dll");
    
    HMODULE hRevive = nullptr;
    for (const auto& path : pathsToCheck) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            hRevive = LoadLibraryW(path.c_str());
            if (hRevive) {
                // If it's the XR DLL, we also need the main proxy DLL
                if (path.find(L"XR") != std::wstring::npos) {
                    WCHAR proxyPath[MAX_PATH];
                    wcscpy_s(proxyPath, MAX_PATH, path.c_str());
                    WCHAR* slash = wcsrchr(proxyPath, L'\\');
                    if (slash) {
                        wcscpy_s(slash + 1, MAX_PATH - (slash - proxyPath + 1), L"LibRevive64.dll");
                        LoadLibraryW(proxyPath);
                    }
                }
                break;
            }
        }
    }
}

// =============================================================
// CONFIGURATION
// =============================================================
struct ButtonMapping { unsigned int source; unsigned int target; };
struct AnalogMapping { int source; int target; };
struct StickMapping  { int source; int target; };

std::vector<ButtonMapping> g_ButtonMappings;
std::vector<AnalogMapping> g_AnalogMappings; 
std::vector<StickMapping>  g_StickMappings;
int g_StickRemapMode = 0; // 0=Both, 1=TurningOnly, 2=ButtonsOnly

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

unsigned int StringToButton(const std::string& name) {
    if (name == "A") return OVR_BUTTON_A;
    if (name == "B") return OVR_BUTTON_B;
    if (name == "X") return OVR_BUTTON_X;
    if (name == "Y") return OVR_BUTTON_Y;
    if (name == "LStick") return OVR_BUTTON_LTHUMB;
    if (name == "RStick") return OVR_BUTTON_RTHUMB;
    if (name == "Menu") return OVR_BUTTON_ENTER;
    if (name == "LGrip_Btn") return OVR_BUTTON_LSHOULDER;
    if (name == "RGrip_Btn") return OVR_BUTTON_RSHOULDER;
    return 0;
}

int StringToAnalog(const std::string& name) {
    if (name == "LTrigger") return 0;
    if (name == "RTrigger") return 1;
    if (name == "LGrip") return 2;
    if (name == "RGrip") return 3;
    return -1;
}

int StringToStick(const std::string& name) {
    if (name == "LStick") return 0;
    if (name == "RStick") return 1;
    return -1;
}

void LoadConfig() {
    g_ButtonMappings.clear();
    g_AnalogMappings.clear();
    g_StickMappings.clear();
    std::ifstream file("haptics_config.txt");
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;
        std::istringstream is_line(line);
        std::string key;
        if (std::getline(is_line, key, '=')) {
            std::string value;
            if (std::getline(is_line, value)) {
                key = Trim(key);
                value = Trim(value);
                if (key == "HapticStrength") g_HapticStrength = std::stof(value);
                else if (key == "FovMultiplier") g_FovMultiplier = std::stof(value);
                else if (key == "StickRemapMode") g_StickRemapMode = std::stoi(value);
                else if (key == "SteamVRMode") g_SteamVRMode = (value == "1" || value == "true");
                else if (key == "RevivePath") {
                    int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, NULL, 0);
                    if (len > 0) {
                        std::wstring wstr(len, 0);
                        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &wstr[0], len);
                        g_RevivePath = wstr.c_str(); // remove extra null term
                    }
                }
                else if (key.find("Map_") == 0) {
                    std::string btnName = key.substr(4);
                    
                    // Analog (Triggers/Grips)
                    int sourceA = StringToAnalog(btnName); // KEY is physical source
                    int targetA = StringToAnalog(value);   // VALUE is virtual target
                    if (sourceA != -1 && targetA != -1) {
                        g_AnalogMappings.push_back({sourceA, targetA});
                        continue;
                    }

                    // Stick Analog
                    int sourceS = StringToStick(btnName);
                    int targetS = StringToStick(value);
                    if (sourceS != -1 && targetS != -1) {
                        if (g_StickRemapMode == 0 || g_StickRemapMode == 1) {
                            g_StickMappings.push_back({sourceS, targetS});
                        }
                        if (g_StickRemapMode == 1) continue; // Only turning, skip button map
                    }
                    
                    // Buttons (including Stick Click)
                    unsigned int sourceB = StringToButton(btnName);
                    unsigned int targetB = StringToButton(value);
                    if (sourceB != 0 && targetB != 0) {
                        bool isStickBtn = (StringToStick(btnName) != -1);
                        if (isStickBtn) {
                            if (g_StickRemapMode == 0 || g_StickRemapMode == 2) {
                                g_ButtonMappings.push_back({sourceB, targetB});
                            }
                        } else {
                            g_ButtonMappings.push_back({sourceB, targetB});
                        }
                    }
                }
            }
        }
    }
    if (g_HapticStrength > 5.0f) g_HapticStrength = 5.0f;
    if (g_FovMultiplier < 0.1f) g_FovMultiplier = 1.0f; 
}

// =============================================================
// HOOKS (haptics, FOV, input — work on top of either Oculus or Revive)
// =============================================================

ovrResult __cdecl Hooked_SubmitControllerVibration(ovrSession session, ovrControllerType type, const ovrHapticsBuffer* buffer) {
    if (!Real_SetControllerVibration) {
        // Try to find the function in whatever LibOVR is loaded (real or Revive)
        HMODULE h = GetModuleHandleA("LibOVRRT64_1.dll");
        if (!h) h = GetModuleHandleA("LibReviveXR64.dll");
        if (!h) h = GetModuleHandleA("LibRevive64.dll");
        if (!h) h = GetModuleHandleA("LibRevive64_1.dll");
        if (h) Real_SetControllerVibration = (pf_SetControllerVibration)GetProcAddress(h, "ovr_SetControllerVibration");
    }
    if (!Real_SetControllerVibration) return 0;

    bool shouldVibrate = false;
    float finalAmplitude = 0.0f;
    if (buffer && buffer->SamplesCount > 0) {
        const unsigned char* samples = (const unsigned char*)buffer->Samples;
        long total = 0;
        for (int i = 0; i < buffer->SamplesCount; i++) total += samples[i];
        float amp = ((float)total / buffer->SamplesCount) / 255.0f; 
        if (amp > 0.01f) { 
            shouldVibrate = true;
            finalAmplitude = amp * g_HapticStrength;
            if (finalAmplitude > 1.0f) finalAmplitude = 1.0f;
        }
    }
    if (shouldVibrate) Real_SetControllerVibration(session, type, 1.0f, finalAmplitude);
    else Real_SetControllerVibration(session, type, 0.0f, 0.0f);
    return 0; 
}
void __cdecl Hooked_GetHmdDesc(ovrHmdDesc* retbuf, ovrSession session) {
    Real_GetHmdDesc(retbuf, session);
    if (g_FovMultiplier != 1.0f) {
        for (int i = 0; i < 2; ++i) {
            retbuf->DefaultEyeFov[i].UpTan *= g_FovMultiplier;
            retbuf->DefaultEyeFov[i].DownTan *= g_FovMultiplier;
            retbuf->DefaultEyeFov[i].LeftTan *= g_FovMultiplier;
            retbuf->DefaultEyeFov[i].RightTan *= g_FovMultiplier;
        }
    }
}


ovrResult __cdecl Hooked_GetInputState(ovrSession session, ovrControllerType controllerType, ovrInputState* inputState) {
    // If we have mappings, we need data from BOTH controllers (Touch) to perform the swap correctly.
    // LibOVR otherwise only returns data for the specific controller requested.
    ovrControllerType requestedType = controllerType;
    if (!g_ButtonMappings.empty() || !g_AnalogMappings.empty() || !g_StickMappings.empty()) {
        if (controllerType == ovrControllerType_LTouch || controllerType == ovrControllerType_RTouch) {
            controllerType = ovrControllerType_Touch;
        }
    }

    ovrResult result = Real_GetInputState(session, controllerType, inputState);
    if (result >= 0 && inputState) {
        // Restore the original controller type requested by the game
        inputState->ControllerType = requestedType;

        if (!g_ButtonMappings.empty()) {
            unsigned int originalButtons = inputState->Buttons;
            unsigned int newButtons = 0;
            for (const auto& m : g_ButtonMappings) {
                if (originalButtons & m.source) newButtons |= m.target;
            }
            unsigned int sources = 0;
            for (const auto& m : g_ButtonMappings) sources |= m.source;
            inputState->Buttons = (originalButtons & ~sources) | newButtons;
        }

        if (!g_AnalogMappings.empty()) {
            float origTrig[2] = {inputState->IndexTrigger[0], inputState->IndexTrigger[1]};
            float origGrip[2] = {inputState->HandTrigger[0], inputState->HandTrigger[1]};
            float newTrig[2] = {0,0}, newGrip[2] = {0,0};
            auto GetOrig = [&](int id) {
                if (id == 0) return origTrig[0];
                if (id == 1) return origTrig[1];
                if (id == 2) return origGrip[0];
                if (id == 3) return origGrip[1];
                return 0.0f;
            };

            for (const auto& m : g_AnalogMappings) {
                float val = GetOrig(m.source);
                if (m.target == 0) newTrig[0] = (val > newTrig[0] ? val : newTrig[0]);
                else if (m.target == 1) newTrig[1] = (val > newTrig[1] ? val : newTrig[1]);
                else if (m.target == 2) newGrip[0] = (val > newGrip[0] ? val : newGrip[0]);
                else if (m.target == 3) newGrip[1] = (val > newGrip[1] ? val : newGrip[1]);
            }
            
            for (const auto& m : g_AnalogMappings) {
                if (m.target == 0) inputState->IndexTrigger[0] = newTrig[0];
                else if (m.target == 1) inputState->IndexTrigger[1] = newTrig[1];
                else if (m.target == 2) inputState->HandTrigger[0] = newGrip[0];
                else if (m.target == 3) inputState->HandTrigger[1] = newGrip[1];
            }
        }

        if (!g_StickMappings.empty()) {
            ovrVector2f origStick[2] = { inputState->Thumbstick[0], inputState->Thumbstick[1] };
            ovrVector2f origStickND[2] = { inputState->ThumbstickNoDeadzone[0], inputState->ThumbstickNoDeadzone[1] };
            ovrVector2f origStickRaw[2] = { inputState->ThumbstickRaw[0], inputState->ThumbstickRaw[1] };
            ovrVector2f newStick[2] = {{0,0},{0,0}}, newStickND[2] = {{0,0},{0,0}}, newStickRaw[2] = {{0,0},{0,0}};

            auto GetMagSq = [](const ovrVector2f& v) { return v.x*v.x + v.y*v.y; };

            for (const auto& m : g_StickMappings) {
                if (GetMagSq(origStick[m.source]) > GetMagSq(newStick[m.target])) {
                    newStick[m.target] = origStick[m.source];
                    newStickND[m.target] = origStickND[m.source];
                    newStickRaw[m.target] = origStickRaw[m.source];
                }
            }

            for (const auto& m : g_StickMappings) {
                inputState->Thumbstick[m.target] = newStick[m.target];
                inputState->ThumbstickNoDeadzone[m.target] = newStickND[m.target];
                inputState->ThumbstickRaw[m.target] = newStickRaw[m.target];
            }
        }
    }
    return result;
}

void InstallHooks() {
    HMODULE hLibOVR = nullptr;
    int attempts = 0;
    while (!hLibOVR && attempts < 500) {
        hLibOVR = GetModuleHandleA("LibOVRRT64_1.dll");
        if (!hLibOVR) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        attempts++;
    }
    if (!hLibOVR) return;

    Real_SubmitControllerVibration = (pf_SubmitControllerVibration)GetProcAddress(hLibOVR, "ovr_SubmitControllerVibration");
    Real_GetInputState = (pf_GetInputState)GetProcAddress(hLibOVR, "ovr_GetInputState");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (Real_SubmitControllerVibration) DetourAttach(&(PVOID&)Real_SubmitControllerVibration, Hooked_SubmitControllerVibration);
    if (Real_GetInputState) DetourAttach(&(PVOID&)Real_GetInputState, Hooked_GetInputState);
    DetourTransactionCommit();

    HMODULE hGame = GetModuleHandleA(nullptr);
    BYTE* base = (BYTE*)hGame;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imp->Name; imp++) {
        const char* dllName = (const char*)(base + imp->Name);
        if (_stricmp(dllName, "LibOVRRT64_1.dll") != 0) continue;

        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iatThunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, iatThunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, "ovr_GetHmdDesc") != 0) continue;

            Real_GetHmdDesc = (pf_GetHmdDesc)iatThunk->u1.Function;

            DWORD oldProtect;
            VirtualProtect(&iatThunk->u1.Function, sizeof(PVOID), PAGE_READWRITE, &oldProtect);
            iatThunk->u1.Function = (ULONG_PTR)Hooked_GetHmdDesc;
            VirtualProtect(&iatThunk->u1.Function, sizeof(PVOID), oldProtect, &oldProtect);
            break;
        }
        break;
    }
}

void InitializeEarly(HMODULE hModule) {
    // Load config first to check SteamVR mode
    LoadConfig();
    
    if (g_SteamVRMode) {
        EnsureSteamVRRunning();
        // Give SteamVR a moment to fully initialize its OpenXR runtime
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        InjectRevive();
    }
}

void Initialize() {
    // The haptics/FOV/input hooks are installed asynchronously
    // (they wait for LibOVR or Revive DLL to be loaded)
    std::thread(InstallHooks).detach();
}
