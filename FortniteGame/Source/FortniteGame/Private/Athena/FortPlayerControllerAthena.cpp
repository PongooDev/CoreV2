#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Finder.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Natives.h"
#include "ObjectLayout.h"

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
