#pragma once

#include "Reflection.h"
#include "UObject/Object.h"

class UMaterialInterface : public UObject
{
    DefineUnrealClass(UMaterialInterface, Engine)
};

class UMaterialInstance : public UMaterialInterface
{
    DefineUnrealClass(UMaterialInstance, Engine)

public:
    DefineUProperty(UMaterialInterface*, Parent)
};

class UMaterialInstanceDynamic : public UMaterialInstance
{
    DefineUnrealClass(UMaterialInstanceDynamic, Engine)
};
