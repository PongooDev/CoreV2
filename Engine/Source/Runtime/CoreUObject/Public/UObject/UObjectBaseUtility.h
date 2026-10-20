#pragma once

#include "UObject/UObjectBase.h"

class UObjectBaseUtility : public UObjectBase
{
public:
    void SetFlags(EObjectFlags NewFlags) { SetFlagsTo(GetFlags() | NewFlags); }
    void ClearFlags(EObjectFlags FlagsToClear) { SetFlagsTo(GetFlags() & ~FlagsToClear); }
    bool HasAnyFlags(EObjectFlags FlagsToCheck) const { return (GetFlags() & FlagsToCheck) != 0; }
    bool HasAllFlags(EObjectFlags FlagsToCheck) const { return (GetFlags() & FlagsToCheck) == FlagsToCheck; }
    bool IsTemplate(EObjectFlags TemplateTypes = RF_ArchetypeObject | RF_ClassDefaultObject) const;

    void MarkPendingKill();
    bool IsPendingKill() const;
    bool IsUnreachable() const;
    void AddToRoot();
    void RemoveFromRoot();
    bool IsRooted() const;

    FString GetName() const { return GetFName().ToString(); }
    FString GetPathName(const UObject* StopOuter = nullptr) const;
    FString GetFullName(const UObject* StopOuter = nullptr) const;
    UObject* GetOutermost() const;

    bool IsA(const UClass* SomeBase) const;

    template <class T>
    bool IsA() const
    {
        return IsA(T::StaticClass());
    }
};

inline FString GetNameSafe(const UObjectBaseUtility* Object)
{
    return Object ? Object->GetName() : FString(TEXT("None"));
}
