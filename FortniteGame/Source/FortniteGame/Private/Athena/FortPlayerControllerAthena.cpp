#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Abilities/FortHomebaseSet.h"
#include "Finder.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

bool AFortPlayerControllerAthena::IsDefaultHero(UFortHero* Hero)
{
    UFortHeroType* HeroType = Hero ? Hero->GetHeroTypeBP() : nullptr;
    for (UFortHeroType* DefaultHero : DefaultHeroes)
    {
        if (DefaultHero && DefaultHero == HeroType)
        {
            return true;
        }
    }
    return false;
}

UFortHero* AFortPlayerControllerAthena::GetOrInitializeHero()
{
    // The override (1.7.2 0x42E5E0) keeps the controller's hero, or makes a hero of the next of its DefaultHeroes (a
    // counter over every controller, divided by their number) and sets it through TrySetHeroImplementation.
    if (DefaultHeroes.Num() <= 0 || !Finder::ResolveAthenaHeroLive())
    {
        return nullptr;
    }
    return CallVirtual<UFortHero*>(this, Natives::AFortPlayerController_GetOrInitializeHeroIndex);
}

UFortHero* AFortPlayerControllerAthena::SetupHero()
{
    // Battle Royale played a level 1 Save the World soldier's stats (#73: Soldier_Balanced_UC_T1 in AttributesHeroScaling,
    // 440 health and 172 shield) instead of its hero's (GE_Map_Athena_Vitality_Health: 100 health and a 100 shield cap,
    // GE_Map_Athena_StartingShields: no shield). GetOrInitializeHero binds the player's UFortRegisteredPlayerInfo to the
    // player state before it makes the hero, and binding initializes the attributes (0x5EA3F0) with the controller's hero
    // or, while there is none, with UFortGameData::DefaultHero (HID_Commando_GrenadeGun_UC_T01). Setting the hero
    // afterwards doesn't initialize them again; ResetGameplayAttributes does, with AFortPlayerController::GetHero (0x808A80),
    // which only returns the hero whose id the player state holds. So with the pawn possessed the hero is set once more,
    // which puts its id back on the player state, before Possess resets the attributes with it.
    UFortHero* Hero = GetOrInitializeHero();
    if (Hero)
    {
        TrySetHeroImplementation(Hero, false, false);
    }
    return Hero;
}

void AFortPlayerControllerAthena::SetupBackpackSize()
{
    // The HUD's backpack showed "0 | 0" instead of "0 | 5" (#77). AthenaPickupManager's RefreshCounts shows
    // UFortInventoryContext::GetBackpackItemCounts (0x2DB19B0): the world inventory's limited items against its size
    // (0x63CA40), which is OverriddenBackpackSize when set, else the player info's ability system actor's (the player
    // state's) FortHomebaseSet.WorldInventorySizeBonus. Battle Royale's +5 to that is GE_Map_Athena_Inventory_StartingSize
    // in its heroes' CombinedStatGEs, which reach the player state only when Possess resets the attributes (#73, before
    // that never), after the starting items, and the HUD reads the size only when it's built and when the inventory
    // changes. The size goes on the controller instead, read from the same effects, before the starting items. It
    // replicates while the controller has a cheat manager (0x81FBA0), which every player gets when they join.
    UClass* HomebaseSetClass = UFortHomebaseSet::StaticClass();
    const UProperty* WorldInventorySizeBonus = HomebaseSetClass ? HomebaseSetClass->FindPropertyByName(FName(L"WorldInventorySizeBonus", FNAME_Find)) : nullptr;
    for (UFortHeroType* HeroType : DefaultHeroes)
    {
        const int32_t BackpackSize = HeroType ? static_cast<int32_t>(HeroType->GetCombinedStatBonus(WorldInventorySizeBonus)) : 0;
        if (BackpackSize > 0)
        {
            OverriddenBackpackSize = BackpackSize;
            UE_LOG(LogFort, Log, TEXT("SetupBackpackSize: %s has a backpack of %d (%s)"), *GetName(), BackpackSize, *HeroType->GetName());
            return;
        }
    }
    UE_LOG(LogFort, Warning, TEXT("SetupBackpackSize: %s's heroes give no backpack size, it stays the attribute's"), *GetName());
}
