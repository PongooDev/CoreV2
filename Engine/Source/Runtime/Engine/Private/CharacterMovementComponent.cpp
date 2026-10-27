#include "pch.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Pawn.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <unordered_map>

DEFINE_LOG_CATEGORY_STATIC(LogCharacterMovement, Log, All);

namespace
{
    // What one player's moves looked like to the server since its last summary.
    struct FClientErrorStats
    {
        float LastTimeStamp = 0.0f;
        float LastServerTime = 0.0f;
        float WindowStart = 0.0f;
        float ClientTime = 0.0f;
        float ServerTime = 0.0f;
        float MaxError = 0.0f;
        int32_t Moves = 0;
        int32_t Errors = 0;
        int32_t Logged = 0;
    };

    std::unordered_map<const UCharacterMovementComponent*, FClientErrorStats> ClientErrorStats;

    constexpr int32_t MaxLoggedErrors = 20;
    constexpr float SummaryInterval = 10.0f;
    // AGameNetworkManager::MAXPOSITIONERRORSQUARED, which Fortnite's DefaultGame.ini leaves at the engine's 3.
    constexpr float MaxPositionErrorSquared = 3.0f;
    // PackNetworkMovementMode: a custom mode is sent as 16 + its custom mode, any other as its mode in the low 3 bits.
    constexpr uint8_t PackedCustomModeStart = 16;
    constexpr uint8_t PackedModeMask = 0x07;
    constexpr uint8_t MoveCustom = 6;
}

void UCharacterMovementComponent::SetMovementMode(uint8_t NewMovementMode, uint8_t NewCustomMode)
{
    Reflection::FParams Params(StaticFunction_SetMovementMode());
    Params.Set(L"NewMovementMode", NewMovementMode);
    Params.Set(L"NewCustomMode", NewCustomMode);
    Params.Call(this);
}

