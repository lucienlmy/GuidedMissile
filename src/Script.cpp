#include "Script.hpp"
#include "Keyboard.hpp"
#include "rage/Joaat.hpp"
#include "rage/scrNativeRegistration.hpp"
#include "rage/scrProgram.hpp"
#include "rage/scrThread.hpp"
#include "rage/scrValue.hpp"
#include <natives.h>

struct DroneLaunchData
{
    uint64_t InstanceId;   // 0
    uint64_t DroneType;    // 1
    Vector3 Unk2;          // 2
    Vector3 DroneStartPos; // 5
    Vector3 Unk8;          // 8
    uint64_t Unk11;        // 11
};
static_assert(sizeof(DroneLaunchData) == 12 * 8);

#define FAIL_PROGRAM_INITIALIZATION            \
    do {                                       \
        isProgramInitializationFailed = false; \
        return;                                \
    } while (0)

#define CLEANUP_SCRIPT                \
    do {                              \
        CleanupGuidedMissileScript(); \
        return;                       \
    } while (0)

bool shouldRunScript = false;
bool isProgramInitializationFailed = false;
uint32_t programLoadCounter = 0;
bool isDroneInitialized = false;
rage::scrThread* scriptThread = nullptr;
uint32_t kosatkaOwnerGlobalIndex = 0;
uint32_t kosatkaEntityGlobalIndex = 0;
uint32_t droneDataStaticIndex = 0;
std::unordered_map<uint64_t, uint32_t> nativeHooksCache;
std::unordered_map<uint32_t, std::vector<uint8_t>> scriptPatchesCache;

void NetworkIsGameInProgressDetour(rage::scrNativeCallContext* ctx)
{
    ctx->m_ReturnValue->Int = 1; // Always return true
}

void GetNetworkTimeDetour(rage::scrNativeCallContext* ctx)
{
    ctx->m_ReturnValue->Int = MISC::GET_GAME_TIMER(); // Network time returns 0 in SP, so we redirect it to game timer
}

void ReadScriptCode(rage::scrProgram* program, const char* pattern, int32_t offset, void* out, uint32_t size)
{
    if (!program || !pattern)
        FAIL_PROGRAM_INITIALIZATION;

    auto address = program->ScanPattern(pattern);
    if (!address.has_value())
        FAIL_PROGRAM_INITIALIZATION;

    auto code = program->GetCode(address.value() + offset);
    if (!code)
        FAIL_PROGRAM_INITIALIZATION;

    memcpy(out, code, size);
}

void ApplyNativeHook(rage::scrProgram* program, uint64_t hash, rage::scrNativeHandler detour)
{
    if (!program)
        FAIL_PROGRAM_INITIALIZATION;

    auto handler = rage::scrNativeRegistration::GetHandler(hash);
    if (!handler)
        FAIL_PROGRAM_INITIALIZATION;

    auto index = program->GetNativeIndex(handler);
    if (!index.has_value())
        FAIL_PROGRAM_INITIALIZATION;

    if (!nativeHooksCache.contains(hash))
        nativeHooksCache[hash] = index.value();

    program->m_Natives[index.value()] = detour;
}

void ApplyScriptPatch(rage::scrProgram* program, const char* pattern, int32_t offset, const std::vector<uint8_t>& patch)
{
    if (!program || !pattern)
        FAIL_PROGRAM_INITIALIZATION;

    auto address = program->ScanPattern(pattern);
    if (!address.has_value())
        FAIL_PROGRAM_INITIALIZATION;

    auto code = program->GetCode(address.value() + offset);
    if (!code)
        FAIL_PROGRAM_INITIALIZATION;

    if (!scriptPatchesCache.contains(address.value()))
        scriptPatchesCache[address.value()] = std::vector<uint8_t>(code, code + patch.size());

    memcpy(code, patch.data(), patch.size());
}

void UndoNativeHooks(rage::scrProgram* program)
{
    if (!program)
        return;

    for (auto& [hash, index] : nativeHooksCache)
    {
        auto handler = rage::scrNativeRegistration::GetHandler(hash);
        if (!handler)
            continue;

        program->m_Natives[index] = handler;
    }

    nativeHooksCache.clear();
}

void UndoScriptPatches(rage::scrProgram* program)
{
    if (!program)
        return;

    for (auto& [pc, bytes] : scriptPatchesCache)
    {
        uint8_t* code = program->GetCode(pc);
        if (!code)
            continue;

        memcpy(code, bytes.data(), bytes.size());
    }

    scriptPatchesCache.clear();
}

void CleanupGuidedMissileScript()
{
    if (auto program = rage::scrProgram::GetProgram("AM_MP_DRONE"_J))
    {
        UndoNativeHooks(program);
        UndoScriptPatches(program);
    }

    if (scriptThread)
        scriptThread->Kill();
    else if (SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH("AM_MP_DRONE"_J) > 0)
        MISC::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME("AM_MP_DRONE");

    scriptThread = nullptr;
    shouldRunScript = false;
    isProgramInitializationFailed = false;
    programLoadCounter = 0;
    isDroneInitialized = false;
}

