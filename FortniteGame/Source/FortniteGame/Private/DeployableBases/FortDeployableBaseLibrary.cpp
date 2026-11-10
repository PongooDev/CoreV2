#include "pch.h"
#include "DeployableBases/FortDeployableBaseLibrary.h"
#include "AbilitySystemComponent.h"
#include "DeployableBases/FortDeployableBaseManager.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "StructArray.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace
{
    // An exec's parameters, stepped into a buffer laid out as the function's (arrays and structs included).
    class FExecParms
    {
    public:
        FExecParms(void* TheStack, UFunction* InFunction)
            : Function(InFunction)
            , Buffer(InFunction ? std::max<size_t>(InFunction->ParmsSize, InFunction->GetPropertiesSize()) + sizeof(uint64_t) : 0)
        {
            if (!Function)
            {
                return;
            }
            Function->InitializeStruct(Buffer.data());
            for (UField* Field = Function->Children; Field; Field = Field->Next)
            {
                UProperty* Property = static_cast<UProperty*>(Field);
                if ((Property->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
                {
                    P_STACK.StepCompiledIn<UProperty>(Buffer.data() + Property->GetOffset_ForInternal());
                }
            }
            P_FINISH;
        }

        FExecParms(const FExecParms&) = delete;
        FExecParms& operator=(const FExecParms&) = delete;

        ~FExecParms()
        {
            if (Function)
            {
                Function->DestroyStruct(Buffer.data());
            }
        }

        template <typename T>
        T* Find(const wchar_t* Name)
        {
            UProperty* Property = Function ? Function->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
            return Property && static_cast<size_t>(Property->GetSize()) == sizeof(T) ? reinterpret_cast<T*>(Buffer.data() + Property->GetOffset_ForInternal()) : nullptr;
        }

    private:
        UFunction* Function;
        std::vector<uint8_t> Buffer;
    };

    int32_t WorldLevelOf(UObject* WorldContextObject)
    {
        UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
        AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        return GameState ? static_cast<int32_t>(GameState->WorldLevel) : 0;
    }

    std::vector<AFortPlayerController*> PlayerControllersOf(UObject* WorldContextObject)
    {
        std::vector<AFortPlayerController*> PlayerControllers;
        UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
        for (TObjectIterator<AFortPlayerController> It; World && It; ++It)
        {
            if (It->GetWorld() == World && !It->IsPendingKill())
            {
                PlayerControllers.push_back(*It);
            }
        }
        return PlayerControllers;
    }

    // The Horde Skill Tree's unlocks a player owns are gameplay tags on their pawn's ability system (the homebase nodes'
    // tags, Unlocks.Horde.*).
    bool OwnsUnlock(AFortPlayerController* PlayerController, const FGameplayTag& RequiredTag)
    {
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
        if (!AbilitySystem || !RequiredTag.IsValid())
        {
            return !RequiredTag.IsValid();
        }
        FGameplayTagContainer OwnedTags;
        AbilitySystem->GetOwnedGameplayTags(OwnedTags);
        return OwnedTags.HasTag(RequiredTag);
    }

    // The tier groups plus the manager's skill tree unlocks (CombatZoneStart... or CombatZoneWaveComplete...) the player
    // owns.
    std::vector<FName> TierGroupsFor(AFortPlayerController* PlayerController, const TArray<FName>& TierGroups, AFortDeployableBaseManager* Manager, bool bWaveComplete)
    {
        std::vector<FName> Groups;
        for (int32_t Index = 0; Index < TierGroups.Num(); ++Index)
        {
            Groups.push_back(TierGroups[Index]);
        }
        UFortDeployableBaseSkillTreeUnlocks* Unlocks = Manager ? static_cast<UFortDeployableBaseSkillTreeUnlocks*>(Manager->SupportedSkillTreeBasedUnlocks) : nullptr;
        if (!Unlocks)
        {
            return Groups;
        }
        TStructArray<FFortTaggedDeployableBaseLootUnlock>& LootUnlocks = bWaveComplete ? static_cast<TStructArray<FFortTaggedDeployableBaseLootUnlock>&>(Unlocks->CombatZoneWaveCompleteLootGrantUnlocks)
                                                                                        : static_cast<TStructArray<FFortTaggedDeployableBaseLootUnlock>&>(Unlocks->CombatZoneStartLootGrantUnlocks);
        for (int32_t Index = 0; Index < LootUnlocks.Num(); ++Index)
        {
            FFortTaggedDeployableBaseLootUnlock& Unlock = LootUnlocks[Index];
            if (OwnsUnlock(PlayerController, Unlock.RequiredTag))
            {
                Groups.push_back(Unlock.LootTierGroup);
            }
        }
        return Groups;
    }

    // Rolls each tier group at the world's level and gives the player what it drops. Returns the items given.
    int32_t GiveTierGroupLoot(UObject* WorldContextObject, AFortPlayerController* PlayerController, const std::vector<FName>& TierGroups, std::wstring& OutGiven)
    {
        const int32_t WorldLevel = WorldLevelOf(WorldContextObject);
        int32_t Given = 0;
        for (const FName& TierGroup : TierGroups)
        {
            TStructArray<FFortItemEntry> LootDrops{};
            UFortKismetLibrary::PickLootDrops(WorldContextObject, LootDrops, TierGroup, WorldLevel, -1);
            for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
            {
                FFortItemEntry& Entry = LootDrops[Index];
                UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition));
                const int32_t Count = Entry.Count;
                if (!ItemDefinition || Count <= 0)
                {
                    continue;
                }
                UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, ItemDefinition, Count, true, std::max<int32_t>(Entry.Level, 1));
                OutGiven += std::format(L"{}{} x{}", OutGiven.empty() ? L"" : L", ", ItemDefinition->GetName().ToStdString(), Count);
                ++Given;
            }
            LootDrops.Empty();
        }
        return Given;
    }
}

