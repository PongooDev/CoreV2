#include "pch.h"
#include "Pawns/FortPlayerPawn.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Finder.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameplayTagContainer.h"
#include "ObjectLayout.h"
#include "Player/FortPlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <cmath>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// 1.8.2 (CL-3741772) strips both halves of picking an item up on the server (1.8.1 CL-3729133 has them):
//
// - AFortPlayerPawn::ServerHandlePickup_Implementation (slot 0x17A, the pickup RPC) is the shared empty function in
//   AFortPlayerPawn's vtable and AFortPlayerPawnAthena's, which in 1.8.1 hold 0x80A2C0 and Athena's override 0x4349E0.
//   The client still sends the RPC when the player presses interact on a pickup (the pickup's
//   IFortInteractInterface::LocalOnAttemptInteract, 1.8.2 0x6AF630), but nothing set the pickup flying to the pawn, so
//   Core's give (AFortPickup::GivePickupTo, the arrival's, stripped in every build) never ran: no pickup worked.
// - AFortPlayerPawnAthena::OnCapsuleBeginOverlap (1.8.1 0x42A570) is the shared empty function: its exec (1.8.2
//   0xB27D10) calls 1.8.2 0x1691180, "ret 0". The pawn's BeginPlay still binds it to the capsule's
//   OnComponentBeginOverlap on the server (1.8.2 0x4316A0), and 1.8.2 still registers the console variable it reads
//   (Athena.AutoResourceGathering, "Automatically pick up ammo and other resources."), which nothing reads any more.
//   Walking over ammo, a trap or a resource no longer picked it up.
//
// These follow 1.8.1's bodies. 1.1 to 1.8.1 keep their own and are left alone. The RPC is swapped in the vtables; the
// overlap's exec is replaced in its UFunction (CoreV2 #135: the first restoration redirected the call inside the
// build's exec, and walking over ammo still picked nothing up on 1.8.2).

namespace
{
    // 1.8.2 moved PickupSpeedMultiplier from AFortPlayerPawnAthena (1.8.1 +0x18BC) to AFortPlayerPawn (+0x1390), so the
    // property is looked up on the pawn's own class.
    float PickupSpeedMultiplierOf(AFortPlayerPawn* Pawn)
    {
        UClass* Class = Pawn->GetClass();
        UProperty* Property = Class ? Class->FindPropertyByName(FName(L"PickupSpeedMultiplier", FNAME_Find)) : nullptr;
        if (!Property || Property->ElementSize != static_cast<int32_t>(sizeof(float)))
        {
            return 1.0f;
        }
        return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(Pawn) + Property->GetOffset_ForInternal());
    }

    // Whether the pawn may interact: CanPerformAction(Gameplay.Action.Player.InteractUse), the question 4.1 asks before
    // any pickup (#297). The walk-over pickup (AFortPlayerPawnAthena::OnCapsuleBeginOverlap, 4.1 0xA9A0E0) asks the pawn's
    // (slot 0x168: its ability system's blocked tags) after its DBNO check, and the interact press asks the controller's
    // (0xF9E7A0, slot 0x27C: the same, and the pawn not in stasis) before the pickup's LocalOnAttemptInteract sends
    // ServerHandlePickup. Thanos (the Infinity Gauntlet) has it blocked: he can't pick anything up. The controller's is
    // asked here for both (Core resolves its slot; a pawn in stasis doesn't walk over anything). Where the slot isn't
    // known, or the pawn has no player controller, nothing is refused.
    bool CanInteractUse(AFortPlayerPawn* Pawn)
    {
        AFortPlayerController* PlayerController = Pawn ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller))) : nullptr;
        if (!PlayerController || Natives::AFortPlayerController_CanPerformActionIndex < 0)
        {
            return true;
        }
        const FGameplayTag InteractUse{ FName(L"Gameplay.Action.Player.InteractUse") };
        return CallVirtual<bool>(PlayerController, Natives::AFortPlayerController_CanPerformActionIndex, &InteractUse);
    }
}

void AFortPlayerPawn::ServerHandlePickup(AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound)
{
    Reflection::FParams Params(StaticFunction_ServerHandlePickup());
    Params.Set(L"Pickup", Pickup);
    Params.Set(L"InFlyTime", InFlyTime);
    Params.Set(L"InStartDirection", InStartDirection);
    Params.Set(L"bPlayPickupSound", bPlayPickupSound);
    Params.Call(this);
}

