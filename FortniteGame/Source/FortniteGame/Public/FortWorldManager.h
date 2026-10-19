#pragma once

#include "GameFramework/Info.h"

class UFortWorldRecord;

enum class EFortWorldManagerState : uint8_t
{
    WMS_Created,
    WMS_QueryingWorld,
    WMS_WorldQueryComplete,
    WMS_CreatingNewWorld,
    WMS_LoadingExistingWorld,
    WMS_Running,
    WMS_Failed,
    WMS_MAX,
};

class AFortWorldManager : public AInfo
{
    DefineUnrealClass(AFortWorldManager, FortniteGame)

public:
    DefineUProperty(EFortWorldManagerState, WorldManagerState)
    DefineUProperty(UFortWorldRecord*, CurrentWorldRecord)

    void OnWorldRecordQueryComplete();

    static void Hook();

private:
    static inline void (*OnWorldRecordQueryCompleteOG)(AFortWorldManager* This) = nullptr;
    static void OnWorldRecordQueryCompleteHK(AFortWorldManager* This);
};