bool UFortDeployableBaseLibrary::IsWorldItemDefValidToPersistForDeployableBasePlayerInventory(const UFortWorldItemDefinition* ItemDefinition)
{
    Reflection::FParams Params(StaticFunction_IsWorldItemDefValidToPersistForDeployableBasePlayerInventory());
    Params.Set(L"ItemDefinition", ItemDefinition);
    return Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
}

void UFortDeployableBaseLibrary::SetupDeployableBaseCombatZoneInventoriesOnPlayers(UObject* WorldContextObject, const TArray<FName>& TierGroups, const TArray<AFortPlayerController*>& PlayerControllers)
{
#if WITH_SERVER_CODE
    AFortDeployableBaseManager* Manager = AFortDeployableBaseManager::GetCurrent(WorldContextObject);
    for (int32_t Index = 0; Index < PlayerControllers.Num(); ++Index)
    {
        AFortPlayerController* PlayerController = PlayerControllers[Index];
        if (!PlayerController)
        {
            continue;
        }
        std::wstring Given;
        const std::vector<FName> Groups = TierGroupsFor(PlayerController, TierGroups, Manager, false);
        const int32_t Count = GiveTierGroupLoot(WorldContextObject, PlayerController, Groups, Given);
        Log::Info(L"Horde: tier start inventory for {}: {} item(s) from {} tier group(s) at world level {}: {}", PlayerController->GetName().ToStdString(), Count, Groups.size(),
            WorldLevelOf(WorldContextObject), Given.empty() ? std::wstring(L"none") : Given);
    }
#endif
}

void UFortDeployableBaseLibrary::GrantPlayersCompletedWaveRewards(UObject* WorldContextObject, int32_t Tier, int32_t Wave, int32_t NumberOfPlayers, const TArray<FName>& TierGroups)
{
#if WITH_SERVER_CODE
    AFortDeployableBaseManager* Manager = AFortDeployableBaseManager::GetCurrent(WorldContextObject);
    const std::vector<AFortPlayerController*> PlayerControllers = PlayerControllersOf(WorldContextObject);
    Log::Info(L"Horde: wave {} of tier {} won ({} player(s) at its start, {} now), granting its rewards", Wave, Tier, NumberOfPlayers, PlayerControllers.size());
    for (AFortPlayerController* PlayerController : PlayerControllers)
    {
        std::wstring Given;
        const std::vector<FName> Groups = TierGroupsFor(PlayerController, TierGroups, Manager, true);
        const int32_t Count = GiveTierGroupLoot(WorldContextObject, PlayerController, Groups, Given);
        Log::Info(L"Horde: wave {} rewards for {}: {} item(s): {}", Wave, PlayerController->GetName().ToStdString(), Count, Given.empty() ? std::wstring(L"none") : Given);
    }
#endif
}

