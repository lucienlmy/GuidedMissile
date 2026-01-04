#include "Script.hpp"
#include "Keyboard.hpp"
#include "rage/Joaat.hpp"
#include "rage/scrNativeRegistration.hpp"
#include "rage/scrProgram.hpp"
#include "rage/scrThread.hpp"
#include "rage/scrValue.hpp"
#include <enums.h>
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

#define FAIL_PROGRAM_INITIALIZATION           \
    do                                        \
    {                                         \
        isProgramInitializationFailed = true; \
        return;                               \
    } while (0)

#define CLEANUP_SCRIPT                \
    do                                \
    {                                 \
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
uint32_t submarineMissilesDistanceTunableOffset = 0;
uint32_t terrorbyteDroneHeightLimitTunableOffset = 0;
uint32_t droneDataStaticIndex = 0;
uint32_t droneStateStaticOffset = 0;
uint64_t networkIsGameInProgressHash = 0;
uint64_t GetNetworkTimeHash = 0;
std::unordered_map<uint32_t, std::vector<uint8_t>> scriptPatchesCache;
std::unordered_map<uint64_t, uint32_t> nativeHooksCache;

void NetworkIsGameInProgressDetour(rage::scrNativeCallContext* ctx)
{
    ctx->m_ReturnValue->Int = 1; // Always return true
}

void GetNetworkTimeDetour(rage::scrNativeCallContext* ctx)
{
    ctx->m_ReturnValue->Int = MISC::GET_GAME_TIMER(); // Network time returns 0 in SP, so we redirect it to game timer
}

void ReadScriptCode(rage::scrProgram* program, const char* pattern, const std::vector<std::pair<uint32_t, bool>>& offsetAndRip, void* out, uint32_t size)
{
    if (!program || !pattern)
    {
        LOG("Script program or pattern is invalid, failing initialization.");
        FAIL_PROGRAM_INITIALIZATION;
    }

    auto addressOpt = program->ScanPattern(pattern);
    if (!addressOpt.has_value())
    {
        LOGF("Failed to find script pattern '%s' in script program %s, failing initialization.", pattern, program->m_Name);
        FAIL_PROGRAM_INITIALIZATION;
    }

    uint32_t address = addressOpt.value();

    for (const auto& [offset, rip] : offsetAndRip)
    {
        address += offset;
        if (rip)
        {
            auto code = program->GetCode(address);
            if (!code)
            {
                LOGF("Code for script program %s is invalid, failing initialization.", program->m_Name);
                FAIL_PROGRAM_INITIALIZATION;
            }

            address = code[0] | (code[1] << 8) | (code[2] << 16);
        }
    }

    auto code = program->GetCode(address);
    if (!code)
    {
        LOGF("Code for script program %s is invalid, failing initialization.", program->m_Name);
        FAIL_PROGRAM_INITIALIZATION;
    }

    memcpy(out, code, size);

    int32_t value = 0;
    memcpy(&value, out, size);
    LOGF("Read script code at 0x%08X in script program %s, value is %d.", address, program->m_Name, value);
}

void ApplyScriptPatch(rage::scrProgram* program, const char* pattern, const std::vector<std::pair<uint32_t, bool>>& offsetAndRip, const std::vector<uint8_t>& patch)
{
    if (!program || !pattern)
    {
        LOG("Script program or pattern is invalid, failing initialization.");
        FAIL_PROGRAM_INITIALIZATION;
    }

    auto addressOpt = program->ScanPattern(pattern);
    if (!addressOpt.has_value())
    {
        LOGF("Failed to find script pattern '%s' in script program %s, failing initialization.", pattern, program->m_Name);
        FAIL_PROGRAM_INITIALIZATION;
    }

    uint32_t address = addressOpt.value();

    for (const auto& [offset, rip] : offsetAndRip)
    {
        address += offset;
        if (rip)
        {
            auto code = program->GetCode(address);
            if (!code)
            {
                LOGF("Code for script program %s is invalid, failing initialization.", program->m_Name);
                FAIL_PROGRAM_INITIALIZATION;
            }

            address = code[0] | (code[1] << 8) | (code[2] << 16);
        }
    }

    auto code = program->GetCode(address);
    if (!code)
    {
        LOGF("Code for script program %s is invalid, failing initialization.", program->m_Name);
        FAIL_PROGRAM_INITIALIZATION;
    }

    if (!scriptPatchesCache.contains(address))
        scriptPatchesCache[address] = std::vector<uint8_t>(code, code + patch.size());

    memcpy(code, patch.data(), patch.size());
    LOGF("Applied script patch at 0x%08X in script program %s.", address, program->m_Name);
}

void ApplyNativeHook(rage::scrProgram* program, uint64_t hash, rage::scrNativeHandler detour)
{
    if (!program)
    {
        LOG("Script program is invalid, failing initialization.");
        FAIL_PROGRAM_INITIALIZATION;
    }

    auto handler = rage::scrNativeRegistration::GetHandler(hash);
    if (!handler)
    {
        LOGF("Failed to find native handler for hash 0x%016llX, failing initialization.", hash);
        FAIL_PROGRAM_INITIALIZATION;
    }

    auto index = program->GetNativeIndex(handler);
    if (!index.has_value())
    {
        LOGF("Failed to find native index for handler 0x%016llX in script program %s, failing initialization.", handler, program->m_Name);
        FAIL_PROGRAM_INITIALIZATION;
    }

    if (!nativeHooksCache.contains(hash))
        nativeHooksCache[hash] = index.value();

    program->m_Natives[index.value()] = detour;
    LOGF("Applied native hook for hash 0x%016llX at index %d in script program %s.", hash, index.value(), program->m_Name);
}

void UndoScriptPatches(rage::scrProgram* program)
{
    if (!program)
        return;

    for (auto& [pc, bytes] : scriptPatchesCache)
    {
        auto code = program->GetCode(pc);
        if (!code)
            continue;

        memcpy(code, bytes.data(), bytes.size());
    }

    scriptPatchesCache.clear();
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

void CleanupGuidedMissileScript()
{
    if (auto program = rage::scrProgram::GetProgram("AM_MP_DRONE"_J))
    {
        UndoScriptPatches(program);
        UndoNativeHooks(program);
    }

    if (kosatkaOwnerGlobalIndex != 0)
        *getGlobalPtr(kosatkaOwnerGlobalIndex) = 0;

    if (kosatkaEntityGlobalIndex != 0)
        *getGlobalPtr(kosatkaEntityGlobalIndex) = 0;

    // Set tunables to their default values
    if (submarineMissilesDistanceTunableOffset != 0)
        *getGlobalPtr(262145 + submarineMissilesDistanceTunableOffset) = 4000;

    if (terrorbyteDroneHeightLimitTunableOffset != 0)
        *getGlobalPtr(262145 + terrorbyteDroneHeightLimitTunableOffset) = 200;

    if (scriptThread)
        scriptThread->Kill();
    else
        MISC::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME("AM_MP_DRONE"); // Make sure we free the program

    scriptThread = nullptr;
    shouldRunScript = false;
    isProgramInitializationFailed = false;
    programLoadCounter = 0;
    isDroneInitialized = false;
    kosatkaOwnerGlobalIndex = 0;
    kosatkaEntityGlobalIndex = 0;
    submarineMissilesDistanceTunableOffset = 0;
    terrorbyteDroneHeightLimitTunableOffset = 0;
    droneDataStaticIndex = 0;
    droneStateStaticOffset = 0;
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
            {
                LOG("Failed to load script program, cleaning up.");
                CLEANUP_SCRIPT;
            }

            LOGF("Loading script program, counter is %d.", programLoadCounter);
            return; // Try next frame
        }

        // Read script variables and apply native hooks and script patches
        if (auto program = rage::scrProgram::GetProgram("AM_MP_DRONE"_J))
        {
            LOG("Script program loaded.");

            // Tested these patterns on b2545 and above, should work down to b2189 as well though
            ReadScriptCode(program, "56 ? ? 5D ? ? ? 5D ? ? ? 2A", {{4, true}, {6, false}}, &kosatkaOwnerGlobalIndex, 3);      // addr + 4 + RIP + 5 + 1
            ReadScriptCode(program, "56 ? ? 5D ? ? ? 5D ? ? ? 2A", {{4, true}, {17, false}}, &kosatkaEntityGlobalIndex, 3);    // addr + 4 + RIP + 5 + 4 + 4 + 3 + 1
            ReadScriptCode(program, "56 ? ? 61 01 00 04 47 ? ? 2C", {{8, false}}, &submarineMissilesDistanceTunableOffset, 2); // No need to wildcard the tunable index as it doesn't change
            ReadScriptCode(program, "61 01 00 04 47 ? ? 39 03 5D ? ? ? 56 ? ? 38 03 3E", {{5, false}}, &terrorbyteDroneHeightLimitTunableOffset, 2);
            ReadScriptCode(program, "78 5D ? ? ? 55 ? ? 73", {{2, true}, {6, false}}, &droneDataStaticIndex, 1);   // addr + 2 + RIP + 5 + 1
            ReadScriptCode(program, "78 5D ? ? ? 55 ? ? 73", {{2, true}, {8, false}}, &droneStateStaticOffset, 1); // addr + 2 + RIP + 5 + 2 + 1

            ApplyScriptPatch(program, g_IsEnhanced ? "37 02 72 71 5D" : "71 39 02 72", {}, {0x72, 0x2E, 0x00, 0x01}); // WaitForBroadcastDataPatch
            ApplyScriptPatch(program, "62 ? ? ? 71 57 ? ? 2C 01", {}, {0x71, 0x2E, 0x00, 0x01});                      // ShouldKillOnlineScriptPatch
            ApplyScriptPatch(program, "5D ? ? ? 56 ? ? 72 2E 00 01 5D ? ? ? 06 56", {}, {0x71, 0x2E, 0x00, 0x01});    // ShouldKillDroneScriptPatch
            ApplyScriptPatch(program, "38 00 39 05 38 05 70 58", {}, {0x72, 0x2E, 0x03, 0x01});                       // IsPlayerStateValidPatch
            ApplyScriptPatch(program, "2C 01 ? ? 29 48 9D B1 C4", {}, {0x71, 0x2E, 0x00, 0x01});                      // ShouldBlockDronePatch
            ApplyScriptPatch(program, "3A ? 41 ? 2C 05 ? ? 39 02 2C 01", {}, {0x2E, 0x00, 0x00});                     // ProcessParticipantScriptLaunchPatch
            ApplyScriptPatch(program, "73 38 00 72 38 01", {}, {0x72, 0x2E, 0x03, 0x01});                             // CanReserveMissionObjectsPatch
            ApplyScriptPatch(program, "5D ? ? ? 2A 56 ? ? 5D ? ? ? 06 1F 2A", {}, {0x55, 0x2F, 0x00});                // IsGuidedMissileTriggeredPatch
            ApplyScriptPatch(program, "29 00 C8 AF C7 29 00 C8", {}, {0x71, 0x2E, 0x00, 0x01});                       // IsDroneOutOfWorldBoundsPatch (min: -90000f, -90000f, -1600f, max: 90000f, 90000f, 2600f)

            ApplyNativeHook(program, networkIsGameInProgressHash, NetworkIsGameInProgressDetour);
            ApplyNativeHook(program, GetNetworkTimeHash, GetNetworkTimeDetour);
        }
        else
        {
            LOG("Failed to load script program, cleaning up.");
            CLEANUP_SCRIPT;
        }

        if (isProgramInitializationFailed)
        {
            LOG("Script program initialization failed, cleaning up.");
            CLEANUP_SCRIPT;
        }

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
        {
            LOG("Failed to create script thread, cleaning up.");
            CLEANUP_SCRIPT;
        }

        LOGF("Script thread created. Thread ID is %d.", id);
    }

    // If somehow the script died, cleanup
    if (scriptThread->GetState() == rage::scrThread::State::KILLED)
    {
        LOG("Script thread state is KILLED, cleaning up.");
        CLEANUP_SCRIPT;
    }

    // If somehow the stack is invalid, cleanup
    auto stack = scriptThread->GetStack();
    if (!stack)
    {
        LOG("Script thread stack is invalid, cleaning up.");
        CLEANUP_SCRIPT;
    }

    // Spoof the kosatka entity with our ped (otherwise script will cleanup)
    *getGlobalPtr(kosatkaOwnerGlobalIndex) = PLAYER::PLAYER_ID();
    *getGlobalPtr(kosatkaEntityGlobalIndex) = PLAYER::PLAYER_PED_ID();

    // Spoof the distance and height limit so that the script doesn't cleanup when the missile gets too far away from the player
    *getGlobalPtr(262145 + submarineMissilesDistanceTunableOffset) = INT_MAX;
    *getGlobalPtr(262145 + terrorbyteDroneHeightLimitTunableOffset) = INT_MAX / 10; // The script multiplies the tunable value by 10, so we divide it by 10 to avoid signed overflow that would wrap it negative.

    // Wait until the script is initialized (drone state becomes >= 1)
    if (!isDroneInitialized && stack[droneDataStaticIndex + droneStateStaticOffset].Int >= 1)
    {
        isDroneInitialized = true;
        LOG("Drone initialized.");
    }

    // Cleanup when the drone state becomes 0 again (missile collided or F pressed)
    if (isDroneInitialized && stack[droneDataStaticIndex + droneStateStaticOffset].Int == 0)
    {
        LOG("Script completed sucessfully, cleaning up.");
        CLEANUP_SCRIPT;
    }
}

