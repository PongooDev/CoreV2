#include "pch.h"
#include "Pawns/FortPlayerPawn.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"

void AFortPlayerPawn::ResetGameplayAttributes()
{
    if (Finder::ResolveResetGameplayAttributesLive())
    {
        CallVirtual<void>(this, Natives::AFortPlayerPawn_ResetGameplayAttributesIndex);
    }
}