void UCharacterMovementComponent::ReportClientError(float TimeStamp, const FVector& ClientLoc, uint8_t CompressedMoveFlags, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode)
{
    // ServerMoveDual's first move carries no client location (1, 2, 3) and isn't checked. The pawn's capsule moves
    // unattached, so its relative location is where it is.
    APawn* Pawn = CharacterOwner;
    USceneComponent* Updated = UpdatedComponent;
    UWorld* World = Pawn ? Pawn->GetWorld() : nullptr;
    if (!World || !Updated || Updated->AttachParent || (ClientLoc.X == 1.0f && ClientLoc.Y == 2.0f && ClientLoc.Z == 3.0f))
    {
        return;
    }

    if (ClientErrorStats.size() > 256)
    {
        ClientErrorStats.clear();
    }
    FClientErrorStats& Stats = ClientErrorStats[this];
    const float Now = World->TimeSeconds;
    const float ClientDelta = Stats.LastTimeStamp > 0.0f && TimeStamp > Stats.LastTimeStamp ? TimeStamp - Stats.LastTimeStamp : 0.0f;
    const float ServerDelta = Stats.LastServerTime > 0.0f ? Now - Stats.LastServerTime : 0.0f;
    Stats.LastTimeStamp = TimeStamp;
    Stats.LastServerTime = Now;
    Stats.ClientTime += ClientDelta;
    Stats.ServerTime += ServerDelta;
    ++Stats.Moves;

    // ServerMoveHandleClientError's check (ServerCheckClientError, 1.7.2 0x20213E0): the location the move ended at on the
    // server against the client's, which on a movable base is relative to the base's location, then the movement mode.
    // A bone of a skeletal base can't be placed from here: those moves only have their mode checked.
    const bool bRelative = ClientMovementBase && ClientMovementBase->Mobility == EComponentMobility::Movable;
    const bool bPlaceable = !bRelative || (ClientBaseBoneName.IsNone() && !ClientMovementBase->AttachParent);
    const FVector LocDiff = Updated->RelativeLocation - (bRelative && bPlaceable ? ClientLoc + ClientMovementBase->RelativeLocation : ClientLoc);
    const uint8_t ServerMode = MovementMode;
    const uint8_t ServerCustomMode = CustomMovementMode;
    const bool bModeDiffers = ClientMovementMode < PackedCustomModeStart ? (ClientMovementMode & PackedModeMask) != ServerMode : ServerMode != MoveCustom || ClientMovementMode - PackedCustomModeStart != ServerCustomMode;
    if ((bPlaceable && LocDiff.SizeSquared() > MaxPositionErrorSquared) || bModeDiffers)
    {
        ++Stats.Errors;
        Stats.MaxError = std::max(Stats.MaxError, bPlaceable ? LocDiff.Size() : 0.0f);
        if (Stats.Logged < MaxLoggedErrors)
        {
            ++Stats.Logged;
            const FVector ServerVelocity = Velocity;
            UE_LOG(LogCharacterMovement, Warning, TEXT("%s: the server disagrees with the move at %.3f (client +%.3fs, server +%.3fs): off by %.1f (%.1f, %.1f, %.1f)%s, client mode %d, server mode %d/%d, server velocity (%.0f, %.0f, %.0f), move flags 0x%02X"),
                *Pawn->GetName(), TimeStamp, ClientDelta, ServerDelta, bPlaceable ? LocDiff.Size() : 0.0f, LocDiff.X, LocDiff.Y, LocDiff.Z, bRelative ? TEXT(" on a movable base") : TEXT(""),
                ClientMovementMode, ServerMode, ServerCustomMode, ServerVelocity.X, ServerVelocity.Y, ServerVelocity.Z, CompressedMoveFlags);
        }
    }

    // The two clocks show the engine's movement time discrepancy (ProcessClientTimeStampForTimeDiscrepancy, 0x201A9D0):
    // a client clock 0.25s ahead of the server's makes it resolve the difference by shortening moves and correcting them.
    if (Now - Stats.WindowStart < SummaryInterval)
    {
        return;
    }
    if (Stats.Errors > 0)
    {
        UE_LOG(LogCharacterMovement, Warning, TEXT("%s: the server disagreed with %d of %d moves in the last %.0fs (largest %.1f), client clock +%.2fs, server clock +%.2fs"),
            *Pawn->GetName(), Stats.Errors, Stats.Moves, Now - Stats.WindowStart, Stats.MaxError, Stats.ClientTime, Stats.ServerTime);
    }
    Stats.WindowStart = Now;
    Stats.ClientTime = 0.0f;
    Stats.ServerTime = 0.0f;
    Stats.MaxError = 0.0f;
    Stats.Moves = 0;
    Stats.Errors = 0;
}

void UCharacterMovementComponent::HookServerMove(const wchar_t* ClassPath)
{
    if (!Finder::ResolveServerMoveLive())
    {
        return;
    }

    UClass* MovementClass = FindObject<UClass>(nullptr, ClassPath);
    UObject* MovementDefault = MovementClass ? MovementClass->GetDefaultObject(false) : nullptr;
    void** MovementVTable = MovementDefault ? *reinterpret_cast<void***>(MovementDefault) : nullptr;
    const int32_t Index = Natives::UCharacterMovementComponent_ServerMove_ImplementationIndex;
    void* const Original = MovementVTable ? MovementVTable[Index] : nullptr;
    if (!Original)
    {
        Log::Error(L"Finder: {} has no ServerMove_Implementation", ClassPath);
        return;
    }

    ServerMove_ImplementationOG = reinterpret_cast<decltype(ServerMove_ImplementationOG)>(Original);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(MovementClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Original)
        {
            Hooks::SwapVTable(L"UCharacterMovementComponent::ServerMove_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerMove_ImplementationHK));
        }
    }
}

void UCharacterMovementComponent::ServerMove_ImplementationHK(UCharacterMovementComponent* This, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, uint8_t CompressedMoveFlags, uint8_t ClientRoll, uint32_t View, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode)
{
    ServerMove_ImplementationOG(This, TimeStamp, InAccel, ClientLoc, CompressedMoveFlags, ClientRoll, View, ClientMovementBase, ClientBaseBoneName, ClientMovementMode);

#if WITH_SERVER_CODE
    This->ReportClientError(TimeStamp, ClientLoc, CompressedMoveFlags, ClientMovementBase, ClientBaseBoneName, ClientMovementMode);
#endif
}
