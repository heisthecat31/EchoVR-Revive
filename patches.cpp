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
#include <cstdio>

static void DebugLog(const char* fmt, ...) {
    FILE* f = fopen("echovr_mod_debug.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
    va_end(args);
    fclose(f);
}

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "shlwapi.lib")

float g_HapticStrength = 1.4f;   
float g_FovMultiplierX = 1.0f;
float g_FovMultiplierY = 1.0f;
bool  g_SteamVRMode    = false;  // When true, redirect LibOVR to Revive

// Stick Configuration
float g_DeadzoneLX = 0.0f;
float g_DeadzoneLY = 0.0f;
float g_DeadzoneRX = 0.0f;
float g_DeadzoneRY = 0.0f;
float g_SensLX     = 1.0f;
float g_SensLY     = 1.0f;
float g_SensRX     = 1.0f;
float g_SensRY     = 1.0f;
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
typedef int ovrEyeType;

struct ovrVector2f { float x, y; };
struct ovrVector3f { float x, y, z; };
struct ovrQuatf { float x, y, z, w; };
struct ovrPosef { ovrQuatf Orientation; ovrVector3f Position; };
struct ovrSizei { int w, h; };
struct ovrRecti { ovrVector2f Pos; ovrSizei Size; };
struct ovrFovPort { float UpTan; float DownTan; float LeftTan; float RightTan; };

struct ovrEyeRenderDesc {
    ovrEyeType Eye;
    ovrFovPort Fov;
    ovrRecti DistortedViewport;
    ovrVector2f PixelsPerTanAngleAtCenter;
    ovrPosef HmdToEyePose;
};

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
typedef ovrHmdDesc*(__cdecl* pf_GetHmdDesc)(ovrHmdDesc*, ovrSession);
typedef ovrResult(__cdecl* pf_GetInputState)(ovrSession, ovrControllerType, ovrInputState*);
typedef ovrEyeRenderDesc*(__cdecl* pf_GetRenderDesc)(ovrEyeRenderDesc*, ovrSession, ovrEyeType, ovrFovPort);

pf_SetControllerVibration Real_SetControllerVibration = nullptr;
pf_SubmitControllerVibration Real_SubmitControllerVibration = nullptr;
pf_GetHmdDesc Real_GetHmdDesc = nullptr;
pf_GetInputState Real_GetInputState = nullptr;
pf_GetRenderDesc Real_GetRenderDesc = nullptr;

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