void AFortPlayerPawn::ServerHandlePickup_Implementation(AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound)
{
#if WITH_SERVER_CODE
    // AFortPlayerPawnAthena's override (1.8.1 0x4349E0) divides the fly time by the pawn's PickupSpeedMultiplier, then
    // calls AFortPlayerPawn's (0x80A2C0), which sets a pickup that isn't pending kill flying to the pawn. On its arrival
    // the pickup gives itself to the pawn's inventory owner (AFortPickup::GivePickupTo).
    const float SpeedMultiplier = PickupSpeedMultiplierOf(this);
    if (SpeedMultiplier > 0.0f)
    {
        InFlyTime /= SpeedMultiplier;
    }
    if (!Pickup || Pickup->IsPendingKill())
    {
        Log::Warning(L"Pickup: {} asked for {}", GetName().ToStdString(), Pickup ? L"a pickup that is pending kill" : L"no pickup");
        return;
    }
    // The client asks it before it sends the RPC; asked again so a pawn that can't interact takes nothing.
    if (!CanInteractUse(this))
    {
        Log::Info(L"Pickup: {} can't interact (Gameplay.Action.Player.InteractUse is blocked), so doesn't take {}", GetName().ToStdString(), Pickup->GetName().ToStdString());
        return;
    }
    FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
    const UFortItemDefinition* ItemDefinition = Entry.ItemDefinition;
    Log::Info(L"Pickup: {} picks up {} {} ({:.2f}s)", GetName().ToStdString(), static_cast<int32_t>(Entry.Count), ItemDefinition ? ItemDefinition->GetName().ToStdString() : L"None", InFlyTime);
    Pickup->SetPickupTarget(this, InFlyTime, InStartDirection, bPlayPickupSound);
#endif
}

void AFortPlayerPawn::HookPickupVTables()
{
    const int32_t Index = Finder::ImplementationIndex(StaticFunction_ServerHandlePickup());
    UObject* PawnDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** PawnVTable = PawnDefault ? *reinterpret_cast<void***>(PawnDefault) : nullptr;
    if (Index < 0 || !PawnVTable)
    {
        Log::Error(L"Finder: AFortPlayerPawn::ServerHandlePickup_Implementation slot not found");
        return;
    }
    if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(PawnVTable[Index])))
    {
        Log::Info(L"Finder: AFortPlayerPawn::ServerHandlePickup_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }
    if (!Natives::AFortPickup_SetPickupTarget && !Finder::ResolvePickupGiveLive())
    {
        Log::Error(L"Pickup: ServerHandlePickup_Implementation is stripped and AFortPickup::SetPickupTarget wasn't found, nobody can pick anything up");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            Hooks::SwapVTable(L"AFortPlayerPawn::ServerHandlePickup_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerHandlePickup_ImplementationHK));
        }
    }
}

void AFortPlayerPawn::ServerHandlePickup_ImplementationHK(AFortPlayerPawn* This, AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound)
{
    This->ServerHandlePickup_Implementation(Pickup, InFlyTime, InStartDirection, bPlayPickupSound);
}

namespace
{
    // Why an overlap didn't pick anything up. Each reason is said once per pickup, at most MaxRefusalLines times in all
    // (an overlap fires for every actor the capsule touches); "not a pickup" is said once in all.
    enum class EAutoPickupRefusal : uint8_t
    {
        Disabled,
        NotAuthority,
        DBNO,
        CannotInteractUse,
        NotPickup,
        OwnDrop,
        NoMovement,
        Standing,
        NoItem,
        ItemType,
        CannotInteract,
        Count,
    };

    constexpr int32_t MaxRefusalLines = 200;

    void LogRefusal(EAutoPickupRefusal Refusal, AFortPlayerPawnAthena* Pawn, AActor* OtherActor, const std::wstring& Why)
    {
        static std::unordered_set<uint64_t> Logged;
        static int32_t Lines = 0;
        const uint64_t Actor = Refusal == EAutoPickupRefusal::NotPickup ? 0 : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(OtherActor));
        if (Lines >= MaxRefusalLines || !Logged.insert((Actor << 8) | static_cast<uint64_t>(Refusal)).second)
        {
            return;
        }
        if (++Lines == MaxRefusalLines)
        {
            Log::Info(L"Pickup: auto pickup: {} refusals said, no more will be", MaxRefusalLines);
        }
        Log::Info(L"Pickup: auto pickup: {} overlapped {} and didn't pick it up: {}", Pawn->GetName().ToStdString(), OtherActor ? OtherActor->GetName().ToStdString() : L"None", Why);
    }

    // Where the exec steps the SweepResult (a const FHitResult&, 0x88 bytes in 4.16) when it isn't passed by address.
    constexpr size_t SweepResultStorageSize = 0x200;

    // A stripped bool function as 3.5's packed image has it: one or more jumps (jmp short / jmp near) to xor al, al and a
    // return written as lea rsp, [rsp+8]; jmp [rsp-8] (3.5.1 0x657BE0 -> 0x657BD3 -> 0x2C6E85), or a plain stub.
    bool IsFalseStub(uintptr_t Function)
    {
        for (int32_t Jumps = 0; Function && Jumps <= 4; ++Jumps)
        {
            if (Finder::IsStubFunction(Function) || Finder::BodyIs(Function, { 0x32, 0xC0, 0x48, 0x8D, 0x64, 0x24, 0x08, 0xFF, 0x64, 0x24, 0xF8 }))
            {
                return true;
            }
            if (Finder::BodyIs(Function, { 0xEB }))
            {
                Function = Function + 2 + *reinterpret_cast<const int8_t*>(Function + 1);
            }
            else if (Finder::BodyIs(Function, { 0xE9 }))
            {
                Function = Finder::RelativeTarget(Function, 1, 5);
            }
            else
            {
                return false;
            }
        }
        return false;
    }
}

