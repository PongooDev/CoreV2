#pragma once

#include "UObject/NameTypes.h"
#include "UObject/Object.h"

class UCurveTable : public UObject
{
    DefineUnrealClass(UCurveTable, Engine)
};

struct FCurveTableRowHandle
{
    DefineUnrealStruct(FCurveTableRowHandle, Engine)
    DefineUProperty(UCurveTable*, CurveTable)
    DefineUProperty(FName, RowName)

    // UE 4.22's bool form (7.20 CL-4727874 0x27C3800): the row's curve at XValue into YValue. False, and YValue left as
    // it is, when the handle has no table or the table has no such row.
    bool Eval(float XValue, float* YValue, const wchar_t* ContextString) const;
};
