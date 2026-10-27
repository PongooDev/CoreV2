#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

struct FWorldContext;
class ULocalPlayer;

class UGameInstance : public UObject
{
    DefineUnrealClass(UGameInstance, Engine)

public:
    DefineNativeMember(FWorldContext*, WorldContext, UGameInstance_WorldContext)
    DefineUProperty(TArray<ULocalPlayer*>, LocalPlayers)

    bool IsDedicatedServerInstance() const;

    static void Hook();

private:
    static bool IsDedicatedServerInstanceHK(const UGameInstance* This);
};