void UFortDeployableBaseLibrary::ResetDeployableBaseHordePlayerInventories(UObject* WorldContextObject)
{
#if WITH_SERVER_CODE
    for (AFortPlayerController* PlayerController : PlayerControllersOf(WorldContextObject))
    {
        AFortInventory* Inventory = PlayerController->WorldInventory;
        if (!Inventory)
        {
            continue;
        }
        std::vector<std::pair<FGuid, int32_t>> Removing;
        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            UFortWorldItem* WorldItem = Instances[Index];
            FFortItemEntry* ItemEntry = WorldItem ? &static_cast<FFortItemEntry&>(WorldItem->ItemEntry) : nullptr;
            UFortWorldItemDefinition* ItemDefinition = ItemEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition)) : nullptr;
            if (ItemDefinition && IsWorldItemDefValidToPersistForDeployableBasePlayerInventory(ItemDefinition))
            {
                Removing.emplace_back(ItemEntry->ItemGuid, static_cast<int32_t>(ItemEntry->Count));
            }
        }
        int32_t Removed = 0;
        for (const auto& [ItemGuid, Count] : Removing)
        {
            Removed += PlayerController->RemoveInventoryItem(ItemGuid, Count, true) ? 1 : 0;
        }
        Log::Info(L"Horde: reset {}'s combat zone inventory ({} of {} item stack(s) removed)", PlayerController->GetName().ToStdString(), Removed, Removing.size());
    }
#endif
}

DEFINE_FUNCTION(UFortDeployableBaseLibrary::execSetupDeployableBaseCombatZoneInventoriesOnPlayers)
{
    FExecParms Parms(TheStack, StaticFunction_SetupDeployableBaseCombatZoneInventoriesOnPlayers());
    UObject** WorldContextObject = Parms.Find<UObject*>(L"WorldContextObject");
    TArray<FName>* TierGroups = Parms.Find<TArray<FName>>(L"TierGroups");
    TArray<AFortPlayerController*>* PlayerControllers = Parms.Find<TArray<AFortPlayerController*>>(L"PlayerControllers");
    P_NATIVE_BEGIN;
    if (WorldContextObject && TierGroups && PlayerControllers)
    {
        SetupDeployableBaseCombatZoneInventoriesOnPlayers(*WorldContextObject, *TierGroups, *PlayerControllers);
    }
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortDeployableBaseLibrary::execGrantPlayersCompletedWaveRewards)
{
    FExecParms Parms(TheStack, StaticFunction_GrantPlayersCompletedWaveRewards());
    UObject** WorldContextObject = Parms.Find<UObject*>(L"WorldContextObject");
    int32_t* Tier = Parms.Find<int32_t>(L"Tier");
    int32_t* Wave = Parms.Find<int32_t>(L"Wave");
    int32_t* NumberOfPlayers = Parms.Find<int32_t>(L"NumberOfPlayers");
    TArray<FName>* TierGroups = Parms.Find<TArray<FName>>(L"TierGroups");
    P_NATIVE_BEGIN;
    if (WorldContextObject && Tier && Wave && NumberOfPlayers && TierGroups)
    {
        GrantPlayersCompletedWaveRewards(*WorldContextObject, *Tier, *Wave, *NumberOfPlayers, *TierGroups);
    }
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortDeployableBaseLibrary::execResetDeployableBaseHordePlayerInventories)
{
    FExecParms Parms(TheStack, StaticFunction_ResetDeployableBaseHordePlayerInventories());
    UObject** WorldContextObject = Parms.Find<UObject*>(L"WorldContextObject");
    P_NATIVE_BEGIN;
    if (WorldContextObject)
    {
        ResetDeployableBaseHordePlayerInventories(*WorldContextObject);
    }
    P_NATIVE_END;
}

void UFortDeployableBaseLibrary::HookNatives()
{
    // Only 5.0's deployable base library has these, and only where the exec's native call is a stub (stripped).
    if (!StaticClass() || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }

    for (const auto& [Function, Native, Name] : {
             std::tuple{ FindStaticFunction_SetupDeployableBaseCombatZoneInventoriesOnPlayers(), &execSetupDeployableBaseCombatZoneInventoriesOnPlayers, L"UFortDeployableBaseLibrary::SetupDeployableBaseCombatZoneInventoriesOnPlayers" },
             std::tuple{ FindStaticFunction_GrantPlayersCompletedWaveRewards(), &execGrantPlayersCompletedWaveRewards, L"UFortDeployableBaseLibrary::GrantPlayersCompletedWaveRewards" },
             std::tuple{ FindStaticFunction_ResetDeployableBaseHordePlayerInventories(), &execResetDeployableBaseHordePlayerInventories, L"UFortDeployableBaseLibrary::ResetDeployableBaseHordePlayerInventories" } })
    {
        uint8_t* Call = Function ? Finder::ImplementationCall(Function) : nullptr;
        if (!Call || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(Call), 1, 5)))
        {
            if (Function)
            {
                Log::Info(L"Hook {}: the build's own (not stripped)", Name);
            }
            continue;
        }
        Function->SetNativeFunc(Native);
        Log::Info(L"Hook {}: native (stripped)", Name);
    }
}
