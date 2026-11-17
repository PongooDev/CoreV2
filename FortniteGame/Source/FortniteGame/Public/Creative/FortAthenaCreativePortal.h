#pragma once

#include "Building/BuildingActor.h"
#include "GameFramework/Info.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Reflection.h"
#include "UObject/Stack.h"

#include <cstdint>

class AFortPlayerControllerAthena;
class AFortPlayerPawn;
class AFortPlayerState;
class AFortVolume;
class UFortCreativeRealEstatePlotItem;
class UFortLevelSaveComponent;

// A rift of Creative's hub (from 7.00; BGA_IslandPortal): the hub has one for each player of the match, which takes
// them to the island its LinkedVolume holds, the island its owner picked ("Set Rift Destination", which calls
// ServerLoadPlotForPortal). An island's own rift back to the hub has bReturnToCreativeHub.
class AFortAthenaCreativePortal : public ABuildingActor
{
    DefineUnrealClass(AFortAthenaCreativePortal, FortniteGame)
    DefineUFunction(TeleportPlayer)
    DefineUFunction(TeleportPlayerToLinkedVolume)
    DefineUFunction(NotifyTeleportedPlayerPawn)
    DefineUFunction(OnRep_OwningPlayerState)
    DefineUFunction(OnRep_OwningPlayer)
    DefineUFunction(OnRep_PortalOpen)

public:
    DefineUProperty(int32_t, PortalIndex)
    DefineUProperty(AActor*, DestinationActor)
    DefineUProperty(bool, bReturnToCreativeHub)
    DefineUProperty(AFortVolume*, LinkedVolume)
    DefineUProperty(AFortPlayerState*, OwningPlayerState)
    // 7.10 (CL-4667333) keeps the owner's id instead (replicated, OnRep_OwningPlayer), with the player state only as a
    // private cache the rift fills itself (CachedOwningPlayerState, GetPlayerState).
    DefineUStructProperty(FUniqueNetIdRepl, OwningPlayer)
    DefineUProperty(bool, bPortalOpen)
    DefineUProperty(FVector, TeleportLocation)
    // The players on the rift's island (the controllers whose CreativePlotLinkedVolume is LinkedVolume), which the
    // rift shows as its "x/y" (OnRep_PopulationChanged).
    DefineUProperty(uint8_t, CurrentPopulation)
    static int32_t CurrentPopulationOffset() { return Member_CurrentPopulation.IsAvailable() ? static_cast<int32_t>(Member_CurrentPopulation.GetOffsetChecked()) : -1; }

    // Restored natives (7.00 strips both: their execs call the shared empty function). The rift's Blueprint
    // (BGA_IslandPortal) sends a pawn that steps into an open rift through TeleportPlayerToLinkedVolume when it has an
    // island, else through TeleportPlayer to its destination.
    void TeleportPlayer(AFortPlayerPawn* PlayerPawn, const FRotator& TeleportRotation);
    void TeleportPlayerToLinkedVolume(AFortPlayerPawn* PlayerPawn, bool bUseSpawnTags);

    // Restored (the call ServerLoadPlotForPortal_Implementation, 7.00 0xC65E80, makes once it has checked the rift is
    // the player's and found the plot item in their creative profile; 7.00 calls the shared empty function there): the
    // rift becomes UserId's and its island loads PlotItem.
    void LoadPlotForUser(const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem);

    // Restored (7.10's ServerLoadPlotForPortalFromMnemonic_Implementation, 0xC84E10, calls the shared empty function
    // there, once it has checked the rift and has the player's id): the island of the code Mnemonic, a published island,
    // loads into the rift's island space for UserId (UFortLevelSaveComponent::LoadPlotFromMnemonic).
    void LoadPlotFromMnemonicForUser(const FUniqueNetIdRepl& UserId, const FString& Mnemonic);
    static void LoadPlotFromMnemonicForUserHK(AFortAthenaCreativePortal* This, const FUniqueNetIdRepl& UserId, const FString& Mnemonic);

    UFortLevelSaveComponent* GetLevelSaveComponent() const;
    // The game's rift refresh (Natives::AFortAthenaCreativePortal_RefreshPortal): its island's name, open state and
    // CurrentPopulation counted again.
    void RefreshPortal();
    void SetOwningPlayerState(AFortPlayerState* NewOwner);
    // Whether the rift has an owner, and whether it is PlayerState's, by the player state (7.00) or its id (7.10).
    bool HasOwner() const;
    bool IsOwnedBy(const AFortPlayerState* PlayerState) const;

    DECLARE_FUNCTION(execTeleportPlayer);
    DECLARE_FUNCTION(execTeleportPlayerToLinkedVolume);

    // From 7.00: the two natives, where the build's execs call the stub, and the redirect of ServerLoadPlotForPortal's
    // stripped call. Nothing on older builds (no such class).
    static void HookNatives();
    // Where Core's redirect of ServerLoadPlotForPortal's stripped call lands (the rift in rcx, as the call passes it).
    static void LoadPlotForUserHK(AFortAthenaCreativePortal* This, const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem);

private:
    void NotifyTeleportedPlayerPawn(AFortPlayerPawn* PlayerPawn);
};