void RunGuidedMissileScript()
{
    if (!shouldRunScript)
        return;

    if (!scriptThread)
    {
        // Load the script program
        SCRIPT::REQUEST_SCRIPT_WITH_NAME_HASH("AM_MP_DRONE"_J);
        if (!SCRIPT::HAS_SCRIPT_WITH_NAME_HASH_LOADED("AM_MP_DRONE"_J))
        {
            if (++programLoadCounter >= 30)
                CLEANUP_SCRIPT;

            return; // Try next frame
        }

        // Apply native hooks and script patches
        if (auto program = rage::scrProgram::GetProgram("AM_MP_DRONE"_J))
        {
            ReadScriptCode(program, "62 ? ? ? 5D ? ? ? 09", 1, &kosatkaOwnerGlobalIndex, 3);
            ReadScriptCode(program, "62 ? ? ? 2C 05 ? ? 56", 1, &kosatkaEntityGlobalIndex, 3);
            ReadScriptCode(program, "3A ? 41 F4 38 00 58", 1, &droneDataStaticIndex, 1);

            ApplyNativeHook(program, 0x76CD105BCAC6EB9F, NetworkIsGameInProgressDetour);
            ApplyNativeHook(program, 0x7E3F74F641EE6B27, GetNetworkTimeDetour);

            ApplyScriptPatch(program, g_IsEnhanced ? "37 02 72 71 5D" : "71 39 02 72", 0, {0x72, 0x2E, 0x00, 0x01}); // WaitForBroadcastDataPatch
            ApplyScriptPatch(program, "62 ? ? ? 71 57 ? ? 2C 01", 0, {0x71, 0x2E, 0x00, 0x01});                      // ShouldKillOnlineScriptPatch
            ApplyScriptPatch(program, "5D ? ? ? 56 ? ? 72 2E 00 01 5D ? ? ? 06 56", 0, {0x71, 0x2E, 0x00, 0x01});    // ShouldKillDroneScriptPatch
            ApplyScriptPatch(program, "38 00 39 05 38 05 70 58", 0, {0x72, 0x2E, 0x03, 0x01});                       // IsPlayerStateValidPatch
            ApplyScriptPatch(program, "2C 01 ? ? 29 48 9D B1 C4", 0, {0x71, 0x2E, 0x00, 0x01});                      // ShouldBlockDronePatch
            ApplyScriptPatch(program, "3A ? 41 ? 2C 05 ? ? 39 02 2C 01", 0, {0x2E, 0x00, 0x00});                     // ProcessParticipantScriptLaunchPatch
            ApplyScriptPatch(program, "73 38 00 72 38 01", 0, {0x72, 0x2E, 0x03, 0x01});                             // CanReserveMissionObjectsPatch
            ApplyScriptPatch(program, "5D ? ? ? 2A 56 ? ? 5D ? ? ? 06 1F 2A", 0, {0x55, 0x2F, 0x00});                // IsGuidedMissileTriggeredPatch
        }
        else
            CLEANUP_SCRIPT;

        if (isProgramInitializationFailed)
            CLEANUP_SCRIPT;

        // Start the script thread
        DroneLaunchData launchData;
        launchData.InstanceId = 0;
        launchData.DroneType = 8; // 8 = guided missile
        launchData.Unk2 = Vector3();
        launchData.DroneStartPos = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), FALSE);
        launchData.Unk8 = Vector3();
        launchData.Unk11 = 0;
        int id = BUILTIN::START_NEW_SCRIPT_WITH_NAME_HASH_AND_ARGS("AM_MP_DRONE"_J, reinterpret_cast<Any*>(&launchData), sizeof(launchData) / 8, 1424);
        SCRIPT::SET_SCRIPT_WITH_NAME_HASH_AS_NO_LONGER_NEEDED("AM_MP_DRONE"_J);

        // Get a pointer to the thread we've just started
        scriptThread = rage::scrThread::GetThread(id);
        if (!scriptThread)
            CLEANUP_SCRIPT;
    }

    // If somehow the script died, cleanup
    if (scriptThread->GetState() == rage::scrThread::State::KILLED)
        CLEANUP_SCRIPT;

    // If somehow the stack is invalid, cleanup
    auto stack = scriptThread->GetStack();
    if (!stack)
        CLEANUP_SCRIPT;

    // Spoof the kosatka entity with our ped (otherwise script will cleanup)
    *getGlobalPtr(kosatkaOwnerGlobalIndex) = static_cast<uint64_t>(PLAYER::PLAYER_ID());
    *getGlobalPtr(kosatkaEntityGlobalIndex) = static_cast<uint64_t>(PLAYER::PLAYER_PED_ID());

    // Wait until the script is initialized (drone state becomes >= 1)
    if (!isDroneInitialized && stack[droneDataStaticIndex + 244].Int >= 1) // The offset is not likely to change, so not using pattern for it
        isDroneInitialized = true;

    // Cleanup when the drone state becomes 0 again (missile collided or F
    // pressed)
    if (isDroneInitialized && stack[droneDataStaticIndex + 244].Int == 0)
        CLEANUP_SCRIPT;
}

void ScriptMain()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string name = std::filesystem::path(buf).filename().string();
    if (name == "GTA5_Enhanced.exe")
        g_IsEnhanced = true;

    while (true)
    {
        if (IsKeyJustUp(VK_F11) && SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH("AM_MP_DRONE"_J) == 0)
            shouldRunScript = true;

        RunGuidedMissileScript();

        WAIT(0);
    }
}