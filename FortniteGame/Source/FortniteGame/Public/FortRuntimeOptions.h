#pragma once

#include "UObject/Class.h"
#include "UObject/Object.h"

// The game's runtime switches (DefaultRuntimeOptions.ini, which hotfixes override). The game reads its class defaults
// (4.0 0x10C7C40 returns the class's ClassDefaultObject).
class UFortRuntimeOptions : public UObject
{
    DefineUnrealClass(UFortRuntimeOptions, FortniteGame)

public:
    // 4.0: Save the World's enemy loot drops. AFortAIDirector::GetBaseLootDropData (4.0 0x8C07E0) and
    // GetLootDropModifierData (0x8C5B60) return nothing without it, so no encounter gets loot drop rows.
    DefineUProperty(bool, bEnableSTWLootDrops)
    // 4.0: whether an enemy's account item drop (a Mini-Boss's item cache) drops. Nothing in the client exe reads it: the
    // server code that did is stripped.
    DefineUProperty(bool, bEnableSTWEnemyItemCacheDrops)
    static bool HasSTWLootDrops() { return Member_bEnableSTWLootDrops.IsAvailable(); }
    static bool HasSTWEnemyItemCacheDrops() { return Member_bEnableSTWEnemyItemCacheDrops.IsAvailable(); }

    static const UFortRuntimeOptions* Get()
    {
        UClass* Class = StaticClass();
        return Class ? static_cast<const UFortRuntimeOptions*>(Class->GetDefaultObject(false)) : nullptr;
    }
};
