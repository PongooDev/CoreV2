#pragma once

#include "Engine/NetSerialization.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"
#include "StructArray.h"

// A gameplay cue added to an ability system (UAbilitySystemComponent::AddGameplayCue_Internal) and held there until it
// is removed: its tag, prediction key and parameters. Each client gets it in the ability system's replicated
// ActiveGameplayCues and runs the cue's WhileActive event as it arrives (FActiveGameplayCue::PostReplicatedAdd) and its
// Removed event as it goes (PreReplicatedRemove), on the ability system's avatar.
struct FActiveGameplayCue : public FFastArraySerializerItem
{
    DefineUnrealStruct(FActiveGameplayCue, GameplayAbilities)
    DefineUProperty(FGameplayTag, GameplayCueTag)
};

struct FActiveGameplayCueContainer : public FFastArraySerializer
{
    DefineUnrealStruct(FActiveGameplayCueContainer, GameplayAbilities)
    DefineUStructArrayProperty(FActiveGameplayCue, GameplayCues)

public:
    // FActiveGameplayCueContainer::AddCue less its Owner->UpdateTagMap (the server's own count of the tag, which the
    // clients keep from the replicated cue): the new cue, dirty for replication. Its parameters are the caller's to fill.
    FActiveGameplayCue& AddCue(const FGameplayTag& Tag)
    {
        FActiveGameplayCue& NewCue = GameplayCues.AddDefaulted();
        NewCue.GameplayCueTag = Tag;
        MarkItemDirty(NewCue);
        return NewCue;
    }

    // FActiveGameplayCueContainer::RemoveCue less its UpdateTagMap: the first cue with the tag goes.
    bool RemoveCue(const FGameplayTag& Tag)
    {
        TStructArray<FActiveGameplayCue>& Cues = GameplayCues;
        for (int32_t Index = 0; Index < Cues.Num(); ++Index)
        {
            if (Cues[Index].GameplayCueTag == Tag)
            {
                Cues.RemoveAt(Index);
                MarkArrayDirty();
                return true;
            }
        }
        return false;
    }
};
