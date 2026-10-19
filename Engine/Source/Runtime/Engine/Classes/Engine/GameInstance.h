#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

struct FWorldContext;

class UGameInstance : public UObject
{
    DefineUnrealClass(UGameInstance, Engine)

public:
    DefineNativeMember(FWorldContext*, WorldContext, UGameInstance_WorldContext)

    bool IsDedicatedServerInstance() const;

    static void Hook();

private:
    static bool IsDedicatedServerInstanceHK(const UGameInstance* This);
};
