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
};
