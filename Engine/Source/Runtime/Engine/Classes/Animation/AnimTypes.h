#pragma once

#include "Animation/AnimLinkableElement.h"

class UAnimNotify;

// One of an animation's notifies, where it fires (Engine's FAnimNotifyEvent).
struct FAnimNotifyEvent : public FAnimLinkableElement
{
    DefineUnrealStruct(FAnimNotifyEvent, Engine)
    DefineUProperty(float, TriggerTimeOffset)
    DefineUProperty(UAnimNotify*, Notify)

    // When the notify fires, in its animation (Epic's GetTriggerTime, AnimTypes.cpp).
    float GetTriggerTime() const { return GetTime() + TriggerTimeOffset; }
};
