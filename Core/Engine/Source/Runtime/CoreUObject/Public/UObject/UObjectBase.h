#pragma once

#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

class UClass;
class UObject;

class UObjectBase
{
public:
    virtual ~UObjectBase() = default;

    UClass* GetClass() const { return ClassPrivate; }
    UObject* GetOuter() const { return OuterPrivate; }
    FName GetFName() const { return NamePrivate; }
    uint32_t GetUniqueID() const { return static_cast<uint32_t>(InternalIndex); }
    EObjectFlags GetFlags() const { return ObjectFlags; }

protected:
    void SetFlagsTo(EObjectFlags NewFlags) { ObjectFlags = NewFlags; }

private:
    EObjectFlags ObjectFlags;
    int32_t InternalIndex;
    UClass* ClassPrivate;
    FName NamePrivate;
    UObject* OuterPrivate;
};
static_assert(sizeof(UObjectBase) == 0x28);
