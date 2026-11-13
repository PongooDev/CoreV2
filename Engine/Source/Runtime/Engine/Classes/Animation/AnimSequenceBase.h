#pragma once

#include "Animation/AnimTypes.h"
#include "StructArray.h"

// An animation with notifies and a length (Engine's UAnimSequenceBase, a montage's base).
class UAnimSequenceBase : public UObject
{
    DefineUnrealClass(UAnimSequenceBase, Engine)

public:
    DefineUStructArrayProperty(FAnimNotifyEvent, Notifies)
    DefineUProperty(float, SequenceLength)
};