void AFortPlayerPawnAthena::OnCapsuleBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult)
{
#if WITH_SERVER_CODE
    static bool bReached = false;
    if (!bReached)
    {
        bReached = true;
        Log::Info(L"Pickup: auto pickup: the first capsule overlap reached Core ({} overlapped {})", GetName().ToStdString(), OtherActor ? OtherActor->GetName().ToStdString() : L"None");
    }

    // 1.8.1's, step by step (0x42A570). Only on the server: the console variable must be on, and the pawn not down.
    static IConsoleVariable* AutoResourceGathering = IConsoleManager::Get().FindConsoleVariable(L"Athena.AutoResourceGathering");
    if (!AutoResourceGathering || AutoResourceGathering->GetInt() == 0)
    {
        LogRefusal(EAutoPickupRefusal::Disabled, this, OtherActor, AutoResourceGathering ? L"Athena.AutoResourceGathering is 0" : L"Athena.AutoResourceGathering isn't registered");
        return;
    }
    if (Role != ROLE_Authority)
    {
        LogRefusal(EAutoPickupRefusal::NotAuthority, this, OtherActor, L"the pawn isn't the authority");
        return;
    }
    if (bIsDBNO)
    {
        LogRefusal(EAutoPickupRefusal::DBNO, this, OtherActor, L"the pawn is down (DBNO)");
        return;
    }
    // 4.1's (0xA9A0E0) asks CanPerformAction(InteractUse) next (#297).
    if (!CanInteractUse(this))
    {
        LogRefusal(EAutoPickupRefusal::CannotInteractUse, this, OtherActor, L"the pawn can't interact (Gameplay.Action.Player.InteractUse is blocked)");
        return;
    }

    // Not a pickup the pawn dropped itself (Core's give drops what didn't fit at the pawn's feet as the pawn's).
    AFortPickup* Pickup = Cast<AFortPickup>(OtherActor);
    if (!Pickup)
    {
        LogRefusal(EAutoPickupRefusal::NotPickup, this, OtherActor, L"not a pickup");
        return;
    }
    if (static_cast<AFortPawn*>(Pickup->PawnWhoDroppedPickup) == this)
    {
        LogRefusal(EAutoPickupRefusal::OwnDrop, this, OtherActor, L"the pawn dropped it");
        return;
    }

    // Only while the pawn moves (1.8.1 compares each axis of the movement's velocity with KINDA_SMALL_NUMBER).
    ACharacter* Character = Cast<ACharacter>(static_cast<UObject*>(this));
    UCharacterMovementComponent* Movement = Character ? static_cast<UCharacterMovementComponent*>(Character->CharacterMovement) : nullptr;
    if (!Movement)
    {
        LogRefusal(EAutoPickupRefusal::NoMovement, this, OtherActor, L"the pawn has no CharacterMovement");
        return;
    }
    const FVector Velocity = Movement->Velocity;
    constexpr float KindaSmallNumber = 1.e-4f;
    if (std::fabs(Velocity.X) <= KindaSmallNumber && std::fabs(Velocity.Y) <= KindaSmallNumber && std::fabs(Velocity.Z) <= KindaSmallNumber)
    {
        LogRefusal(EAutoPickupRefusal::Standing, this, OtherActor, std::format(L"the pawn isn't moving (velocity {:.1f}, {:.1f}, {:.1f})", Velocity.X, Velocity.Y, Velocity.Z));
        return;
    }

    // Ammo, traps and resources: 1.8.1's "bt 0x108002" is EFortItemType 1, 15 and 20, which are Ammo, Trap and
    // WorldResource in 1.8.1 and 1.8.2 (both have Consumable at 6). They are compared by name here. The definitions'
    // constructors set ItemType (1.8.2: UFortAmmoItemDefinition 0x91E790 to 1, the resource definition's 0x6996A0 to
    // 0x14, the trap's 0x699AB0 to 0xF), and their GetItemType (slot 0x4C, which 1.8.1 calls) returns it.
    FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
    const UFortItemDefinition* ItemDefinition = Entry.ItemDefinition;
    if (!ItemDefinition)
    {
        LogRefusal(EAutoPickupRefusal::NoItem, this, OtherActor, L"the pickup has no item");
        return;
    }
    const EFortItemType ItemType = static_cast<EFortItemType>(ItemDefinition->ItemType);
    if (ItemType != EFortItemType::GetAmmo() && ItemType != EFortItemType::GetTrap() && ItemType != EFortItemType::GetWorldResource())
    {
        LogRefusal(EAutoPickupRefusal::ItemType, this, OtherActor, std::format(L"its {} (item type {}) isn't ammo, a trap or a resource", ItemDefinition->GetName().ToStdString(), static_cast<int32_t>(static_cast<uint8_t>(ItemType))));
        return;
    }

    // As a press of interact would (IT_Simple), and on the pickup's own terms (its class's CanInteract).
    if (!Pickup->CanInteract(this))
    {
        // The Athena pickup's CanInteract (1.8.2 0x69DB90) wants IT_Simple, a pickup not picked up and not in its short
        // interaction lock, and its OptionalOwnerID equal to the requester's WorldPlayerId (-1 for both in Battle Royale).
        AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(static_cast<APlayerState*>(PlayerState));
        LogRefusal(EAutoPickupRefusal::CannotInteract, this, OtherActor, std::format(L"the pickup's CanInteract refused ({} {}, picked up {}, OptionalOwnerID {}, the pawn's WorldPlayerId {})",
            static_cast<int32_t>(Entry.Count), ItemDefinition->GetName().ToStdString(), static_cast<bool>(Pickup->bPickedUp), static_cast<int32_t>(Pickup->OptionalOwnerID),
            FortPlayerState ? static_cast<int32_t>(FortPlayerState->WorldPlayerId) : -2));
        return;
    }

    static bool bPickedUpOnce = false;
    if (!bPickedUpOnce)
    {
        bPickedUpOnce = true;
        Log::Info(L"Pickup: auto pickup: the first one, {} walks over {} {}", GetName().ToStdString(), static_cast<int32_t>(Entry.Count), ItemDefinition->GetName().ToStdString());
    }
    ServerHandlePickup(Pickup, AFortPickup::GetRandomFlyTime(), FVector(0.0f, 0.0f, 1.0f), true);
#endif
}

