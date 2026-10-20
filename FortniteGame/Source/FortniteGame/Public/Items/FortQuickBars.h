#pragma once

#include "Containers/ScriptArray.h"
#include "GameFramework/Actor.h"

enum class EFortQuickBars : uint8_t
{
    Primary,
    Secondary,
    Max_None,
};

struct FQuickBarSlot
{
    DefineUnrealStruct(FQuickBarSlot, FortniteGame)
};

struct FQuickBarData
{
    DefineUnrealStruct(FQuickBarData, FortniteGame)
    DefineUProperty(FScriptArray, QuickbarSlots)
};

struct FQuickBar
{
    DefineUnrealStruct(FQuickBar, FortniteGame)
    DefineUProperty(FScriptArray, Slots)
    DefineUStructProperty(FQuickBarData, DataDefinition)
};

class AFortQuickBars : public AActor
{
    DefineUnrealClass(AFortQuickBars, FortniteGame)

public:
    DefineUStructProperty(FQuickBar, PrimaryQuickBar)
    DefineUStructProperty(FQuickBar, SecondaryQuickBar)
    DefineUFunction(EnableSlot)

    FQuickBar& GetQuickBar(EFortQuickBars InQuickBar) { return InQuickBar == EFortQuickBars::Primary ? PrimaryQuickBar : SecondaryQuickBar; }

    void EnableSlot(EFortQuickBars InQuickBar, int32_t SlotIndex);
    void SetupQuickbar(EFortQuickBars InQuickBar, const FQuickBarData& QuickBarData);
};
