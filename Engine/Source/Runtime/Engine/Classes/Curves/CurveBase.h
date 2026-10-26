#pragma once

#include "UObject/Object.h"

class UCurveBase : public UObject
{
    DefineUnrealClass(UCurveBase, Engine)
    DefineUFunction(GetTimeRange)

public:
    void GetTimeRange(float& MinTime, float& MaxTime) const;
};

class UCurveVector : public UCurveBase
{
    DefineUnrealClass(UCurveVector, Engine)
};