bool IsSafeToRunGuidedMissileScript()
{
    if (SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH("AM_MP_DRONE"_J) > 0)
    {
        LOG("Not safe to run the guided missile script, AM_MP_DRONE is already running.");
        return false;
    }

    if (SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH("appInternet"_J) > 0)
    {
        LOG("Not safe to run the guided missile script, appInternet is running.");
        return false;
    }

    if (SCRIPT::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH("appCamera"_J) > 0)
    {
        LOG("Not safe to run the guided missile script, appCamera is running.");
        return false;
    }

    if (!PAD::IS_CONTROL_ENABLED(2, eControl::ControlFrontendPauseAlternate)) // Check if any in-game menu is open (this may have false positives)
    {
        LOG("Not safe to run the guided missile script, an in-game menu is probably active.");
        return false;
    }

    if (HUD::IS_PAUSE_MENU_ACTIVE())
    {
        LOG("Not safe to run the guided missile script, pause menu is active.");
        return false;
    }

    if (HUD::IS_WARNING_MESSAGE_ACTIVE())
    {
        LOG("Not safe to run the guided missile script, warning message is active.");
        return false;
    }

    if (CUTSCENE::IS_CUTSCENE_PLAYING())
    {
        LOG("Not safe to run the guided missile script, a cutscene is playing.");
        return false;
    }

    if (STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS())
    {
        LOG("Not safe to run the guided missile script, player switch is in progress.");
        return false;
    }

    if (CAM::IS_SCREEN_FADING_OUT() || CAM::IS_SCREEN_FADED_OUT() || CAM::IS_SCREEN_FADING_IN())
    {
        LOG("Not safe to run the guided missile script, screen is faded out.");
        return false;
    }

    if (PLAYER::IS_PLAYER_BEING_ARRESTED(PLAYER::PLAYER_ID(), TRUE))
    {
        LOG("Not safe to run the guided missile script, player is being arrested.");
        return false;
    }

    if (ENTITY::IS_ENTITY_DEAD(PLAYER::PLAYER_PED_ID(), FALSE))
    {
        LOG("Not safe to run the guided missile script, player is dead.");
        return false;
    }

    return true;
}

