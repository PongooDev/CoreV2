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
    // 5.0+: Mission Control. The missions read it on the server (Retrieve the Data's Mission_DeadDrop for UseNewReadyUp):
    // the ready-up console and its votes instead of the Difficulty Pylon. Off on 5.0's server (CL-4204761), on from 5.10
    // (5.10's constructor, 0x12DF540, sets +0x3F4).
    DefineUProperty(bool, bEnableMissionActivationVote)
    // Toys: whether a toy's CreateMessage (B_Toy_Tomato_Base's hit, B_Toy_BeachBall_Base's keep-up) fills the message it
    // replicates for the clients' message feed. On by default (6.21's constructor, 0x141E760, sets +0x43E).
    DefineUProperty(bool, bToyMessagingEnabled)
    static bool HasMissionActivationVote() { return Member_bEnableMissionActivationVote.IsAvailable(); }
    static bool HasToyMessaging() { return Member_bToyMessagingEnabled.IsAvailable(); }
    static bool HasSTWLootDrops() { return Member_bEnableSTWLootDrops.IsAvailable(); }
    static bool HasSTWEnemyItemCacheDrops() { return Member_bEnableSTWEnemyItemCacheDrops.IsAvailable(); }

    static const UFortRuntimeOptions* Get()
    {
        UClass* Class = StaticClass();
        return Class ? static_cast<const UFortRuntimeOptions*>(Class->GetDefaultObject(false)) : nullptr;
    }
};