// Check if SteamVR is running. If not, show an error and terminate the game.
static bool EnsureSteamVRRunning() {
    if (IsProcessRunning(L"vrserver.exe")) {
        return true; 
    }
    
    MessageBoxA(NULL, "Please Launch SteamVR before opening echo", "EchoVR-Revive", MB_OK | MB_ICONERROR);
    ExitProcess(1);
    return false;
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
                else if (key == "FovMultiplier") { g_FovMultiplierX = std::stof(value); g_FovMultiplierY = std::stof(value); }
                else if (key == "FovMultiplierX") g_FovMultiplierX = std::stof(value);
                else if (key == "FovMultiplierY") g_FovMultiplierY = std::stof(value);
                else if (key == "StickRemapMode") g_StickRemapMode = std::stoi(value);
                else if (key == "SteamVRMode") g_SteamVRMode = (value == "1" || value == "true");
                else if (key == "DeadzoneLX") g_DeadzoneLX = std::stof(value);
                else if (key == "DeadzoneLY") g_DeadzoneLY = std::stof(value);
                else if (key == "DeadzoneRX") g_DeadzoneRX = std::stof(value);
                else if (key == "DeadzoneRY") g_DeadzoneRY = std::stof(value);
                else if (key == "SensLX")     g_SensLX = std::stof(value);
                else if (key == "SensLY")     g_SensLY = std::stof(value);
                else if (key == "SensRX")     g_SensRX = std::stof(value);
                else if (key == "SensRY")     g_SensRY = std::stof(value);
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
    if (g_FovMultiplierX < 0.1f) g_FovMultiplierX = 1.0f; 
    if (g_FovMultiplierY < 0.1f) g_FovMultiplierY = 1.0f; 
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
ovrHmdDesc* __cdecl Hooked_GetHmdDesc(ovrHmdDesc* retbuf, ovrSession session) {
    DebugLog("[FOV] Hooked_GetHmdDesc CALLED, FovX=%.2f FovY=%.2f", g_FovMultiplierX, g_FovMultiplierY);
    Real_GetHmdDesc(retbuf, session);
    if (g_FovMultiplierX != 1.0f || g_FovMultiplierY != 1.0f) {
        DebugLog("[FOV] Before: DefaultEyeFov[0] Up=%.4f Down=%.4f Left=%.4f Right=%.4f",
            retbuf->DefaultEyeFov[0].UpTan, retbuf->DefaultEyeFov[0].DownTan,
            retbuf->DefaultEyeFov[0].LeftTan, retbuf->DefaultEyeFov[0].RightTan);
        for (int i = 0; i < 2; ++i) {
            retbuf->DefaultEyeFov[i].UpTan *= g_FovMultiplierY;
            retbuf->DefaultEyeFov[i].DownTan *= g_FovMultiplierY;
            retbuf->DefaultEyeFov[i].LeftTan *= g_FovMultiplierX;
            retbuf->DefaultEyeFov[i].RightTan *= g_FovMultiplierX;
            retbuf->MaxEyeFov[i].UpTan *= g_FovMultiplierY;
            retbuf->MaxEyeFov[i].DownTan *= g_FovMultiplierY;
            retbuf->MaxEyeFov[i].LeftTan *= g_FovMultiplierX;
            retbuf->MaxEyeFov[i].RightTan *= g_FovMultiplierX;
        }
        DebugLog("[FOV] After: DefaultEyeFov[0] Up=%.4f Down=%.4f Left=%.4f Right=%.4f",
            retbuf->DefaultEyeFov[0].UpTan, retbuf->DefaultEyeFov[0].DownTan,
            retbuf->DefaultEyeFov[0].LeftTan, retbuf->DefaultEyeFov[0].RightTan);
    }
    return retbuf;
}

ovrEyeRenderDesc* __cdecl Hooked_GetRenderDesc(ovrEyeRenderDesc* retbuf, ovrSession session, ovrEyeType eyeType, ovrFovPort fov) {
    DebugLog("[FOV] Hooked_GetRenderDesc CALLED, eye=%d, inputFov Up=%.4f Down=%.4f Left=%.4f Right=%.4f",
        eyeType, fov.UpTan, fov.DownTan, fov.LeftTan, fov.RightTan);
    if (g_FovMultiplierX != 1.0f || g_FovMultiplierY != 1.0f) {
        fov.UpTan *= g_FovMultiplierY;
        fov.DownTan *= g_FovMultiplierY;
        fov.LeftTan *= g_FovMultiplierX;
        fov.RightTan *= g_FovMultiplierX;
    }
    Real_GetRenderDesc(retbuf, session, eyeType, fov);
    if (g_FovMultiplierX != 1.0f || g_FovMultiplierY != 1.0f) {
        retbuf->Fov.UpTan *= g_FovMultiplierY;
        retbuf->Fov.DownTan *= g_FovMultiplierY;
        retbuf->Fov.LeftTan *= g_FovMultiplierX;
        retbuf->Fov.RightTan *= g_FovMultiplierX;
    }
    return retbuf;
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

        // --- STICK DEADZONES & SENSITIVITY ---
        for (int i = 0; i < 2; ++i) {
            float dzX = (i == 0) ? g_DeadzoneLX : g_DeadzoneRX;
            float dzY = (i == 0) ? g_DeadzoneLY : g_DeadzoneRY;
            float sensX = (i == 0) ? g_SensLX : g_SensRX;
            float sensY = (i == 0) ? g_SensLY : g_SensRY;

            // Apply independent X and Y deadzones FIRST
            if (fabs(inputState->Thumbstick[i].x) < dzX) {
                inputState->Thumbstick[i].x = 0.0f;
            }
            if (fabs(inputState->Thumbstick[i].y) < dzY) {
                inputState->Thumbstick[i].y = 0.0f;
            }

            // Apply independent X and Y sensitivity AFTER
            inputState->Thumbstick[i].x *= sensX;
            inputState->Thumbstick[i].y *= sensY;

            // Clamp results to [-1.0, 1.0]
            if (inputState->Thumbstick[i].x > 1.0f) inputState->Thumbstick[i].x = 1.0f;
            if (inputState->Thumbstick[i].x < -1.0f) inputState->Thumbstick[i].x = -1.0f;
            if (inputState->Thumbstick[i].y > 1.0f) inputState->Thumbstick[i].y = 1.0f;
            if (inputState->Thumbstick[i].y < -1.0f) inputState->Thumbstick[i].y = -1.0f;
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
    DebugLog("[HOOKS] LibOVRRT64_1.dll search: %s (after %d attempts)", hLibOVR ? "FOUND" : "NOT FOUND", attempts);
    if (!hLibOVR) return;

    Real_SubmitControllerVibration = (pf_SubmitControllerVibration)GetProcAddress(hLibOVR, "ovr_SubmitControllerVibration");
    Real_GetInputState = (pf_GetInputState)GetProcAddress(hLibOVR, "ovr_GetInputState");
    Real_GetHmdDesc = (pf_GetHmdDesc)GetProcAddress(hLibOVR, "ovr_GetHmdDesc");
    Real_GetRenderDesc = (pf_GetRenderDesc)GetProcAddress(hLibOVR, "ovr_GetRenderDesc");

    DebugLog("[HOOKS] Function addresses: SubmitVib=%p, InputState=%p, HmdDesc=%p, RenderDesc=%p",
        Real_SubmitControllerVibration, Real_GetInputState, Real_GetHmdDesc, Real_GetRenderDesc);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (Real_SubmitControllerVibration) DetourAttach(&(PVOID&)Real_SubmitControllerVibration, Hooked_SubmitControllerVibration);
    if (Real_GetInputState) DetourAttach(&(PVOID&)Real_GetInputState, Hooked_GetInputState);
    if (Real_GetHmdDesc) DetourAttach(&(PVOID&)Real_GetHmdDesc, Hooked_GetHmdDesc);
    if (Real_GetRenderDesc) DetourAttach(&(PVOID&)Real_GetRenderDesc, Hooked_GetRenderDesc);
    LONG result = DetourTransactionCommit();
    DebugLog("[HOOKS] DetourTransactionCommit result: %ld", result);
    DebugLog("[HOOKS] InstallHooks complete.");
}

typedef int (WINAPI *pf_MessageBoxW)(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType);
typedef int (WINAPI *pf_MessageBoxA)(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);

pf_MessageBoxW Real_MessageBoxW = MessageBoxW;
pf_MessageBoxA Real_MessageBoxA = MessageBoxA;

int WINAPI Hooked_MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType) {
    if (lpText && wcsstr(lpText, L"Failed to initialize Oculus VR session")) {
        LPCWSTR newText = L"Failed to initialize VR session.\n\nPlease ensure SteamVR is fully loaded and that your headset is properly connected and detected by SteamVR.";
        return Real_MessageBoxW(hWnd, newText, lpCaption, uType);
    }
    return Real_MessageBoxW(hWnd, lpText, lpCaption, uType);
}

int WINAPI Hooked_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    if (lpText && strstr(lpText, "Failed to initialize Oculus VR session")) {
        LPCSTR newText = "Failed to initialize VR session.\n\nPlease ensure SteamVR is fully loaded and that your headset is properly connected and detected by SteamVR.";
        return Real_MessageBoxA(hWnd, newText, lpCaption, uType);
    }
    return Real_MessageBoxA(hWnd, lpText, lpCaption, uType);
}

void InitializeEarly(HMODULE hModule) {
    // Load config first to check SteamVR mode
    LoadConfig();
    
    if (g_SteamVRMode) {
        // Intercept native Oculus error messages
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)Real_MessageBoxA, Hooked_MessageBoxA);
        DetourAttach(&(PVOID&)Real_MessageBoxW, Hooked_MessageBoxW);
        DetourTransactionCommit();

        if (EnsureSteamVRRunning()) {
            // Give SteamVR a moment to fully initialize its OpenXR runtime
            std::this_thread::sleep_for(std::chrono::seconds(3));
            InjectRevive();
        }
    }
}

void Initialize() {
    // The haptics/FOV/input hooks are installed asynchronously
    // (they wait for LibOVR or Revive DLL to be loaded)
    std::thread(InstallHooks).detach();
}
