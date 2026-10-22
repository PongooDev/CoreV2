#pragma once

#include "Containers/UnrealString.h"
#include "Items/FortQuickBars.h"
#include "UObject/Object.h"

class AFortPlayerController;

struct FFortWorldPlayerLoadout
{
    DefineUnrealStruct(FFortWorldPlayerLoadout, FortniteGame)
    DefineBitfieldUProperty(bPlayerIsNew)
    DefineUStructProperty(FQuickBar, PrimaryQuickBarRecord)
    DefineUStructProperty(FQuickBar, SecondaryQuickBarRecord)
};

class UFortMcpProfileWorld : public UObject
{
    DefineUnrealClass(UFortMcpProfileWorld, FortniteGame)
    DefineUFunction(InitializeTheater)

public:
    DefineUProperty(FString, ProfileId)
    DefineUStructProperty(FFortWorldPlayerLoadout, CachedPlayerLoadout)

    void* BindToPlayerController(AFortPlayerController* PlayerController, bool bUseOutpostInventory);
    void InitializeTheater(const FString& TheaterGuid);

    static void HookVTables();

private:
    static inline void* (*BindToPlayerControllerOG)(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory) = nullptr;
    static void* BindToPlayerControllerHK(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory);
};
