#pragma once

#include "Animation/AnimSequenceBase.h"
#include "UObject/NameTypes.h"

// A named section of a montage, from its time on (Engine's FCompositeSection).
struct FCompositeSection : public FAnimLinkableElement
{
    DefineUnrealStruct(FCompositeSection, Engine)
    DefineUProperty(FName, SectionName)
};

// Engine's UAnimMontage (a UAnimCompositeBase).
class UAnimMontage : public UAnimSequenceBase
{
    DefineUnrealClass(UAnimMontage, Engine)

public:
    DefineUStructArrayProperty(FCompositeSection, CompositeSections)

    bool IsValidSectionIndex(int32_t SectionIndex) const { return SectionIndex >= 0 && SectionIndex < GetMember_CompositeSections().Num(); }

    // The section's start, and the next section's start (the montage's end for the last one): Epic's
    // GetSectionStartAndEndTime (AnimMontage.cpp).
    void GetSectionStartAndEndTime(int32_t SectionIndex, float& OutStartTime, float& OutEndTime) const
    {
        OutStartTime = 0.0f;
        OutEndTime = GetMember_SequenceLength();
        if (IsValidSectionIndex(SectionIndex))
        {
            OutStartTime = GetMember_CompositeSections()[SectionIndex].GetTime();
        }
        if (IsValidSectionIndex(SectionIndex + 1))
        {
            OutEndTime = GetMember_CompositeSections()[SectionIndex + 1].GetTime();
        }
    }
};
