#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

class UFunction;

namespace Finder
{
    uintptr_t ImageBase();
    uintptr_t TextBegin();
    uintptr_t TextEnd();

    std::vector<uintptr_t> StringRefs(const wchar_t* Text);
    std::vector<uintptr_t> StringRefs(const char* Text);

    uintptr_t FunctionStart(uintptr_t Address);
    uintptr_t FunctionEnd(uintptr_t FunctionStart);

    uintptr_t RelativeTarget(uintptr_t Instruction, uint32_t OperandOffset, uint32_t InstructionLength);
    uintptr_t NextCall(uintptr_t From, size_t MaxDistance);
    std::vector<std::pair<uintptr_t, uintptr_t>> DirectCalls(uintptr_t Begin, uintptr_t End);
    std::vector<uintptr_t> CallsTo(uintptr_t Target, bool bIncludeJumps);

    int32_t VTableIndex(void** VTable, void* Function, int32_t MaxSlots);
    void** VTableContaining(uintptr_t Function, int32_t Slot, int32_t Slots);
    uintptr_t ImportSlot(const char* Module, const char* Symbol);
    bool IsReadable(const void* Pointer, size_t Size);
    uintptr_t LogCategory(const wchar_t* CategoryName);

    bool ResolveRuntime();
    bool ResolveServer();
    bool ResolveObjectGetWorld();
    bool ResolveClient();
    bool ResolveConsoleManager();
    bool ResolvePropertyCPPType();
    bool ResolveScriptStructCopy();
    bool ResolveScriptFrame();
    bool ResolveTextLocalizationManager();
    bool ResolvePlayerControllerConsole();
    bool ResolveProcessEvent();
    bool ResolveInitListen();
    bool ResolveReplicationLive();
    bool ResolveGameModeZoneLive();
    bool ResolveAbilitySystemLive();
    int32_t ImplementationIndex(UFunction* Function);
    bool ResolveWorldItemDefinitionLive();
    bool ResolvePickupLive();
    bool ResolvePickupGiveLive();
    bool ResolveMissionManagerLive(ptrdiff_t GameStateMissionManagerOffset);
    bool ResolveAIDirectorLive();
    bool ResolveLoginLive();
    bool ResolveMissionGenerationLive();
    bool ResolveMissionObjectivesLive();
    bool ResolveObjectiveActivationLive(ptrdiff_t MissionActiveObjectivesOffset);
    bool ResolveCraftingLive();
    uint8_t* NullGetCurrentCall(UFunction* Function);
    bool ResolveWorldItemLive(void** InventoryOwnerVTable);
    bool ResolveMcpProfileWorldLive(void** ProfileVTable);
    bool ResolveGoalManagerLive();
    bool ResolveQueryGeneratorsLive(void** EnemiesVTable);
    std::vector<uint8_t*> GoalManagerGetCurrentCalls(void** GoalManagerVTable, void** DirectorVTable, int32_t GoalManagerSize, int32_t ActorSize, uintptr_t& OutStub);
    uint8_t* FirstCallInImplementation(UFunction* Function, uintptr_t Target);
    bool ResolveZoneEndLive(ptrdiff_t MissionInfoOffset, ptrdiff_t ZoneFinishedOffset, ptrdiff_t ZoneCompletedOffset, ptrdiff_t EndOfZoneRemainTimeOffset, ptrdiff_t ActiveSpawnPadOffset);
    bool ResolveInventoryOwnerLive(UFunction* ServerRemoveInventoryItem, void** PlayerControllerVTable);
}
