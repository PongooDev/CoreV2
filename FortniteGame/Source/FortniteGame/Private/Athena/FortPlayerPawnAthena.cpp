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
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

void AFortPlayerPawnAthena::BeginSkydiving(bool bFromBus)
{
#if WITH_SERVER_CODE
    if (!Natives::AFortPlayerPawnAthena_BeginSkydiving)
    {
        return;
    }
    // 1.8.1 and 1.8.2 (CoreV2 #449): the build's BeginSkydiving sets the skydive up (bIsSkydiving, the tag, the contrail,
    // the movement's skydiving mode) and then, with authority, records where and when the player left the bus from
    // Cast<AFortPlayerControllerAthena>(Controller) with no null check: a player bot's AI controller made it read through
    // null (1.8.2 0x415301 into 0x420264), the server died at the first bot's warmup glide or bus exit. Epic's fix is 1.9's
    // BeginSkydiving (0x417110): that record only from the bus and only for a player controller, the flag before it
    // (1.8.2 +0x848) still set. Core can't change the build's code, so for a pawn that isn't a player's it runs the build's
    // function as the pawn's owning client would (ROLE_AutonomousProxy: everything but the server part, which only that
    // Role check gates), puts the authority back, and sets the flag as 1.9 does.
    const ptrdiff_t ServerFlag = Natives::AFortPlayerPawnAthena_BeginSkydiving_UnguardedServerFlagOffset;
    UObject* PawnController = reinterpret_cast<UObject*>(static_cast<AController*>(Controller));
    if (ServerFlag < 0 || Role != ROLE_Authority || Cast<AFortPlayerControllerAthena>(PawnController))
    {
        Natives::AFortPlayerPawnAthena_BeginSkydiving(this, bFromBus);
        return;
    }
    Role = TEnumAsByte<ENetRole>(ROLE_AutonomousProxy);
    Natives::AFortPlayerPawnAthena_BeginSkydiving(this, bFromBus);
    Role = TEnumAsByte<ENetRole>(ROLE_Authority);
    *(reinterpret_cast<uint8_t*>(this) + ServerFlag) = 1;
    static bool bLogged = false;
    if (!bLogged)
    {
        bLogged = true;
        Log::Info(L"Battle Royale: {} skydives without BeginSkydiving's player-controller part, as on 1.9 (controller {}, flag +0x{:X})", GetName().ToStdString(),
            PawnController ? PawnController->GetName().ToStdString() : std::wstring(L"none"), ServerFlag);
    }
#endif
}

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

void AFortPlayerPawnAthena::ForceReviveFromDBNO()
{
#if WITH_SERVER_CODE
    // 6.21 (CL-4526925) adds ForceReviveFromDBNO (Final, BlueprintAuthorityOnly, no parameters), and its body is server
    // code: the exec (0x9F9440, shared with UFortCheatManager::EndDeimosSpawning) steps the frame and tail-jumps to the
    // shared empty function, and so does its one caller, AFortAthenaMutator_ButterflyEvent's pawn readying (0xD39600,
    // the call at 0xD3971B, behind `if (Pawn->bIsDBNO)`). No asset calls it (6.21's client paks and its server-assets
    // pak). 21.40 keeps it with an AController* EventInstigator and a cheat of the same name on UFortCheatManager.
    // 🟡 The body is inferred: "force" is a revive nobody performs, and the build's own revive with the pawn's own
    // controller as the reviver is that (a self revive). It goes through Epic's ReviveFromDBNO (0x15ED220: ProcessEvent
    // of the ServerReviveFromDBNO RPC, which on the server runs ServerReviveFromDBNO_Implementation in place: Core's
    // restored one where the build strips it, 6.21 among them). Its CanReviveFromDBNO (AFortPlayerPawnAthena slot 0x1DF,
    // 0xCD43C0) takes the pawn's own AFortPlayerControllerAthena on the server: the controller's pawn is this one, on
    // its own team, and the server is the authority. GE_Athena_Revive_Self (PlayerPawn_Athena's SelfReviveGameplayEffect)
    // gets it up, as a self revive.
    if (Role != ROLE_Authority || !bIsDBNO)
    {
        return;
    }
    AController* const PawnController = static_cast<AController*>(Controller);
    static UFunction* const Revive = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerPawn:ReviveFromDBNO");
    Reflection::FParams Params(Revive, true);
    if (!PawnController || !Revive || !Params.Set(L"EventInstigator", PawnController) || !Params.Call(this))
    {
        UE_LOG(LogFort, Warning, TEXT("ForceReviveFromDBNO: %s can't be revived (controller %s, ReviveFromDBNO %s)"), *GetName(), *GetNameSafe(PawnController),
            Revive ? TEXT("found") : TEXT("not found"));
        return;
    }
    UE_LOG(LogFort, Log, TEXT("ForceReviveFromDBNO: %s %s"), *GetName(), bIsDBNO ? TEXT("is still down (its controller's revive was refused)") : TEXT("is up"));
#endif
}

