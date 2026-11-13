#pragma once

#include "UObject/Object.h"

// EAnimLinkMethod: how an element's LinkValue places it in its montage (Engine's AnimLinkableElement.h).
namespace EAnimLinkMethod
{
    enum Type : uint8_t
    {
        // A time in the montage.
        Absolute,
        // A time from the start of the segment the element is linked to.
        Relative,
        // A fraction of that segment's length.
        Proportional,
    };
}

// A notify or a section of a montage, linked to one of its segments (Engine's FAnimLinkableElement).
struct FAnimLinkableElement
{
    DefineUnrealStruct(FAnimLinkableElement, Engine)
    DefineUProperty(uint8_t, LinkMethod)
    DefineUProperty(float, SegmentBeginTime)
    DefineUProperty(float, SegmentLength)
    DefineUProperty(float, LinkValue)

    // The element's time in its montage (Epic's GetTime(EAnimLinkMethod::Absolute), AnimLinkableElement.cpp).
    float GetTime() const
    {
        switch (LinkMethod)
        {
        case EAnimLinkMethod::Relative:
            return SegmentBeginTime + LinkValue;
        case EAnimLinkMethod::Proportional:
            return SegmentBeginTime + LinkValue * SegmentLength;
        default:
            return LinkValue;
        }
    }
};