DEFINE_FUNCTION(AFortPlayerPawnAthena::execOnCapsuleBeginOverlap)
{
    P_GET_OBJECT(UPrimitiveComponent, Z_Param_OverlappedComp);
    P_GET_OBJECT(AActor, Z_Param_OtherActor);
    P_GET_OBJECT(UPrimitiveComponent, Z_Param_OtherComp);
    P_GET_PROPERTY(UIntProperty, Z_Param_OtherBodyIndex);
    P_GET_UBOOL(Z_Param_bFromSweep);
    // The game's exec takes the hit by reference (P_GET_STRUCT_REF); 1.8.1's native never reads it.
    alignas(16) uint8_t SweepResult[SweepResultStorageSize] = {};
    P_STACK.StepCompiledIn<UStructProperty>(SweepResult);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawnAthena*>(Context)->OnCapsuleBeginOverlap(Z_Param_OverlappedComp, Z_Param_OtherActor, Z_Param_OtherComp, Z_Param_OtherBodyIndex, Z_Param_bFromSweep, SweepResult);
    P_NATIVE_END;
}

void AFortPlayerPawnAthena::HookNatives()
{
    UFunction* OverlapFunction = FindStaticFunction_OnCapsuleBeginOverlap();
    if (!OverlapFunction || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }

    // The pawn's BeginPlay binds the UFunction to the capsule's OnComponentBeginOverlap by name, so the overlap reaches it
    // through ProcessEvent and the UFunction's Func, its exec (1.8.1 0xB28BF0, 1.8.2 0xB27D10). The exec calls the native
    // directly, as its last call: 1.8.1 0x42A570, 1.8.2 the shared empty function. Core's exec replaces the build's in
    // the UFunction, so the overlap runs Core's body whatever the exec looks like.
    const uintptr_t Exec = reinterpret_cast<uintptr_t>(OverlapFunction->GetNativeFunc());
    uintptr_t Native = 0;
    if (uint8_t* NativeCall = Finder::ImplementationCall(OverlapFunction))
    {
        Native = Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5);
    }
    else if (const uintptr_t ExecEnd = Exec >= Finder::TextBegin() && Exec < Finder::TextEnd() ? Finder::FunctionEnd(Exec) : 0; ExecEnd > Exec)
    {
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(Exec, ExecEnd);
        Native = Calls.empty() ? 0 : Calls.back().second;
    }
    if (!Native)
    {
        Log::Error(L"Finder: AFortPlayerPawnAthena::OnCapsuleBeginOverlap native call not found (exec +0x{:X})", Exec ? Exec - Finder::ImageBase() : 0);
        return;
    }
    // 3.5 keeps the body (3.5.1 0xA275F0: 1.8.1's checks, then the pawn's pickup handling, 0xA34490), but its first
    // call, a check everything after it waits on, is stripped to a false stub (0x657BE0): walking over ammo picked
    // nothing up (#266). The pickup handling starts with the same check.
    const uintptr_t NativeEnd = Native >= Finder::TextBegin() && Native < Finder::TextEnd() ? Finder::FunctionEnd(Native) : 0;
    const std::vector<std::pair<uintptr_t, uintptr_t>> NativeCalls = NativeEnd > Native ? Finder::DirectCalls(Native, NativeEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    const bool bGatedByStub = !NativeCalls.empty() && IsFalseStub(NativeCalls.front().second);
    if (!Finder::IsStubFunction(Native) && !bGatedByStub)
    {
        Log::Info(L"Finder: AFortPlayerPawnAthena::OnCapsuleBeginOverlap is native in this build (+0x{:X})", Native - Finder::ImageBase());
        return;
    }
    if (bGatedByStub)
    {
        Log::Info(L"Finder: AFortPlayerPawnAthena::OnCapsuleBeginOverlap (+0x{:X}) first calls a stripped check (+0x{:X}), so it never picks anything up",
            Native - Finder::ImageBase(), NativeCalls.front().second - Finder::ImageBase());
    }

    UProperty* SweepResultProperty = OverlapFunction->FindPropertyByName(FName(L"SweepResult", FNAME_Find));
    if (!SweepResultProperty || static_cast<size_t>(SweepResultProperty->ElementSize) > SweepResultStorageSize)
    {
        Log::Error(L"Pickup: AFortPlayerPawnAthena::OnCapsuleBeginOverlap's SweepResult is {} bytes, more than Core's exec holds", SweepResultProperty ? SweepResultProperty->ElementSize : -1);
        return;
    }
    if (!Finder::ResolvePickupCanInteractLive(AFortPickup::PickedUpOffset()))
    {
        Log::Error(L"Pickup: AFortPlayerPawnAthena::OnCapsuleBeginOverlap is stripped and the pickup's CanInteract wasn't found, nothing is picked up by walking over it");
        return;
    }
    OverlapFunction->SetNativeFunc(&execOnCapsuleBeginOverlap);
    Log::Info(L"Hook AFortPlayerPawnAthena::OnCapsuleBeginOverlap: native (the build's exec +0x{:X} called the stub +0x{:X})", Exec - Finder::ImageBase(), Native - Finder::ImageBase());
}
