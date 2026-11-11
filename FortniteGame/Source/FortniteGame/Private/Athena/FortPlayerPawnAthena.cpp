#include "pch.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

void AFortPlayerPawnAthena::StartDBNO()
{
#if WITH_SERVER_CODE
    // 5.40 (CL-4352937) adds UFortWorldItemDefinition::bDropOnDBNO, which The Getaway's jewel sets (Athena_Bling_Pack:
    // "Bring this to a Getaway van!"), so a downed carrier loses it to whoever gets there. The client build has no reader
    // of the flag left (nothing tests its bit, UFortWorldItemDefinition +0x758 bit 2, anywhere in the exe) and no call
    // where one was: the build's StartDBNO (AFortPlayerPawnAthena slot 0x1BD, 0xC68250) differs from 5.30's (0xC58D50)
    // only by the OnPlayerStartDBNO broadcast (0xC688B2), no native code binds that delegate, and the DBNO ability
    // (GAB_AthenaDBNO), the jewel's (GA_Athena_Bling_Pack) and The Getaway's mutator (Mutator_BlingMode_C) drop nothing.
    // So the drop was StartDBNO's server code, which the client build compiles out whole; Core runs it from that
    // broadcast (HandlePlayerStartDBNO, bound to each player's pawn), with bIsDBNO set and before the DBNO ability starts.
    // 21.40 names nothing else for it (its only drops are AFortPlayerController's DropItemsOnPawnDestruction, for
    // EPawnDestructionReason Death and Logout). The items go as a death's do: whole stacks, each its own pickup tossed
    // around the pawn (AFortPlayerControllerAthena::DropInventoryOnDeath).
    if (Role != ROLE_Authority || !bIsDBNO || !UFortWorldItemDefinition::HasDropOnDBNO())
    {
        return;
    }
    AFortPlayerControllerAthena* PlayerController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(static_cast<AController*>(Controller)));
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    if (!Inventory)
    {
        return;
    }

    std::vector<FGuid> Drops;
    TArray<UFortWorldItem*>& Instances = Inventory->Inventory.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* ItemEntry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        const UFortWorldItemDefinition* ItemDefinition = ItemEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition)) : nullptr;
        if (ItemDefinition && ItemDefinition->ShouldDropOnDBNO() && ItemEntry->Count > 0)
        {
            Drops.push_back(ItemEntry->ItemGuid);
        }
    }
    if (Drops.empty())
    {
        return;
    }

    const FVector DownLocation = GetTransform().GetLocation();
    const std::vector<FVector> Spots = AFortPlayerControllerAthena::GetInventoryDropSpots(reinterpret_cast<AActor*>(this), DownLocation, static_cast<int32_t>(Drops.size()));
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    for (size_t Index = 0; Index < Drops.size(); ++Index)
    {
        UFortWorldItem* WorldItem = Inventory->FindWorldItem(Drops[Index]);
        if (!WorldItem)
        {
            continue;
        }
        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        const int32_t Count = ItemEntry.Count;
        EntryStruct->InitializeStruct(DropEntry);
        EntryStruct->CopyScriptStruct(DropEntry, &ItemEntry);
        if (PlayerController->RemoveInventoryItem(Drops[Index], Count, false, true))
        {
            if (AFortPickup* Pickup = PlayerController->SpawnDropPickup(*DropEntry, &DownLocation, nullptr))
            {
                Pickup->PawnWhoDroppedPickup = reinterpret_cast<AFortPawn*>(this);
                Pickup->TossOntoFloor(Spots[Index], reinterpret_cast<AFortPawn*>(this));
                UE_LOG(LogFort, Log, TEXT("StartDBNO: %s went down and dropped %s as %s"), *PlayerController->GetName(),
                    *static_cast<UFortItemDefinition*>(DropEntry->ItemDefinition)->GetName(), *Pickup->GetName());
            }
        }
        EntryStruct->DestroyStruct(DropEntry);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerPawnAthena::execHandlePlayerStartDBNO)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawnAthena*>(Context)->StartDBNO();
    P_NATIVE_END;
}

void AFortPlayerPawnAthena::BindPlayerStartDBNO()
{
#if WITH_SERVER_CODE
    if (HandlePlayerStartDBNOFunction && Role == ROLE_Authority)
    {
        FMulticastScriptDelegate& Delegate = OnPlayerStartDBNO;
        Delegate.AddUniqueDynamic(this, HandlePlayerStartDBNOFunction->GetFName());
    }
#endif
}

void AFortPlayerPawnAthena::StartDBNOHK(AFortPlayerPawnAthena* This)
{
    StartDBNOOG(This);
    This->StartDBNO();
}

void AFortPlayerPawnAthena::HookVTables()
{
    // Only the builds whose items can drop on DBNO (5.40 on); before, StartDBNO has no server part Core restores.
    if (!UFortWorldItemDefinition::HasDropOnDBNO())
    {
        return;
    }

    // The build's StartDBNO broadcasts the pawn's OnPlayerStartDBNO (ProcessMulticastDelegate, 5.40 0x2ECA90), which
    // calls each bound UFunction through ProcessEvent: Core's HandlePlayerStartDBNO, on the pawn's class, runs the server
    // part from there. Each player's pawn binds it as the player gets it (BindPlayerStartDBNO).
    const UProperty* Property = HasOnPlayerStartDBNO() ? StaticClass()->FindPropertyByName(FName(L"OnPlayerStartDBNO")) : nullptr;
    const UFunction* Signature = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
        ? static_cast<UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
    HandlePlayerStartDBNOFunction = Signature ? StaticClass()->AddNativeFunction(L"HandlePlayerStartDBNO", &execHandlePlayerStartDBNO, Signature) : nullptr;
    if (HandlePlayerStartDBNOFunction)
    {
        return;
    }

    // Without it (no OnPlayerStartDBNO, or Core can't add a native to the class): StartDBNO's slot, the build's first.
    Log::Warning(L"DBNO: OnPlayerStartDBNO can't call Core's HandlePlayerStartDBNO, so AFortPlayerPawn::StartDBNO's slot runs the drop");
    const int32_t Index = Finder::StartDBNOIndex(IsDBNOOffset());
    UObject* PawnDefault = Index >= 0 && StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** PawnVTable = PawnDefault ? *reinterpret_cast<void***>(PawnDefault) : nullptr;
    void* Original = PawnVTable ? PawnVTable[Index] : nullptr;
    if (!Original)
    {
        Log::Error(L"DBNO: AFortPlayerPawn::StartDBNO not found, downed players keep the items that drop on DBNO");
        return;
    }
    StartDBNOOG = reinterpret_cast<decltype(StartDBNOOG)>(Original);
    // The subclasses that keep the Athena pawn's StartDBNO (AFortRemoteControlledPawnAthena; the Blueprint pawns share
    // their native class's vtable).
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Original)
        {
            Hooks::SwapVTable(L"AFortPlayerPawn::StartDBNO", VTable, Index, reinterpret_cast<void*>(&StartDBNOHK));
        }
    }
}
