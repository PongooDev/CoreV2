#include "pch.h"
#include "UI/AthenaHUDContext.h"
#include "Engine/LocalPlayer.h"
#include "Finder.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/WeakObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    // EGameplayCueEvent::Removed.
    constexpr uint8_t GameplayCueRemoved = 3;

    TWeakObjectPtr<APawn> BoundPawn;
}

void UAthenaHUDContext::HandleUIGameplayCue(FName CueName, uint8_t EventType)
{
    Reflection::FParams Params(StaticFunction_HandleUIGameplayCue(), true);
    if (Params.Set(L"CueName", CueName) && Params.Set(L"EventType", EventType))
    {
        Params.Call(this);
    }
}

void UAthenaHUDContext::BindLocalPawn()
{
    // The item use timer (AthenaEquipProgress: bandages, med kits, shield potions, chug jugs, being revived) shows on the
    // GameplayCue.Athena.Equipping cue's WhileActive and hides only on its Removed, which reaches the HUD through the local
    // pawn's OnUIGameplayCue. The aircraft phase destroys the warmup pawn in the frame its unequip ends the use ability
    // (#91): the pawn's channel closes at once, the cue's removal replicates with the player state's ability system after
    // it and finds no pawn to play on, and the timer stayed at 0.0 for the rest of the match. When the HUD binds another
    // pawn, or none, the previous pawn's timer ends.
    ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(GetOuter());
    APlayerController* PlayerController = LocalPlayer ? static_cast<APlayerController*>(LocalPlayer->PlayerController) : nullptr;
    APawn* Pawn = PlayerController ? static_cast<APawn*>(PlayerController->Pawn) : nullptr;
    const bool bSamePawn = Pawn && BoundPawn.Get() == Pawn;
    BoundPawn = Pawn;

    BindLocalPawnOG(this);
    if (!bSamePawn)
    {
        HandleUIGameplayCue(FName(L"GameplayCue.Athena.Equipping"), GameplayCueRemoved);
        UE_LOG(LogFort, Log, TEXT("BindLocalPawn: the HUD follows %s, the previous pawn's item use timer ends"), *GetNameSafe(Pawn));
    }
}

void UAthenaHUDContext::BindLocalPawnHK(UAthenaHUDContext* This)
{
    This->BindLocalPawn();
}

void UAthenaHUDContext::Hook()
{
    if (!Finder::ResolveAthenaHUDPawnBinding())
    {
        return;
    }
    Hooks::Detour(L"UAthenaHUDContext::BindLocalPawn", reinterpret_cast<void*>(Natives::UAthenaHUDContext_BindLocalPawn), &BindLocalPawnHK, &BindLocalPawnOG);
}
