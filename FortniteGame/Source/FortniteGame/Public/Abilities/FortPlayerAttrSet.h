#pragma once

#include "AttributeSet.h"

#include <vector>

struct FFortGameplayAttributeData : public FGameplayAttributeData
{
    DefineUnrealStruct(FFortGameplayAttributeData, FortniteGame)
};

class UFortPlayerAttrSet : public UAttributeSet
{
    DefineUnrealClass(UFortPlayerAttrSet, FortniteGame)

public:
    DefineUStructProperty(FFortGameplayAttributeData, HarvestingTool)
};

class UFortHealthSet : public UAttributeSet
{
    DefineUnrealClass(UFortHealthSet, FortniteGame)

public:
    DefineUStructProperty(FFortGameplayAttributeData, Health)
    DefineUStructProperty(FFortGameplayAttributeData, MaxHealth)
};

// A pawn's walk, run and sprint speeds (replicated). AFortPawn::GetMovementSpeed reads them when the pawn has the set
// (1.7.2 0x786CB0, 1.8 0x7DD410, 1.8.1 0x7AC910); Battle Royale's pawn reads its PawnStatHandle row instead up to
// 1.8.x, and from 1.9 its movement set when it has one, crouched speeds included (1.9 0x4219D0).
class UFortMovementSet : public UAttributeSet
{
    DefineUnrealClass(UFortMovementSet, FortniteGame)

public:
    DefineUStructProperty(FFortGameplayAttributeData, WalkSpeed)
    DefineUStructProperty(FFortGameplayAttributeData, RunSpeed)
    DefineUStructProperty(FFortGameplayAttributeData, SprintSpeed)

    // Every speed but RunSpeed: walking, sprinting, and the crouched run and sprint where the build has them.
    std::vector<FFortGameplayAttributeData*> SpeedsBesidesRun();

private:
    DefineUStructProperty(FFortGameplayAttributeData, CrouchedRunSpeed)
    DefineUStructProperty(FFortGameplayAttributeData, CrouchedSprintSpeed)
};
