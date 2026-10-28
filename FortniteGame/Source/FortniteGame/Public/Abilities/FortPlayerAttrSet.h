#pragma once

#include "AttributeSet.h"

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
};