DEFINE_FUNCTION(AFortPlayerPawnAthena::execForceReviveFromDBNO)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawnAthena*>(Context)->ForceReviveFromDBNO();
    P_NATIVE_END;
}

void AFortPlayerPawnAthena::ForceReviveFromDBNOHK(AFortPlayerPawnAthena* This)
{
    This->ForceReviveFromDBNO();
}

void AFortPlayerPawnAthena::HookNatives()
{
    // ForceReviveFromDBNO (6.21 on). Its exec steps the frame (no parameters) and tail-jumps to the native: the shared
    // empty function where the build strips it. Core's exec takes the UFunction (BlueprintAuthorityOnly callers), and
    // the Butterfly mutator's direct call (Finder::ButterflyEventForceReviveCall) goes to Core's body, when it goes to
    // the same stub the exec ends in. Builds without it (1.1 to 6.20) and builds that keep it are left alone.
    if (UFunction* ForceRevive = FindStaticFunction_ForceReviveFromDBNO())
    {
        const uintptr_t Exec = reinterpret_cast<uintptr_t>(ForceRevive->GetNativeFunc());
        uintptr_t ExecNative = 0;
        for (uintptr_t Site = Exec; Exec >= Finder::TextBegin() && Site + 5 <= Exec + 0x40 && Site + 5 <= Finder::TextEnd() && !ExecNative; ++Site)
        {
            if (*reinterpret_cast<const uint8_t*>(Site) == 0xE9)
            {
                const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
                ExecNative = Target >= Finder::TextBegin() && Target < Finder::TextEnd() ? Target : 0;
            }
        }
        if (!ExecNative || !Finder::IsStubFunction(ExecNative))
        {
            Log::Info(L"Finder: AFortPlayerPawnAthena::ForceReviveFromDBNO is {} in this build (exec +0x{:X})", ExecNative ? L"native" : L"not a tail jump",
                Exec ? Exec - Finder::ImageBase() : 0);
        }
        else
        {
            ForceRevive->SetNativeFunc(&execForceReviveFromDBNO);
            Log::Info(L"Hook AFortPlayerPawnAthena::ForceReviveFromDBNO: native (the build's exec +0x{:X} ends in the stub +0x{:X})", Exec - Finder::ImageBase(),
                ExecNative - Finder::ImageBase());
            uint8_t* ReviveCall = Finder::ButterflyEventForceReviveCall(IsDBNOOffset());
            if (ReviveCall && Finder::RelativeTarget(reinterpret_cast<uintptr_t>(ReviveCall), 1, 5) == ExecNative)
            {
                Hooks::RedirectCall(L"AFortAthenaMutator_ButterflyEvent::UpdatePawnBroadcasting -> AFortPlayerPawnAthena::ForceReviveFromDBNO", ReviveCall,
                    reinterpret_cast<void*>(&ForceReviveFromDBNOHK));
            }
            else if (ReviveCall)
            {
                Log::Error(L"Finder: AFortAthenaMutator_ButterflyEvent's revive call (+0x{:X}) goes to +0x{:X}, not ForceReviveFromDBNO's stub, so it is left as it is",
                    reinterpret_cast<uintptr_t>(ReviveCall) - Finder::ImageBase(), Finder::RelativeTarget(reinterpret_cast<uintptr_t>(ReviveCall), 1, 5) - Finder::ImageBase());
            }
        }
    }

    HookPickupNatives();
}
