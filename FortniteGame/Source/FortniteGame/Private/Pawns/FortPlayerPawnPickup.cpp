#include "pch.h"
#include "Pawns/FortPlayerPawn.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Finder.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <cmath>

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
// These follow 1.8.1's bodies. 1.1 to 1.8.1 keep their own and are left alone.

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

void AFortPlayerPawnAthena::OnCapsuleBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult)
{
#if WITH_SERVER_CODE
    // 1.8.1's, step by step (0x42A570). Only on the server: the console variable must be on, and the pawn not down.
    static IConsoleVariable* AutoResourceGathering = IConsoleManager::Get().FindConsoleVariable(L"Athena.AutoResourceGathering");
    if (!AutoResourceGathering || AutoResourceGathering->GetInt() == 0 || Role != ROLE_Authority || bIsDBNO)
    {
        return;
    }

    // Not a pickup the pawn dropped itself (Core's give drops what didn't fit at the pawn's feet as the pawn's).
    AFortPickup* Pickup = Cast<AFortPickup>(OtherActor);
    if (!Pickup || static_cast<AFortPawn*>(Pickup->PawnWhoDroppedPickup) == this)
    {
        return;
    }

    // Only while the pawn moves (1.8.1 compares each axis of the movement's velocity with KINDA_SMALL_NUMBER).
    ACharacter* Character = Cast<ACharacter>(static_cast<UObject*>(this));
    UCharacterMovementComponent* Movement = Character ? static_cast<UCharacterMovementComponent*>(Character->CharacterMovement) : nullptr;
    if (!Movement)
    {
        return;
    }
    const FVector Velocity = Movement->Velocity;
    constexpr float KindaSmallNumber = 1.e-4f;
    if (std::fabs(Velocity.X) <= KindaSmallNumber && std::fabs(Velocity.Y) <= KindaSmallNumber && std::fabs(Velocity.Z) <= KindaSmallNumber)
    {
        return;
    }

    // Ammo, traps and resources (the item's type, 1.8.1's "bt 0x108002": EFortItemType 1, 15 and 20).
    FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
    const UFortItemDefinition* ItemDefinition = Entry.ItemDefinition;
    const EFortItemType ItemType = ItemDefinition ? static_cast<EFortItemType>(ItemDefinition->ItemType) : EFortItemType::GetMax_None();
    if (!ItemDefinition || (ItemType != EFortItemType::GetAmmo() && ItemType != EFortItemType::GetTrap() && ItemType != EFortItemType::GetWorldResource()))
    {
        return;
    }

    // As a press of interact would (IT_Simple), and on the pickup's own terms (its class's CanInteract).
    if (!Pickup->CanInteract(this))
    {
        return;
    }
    ServerHandlePickup(Pickup, AFortPickup::GetRandomFlyTime(), FVector(0.0f, 0.0f, 1.0f), true);
#endif
}

void AFortPlayerPawnAthena::HookNatives()
{
    UFunction* OverlapFunction = FindStaticFunction_OnCapsuleBeginOverlap();
    if (!OverlapFunction || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }

    // The exec calls the native directly (1.8.1 0xB28BF0 -> 0x42A570, 1.8.2 0xB27D10 -> the shared empty function).
    uint8_t* NativeCall = Finder::ImplementationCall(OverlapFunction);
    const uintptr_t Native = NativeCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5) : 0;
    if (!Native)
    {
        Log::Error(L"Finder: AFortPlayerPawnAthena::OnCapsuleBeginOverlap native call not found");
        return;
    }
    if (!Finder::IsStubFunction(Native))
    {
        Log::Info(L"Finder: AFortPlayerPawnAthena::OnCapsuleBeginOverlap is native in this build (+0x{:X})", Native - Finder::ImageBase());
        return;
    }
    if (!Finder::ResolvePickupCanInteractLive(AFortPickup::PickedUpOffset()))
    {
        Log::Error(L"Pickup: AFortPlayerPawnAthena::OnCapsuleBeginOverlap is stripped and the pickup's CanInteract wasn't found, nothing is picked up by walking over it");
        return;
    }
    Hooks::RedirectCall(L"AFortPlayerPawnAthena::OnCapsuleBeginOverlap", NativeCall, reinterpret_cast<void*>(&OnCapsuleBeginOverlapHK));
}

void AFortPlayerPawnAthena::OnCapsuleBeginOverlapHK(AFortPlayerPawnAthena* This, UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult)
{
    This->OnCapsuleBeginOverlap(OverlappedComp, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);
}
