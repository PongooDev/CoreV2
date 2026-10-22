#pragma once

#include "Player/FortPlayerControllerZone.h"

class ADeployableBasePlot;

class AFortPlayerControllerDeployableBase : public AFortPlayerControllerZone
{
    DefineUnrealClass(AFortPlayerControllerDeployableBase, FortniteGame)
    DefineUFunction(ServerMoveItemToDeployableBase)
    DefineUFunction(ServerMoveItemFromDeployableBase)
    DefineUFunction(ServerMoveAllItemsToDeployableBase)
    DefineUFunction(ServerMoveAllItemsFromDeployableBase)

public:
    void ServerMoveItemToDeployableBase_Implementation(const FGuid& ItemOnPlayer, int32_t Count, const ADeployableBasePlot* ExpectedPlot);
    void ServerMoveItemFromDeployableBase_Implementation(const FGuid& ItemOnPlot, int32_t Count, const ADeployableBasePlot* ExpectedPlot);
    void ServerMoveAllItemsToDeployableBase_Implementation(const ADeployableBasePlot* ExpectedPlot);
    void ServerMoveAllItemsFromDeployableBase_Implementation(const ADeployableBasePlot* ExpectedPlot);

    static void HookVTables();

private:
    ADeployableBasePlot* GetPlotWithPermission(const ADeployableBasePlot* ExpectedPlot);
    bool MoveItemToPlot(ADeployableBasePlot* Plot, UFortWorldItem* WorldItem, int32_t Count);
    bool MoveItemFromPlot(ADeployableBasePlot* Plot, UFortWorldItem* PlotItem, int32_t Count);

    static void ServerMoveItemToDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const FGuid& ItemOnPlayer, int32_t Count, const ADeployableBasePlot* ExpectedPlot);
    static void ServerMoveItemFromDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const FGuid& ItemOnPlot, int32_t Count, const ADeployableBasePlot* ExpectedPlot);
    static void ServerMoveAllItemsToDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const ADeployableBasePlot* ExpectedPlot);
    static void ServerMoveAllItemsFromDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const ADeployableBasePlot* ExpectedPlot);
};