void ScriptMain()
{
    auto gameVersion = getGameVersion();
    if (gameVersion < eGameVersion::VER_1_0_2189_0_STEAM) // If this is a version older than the Cayo Perico DLC, than that means the guided missile is not available in this version
    {
        LOGF("Unsupported game version: %d", static_cast<int>(gameVersion));
        return;
    }

    // Thanks to FiveM for old hashes: https://github.com/citizenfx/fivem/blob/3ece3ade3e27ea03b4745de9a1c8f41ad8d0f0e6/code/components/rage-scripting-five/include/CrossMapping_Universal.h
    switch (gameVersion)
    {
    case eGameVersion::VER_1_0_2189_0_STEAM: // 1.52
    case eGameVersion::VER_1_0_2189_0_NOSTEAM:
    {
        networkIsGameInProgressHash = 0x25DDB354A40FFCDB;
        GetNetworkTimeHash = 0x6CAAB7E78B5D978A;
        break;
    }
    case eGameVersion::VER_1_0_2372_0_STEAM: // 1.57
    case eGameVersion::VER_1_0_2372_0_NOSTEAM:
    {
        networkIsGameInProgressHash = 0x02BFF15CAA701972;
        GetNetworkTimeHash = 0x551F46B3C7DFB654;
        break;
    }
    case eGameVersion::VER_1_0_2545_0_STEAM: // 1.58
    case eGameVersion::VER_1_0_2545_0_NOSTEAM:
    {
        networkIsGameInProgressHash = 0x9315DBF7D972F07A;
        GetNetworkTimeHash = 0x0A89FDFA763DCAED;
        break;
    }
    case eGameVersion::VER_1_0_2802_0: // 1.64
    {
        networkIsGameInProgressHash = 0xA26A9A07F761D8F8;
        GetNetworkTimeHash = 0x0DB7F8294D73598B;
        break;
    }
    default: // 2944/1.67 and above
    {
        networkIsGameInProgressHash = 0x76CD105BCAC6EB9F;
        GetNetworkTimeHash = 0x7E3F74F641EE6B27;
        break;
    }
    }

    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string name = std::filesystem::path(buf).filename().string();
    if (name == "GTA5_Enhanced.exe")
        g_IsEnhanced = true;

    LOGF("Game type is %s. Game version is %d.", g_IsEnhanced ? "Enhanced" : "Legacy", static_cast<int>(gameVersion));

    while (true)
    {
        if (IsKeyJustUp(VK_F11) && IsSafeToRunGuidedMissileScript())
        {
            shouldRunScript = true;
            LOG("Key pressed, running the guided missile script.");
        }

        RunGuidedMissileScript();

        WAIT(0);
    }
}