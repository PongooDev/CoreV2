#include "pch.h"
#include "Curves/CurveBase.h"
#include "ReflectionParams.h"

void UCurveBase::GetTimeRange(float& MinTime, float& MaxTime) const
{
    Reflection::FParams Params(StaticFunction_GetTimeRange());
    MinTime = 0.0f;
    MaxTime = 0.0f;
    if (Params.Call(const_cast<UCurveBase*>(this)))
    {
        MinTime = Params.Take<float>(L"MinTime");
        MaxTime = Params.Take<float>(L"MaxTime");
    }
}
