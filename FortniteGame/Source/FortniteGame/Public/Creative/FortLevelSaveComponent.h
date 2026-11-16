#pragma once

#include "Components/ActorComponent.h"
#include "Containers/UnrealString.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Internationalization/Text.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

#include <cstdint>
#include <string>

class AActor;
class AFortVolume;
class UFortCreativeRealEstatePlotItem;

// The result the island's async steps report (7.00's FAsyncTaskResult): bSucceeded, and an ErrorCode and ErrorMessage.
struct FAsyncTaskResult
{
    DefineUnrealStruct(FAsyncTaskResult, FortniteGame)

public:
    DefineUProperty(bool, bSucceeded)
    DefineUProperty(FString, ErrorCode)
};

// A Creative island's template (from 7.00; Temperate_Medium: "Island {islandIndex}", its BasePlayset and size).
class UFortCreativeRealEstatePlotItemDefinition : public UObject
{
    DefineUnrealClass(UFortCreativeRealEstatePlotItemDefinition, FortniteGame)

public:
    DefineUProperty(FText, UIDisplayName)
};

// A player's Creative island (from 7.00; the creative profile's CreativePlot items, Temperate_Medium: the island's
// template, its BasePlayset and size). The item's id names the island's file in the DSS. The profile makes its items
// with itself as their outer (UFortMcpProfile's item factory, slot 0x4F 0x113AD80 -> NewObject(this, Class), 0x94C670).
class UFortCreativeRealEstatePlotItem : public UObject
{
    DefineUnrealClass(UFortCreativeRealEstatePlotItem, FortniteGame)

public:
    DefineUProperty(int32_t, IslandIndex)
    DefineUProperty(FString, IslandName)
    // UFortAccountItem's: the item's template.
    DefineUProperty(UObject*, ItemDefinition)
    // The item's id, through UMcpItem's virtual getter (the one ServerLoadPlotForPortal_Implementation names the plot by).
    FString GetItemId() const;
    // The island's name as the player sees it: its own (islandName, once named), else its template's UIDisplayName with
    // its number ("Island 2").
    FText GetIslandDisplayName() const;
};

// The record of what was built on an island (from 6.20's playspaces): the actors the playset came with and those placed,
// moved or deleted since, which it writes to a buffer (WriteToBuffer, 7.00 0x10941F0) and reads back (ReadFromBuffer).
class ULevelSaveRecord : public UObject
{
    DefineUnrealClass(ULevelSaveRecord, FortniteGame)

public:
    // The record from a saved file's bytes (Natives::ULevelSaveRecord_ReadFromBuffer). False without the native.
    bool ReadFromBuffer(const std::string& Bytes);
};

// A volume's record of its actors (from 6.20): it keeps the record, saves it on a timer (TimeBetweenSaves) and applies
// it to the world.
class ULevelSaveBaseComponent : public UActorComponent
{
    DefineUnrealClass(ULevelSaveBaseComponent, FortniteGame)
    DefineUFunction(ApplySaveToWorld)
    DefineUFunction(ConstructNewLevelRecord)
    DefineUFunction(SaveOutLevelRecord)
    DefineUFunction(StartPeriodicSaveTimer)
    DefineUFunction(StopPeriodicSaveTimer)

public:
    DefineUProperty(ULevelSaveRecord*, LevelRecord)
    DefineUProperty(TArray<AActor*>, ActorToAddAfterWrite)

    void ApplySaveToWorld();
    void ConstructNewLevelRecord(AActor* Owner);
    void SaveOutLevelRecord();
    void StartPeriodicSaveTimer();
    void StopPeriodicSaveTimer();
    // ApplySaveToWorld applies a record once (bit 0 of the native byte after ActorToAddAfterWrite, 7.00 +0x138: "Ignoring
    // ApplySaveToWorld" after): a reload applies the island's record again.
    void ClearAppliedSave();
};

class ULevelSaveComponent : public ULevelSaveBaseComponent
{
    DefineUnrealClass(ULevelSaveComponent, FortniteGame)
};

// The island's record in the cloud (from 7.00): an AFortVolume's, which loads the island of the user its rift took
// (LoadedPlot, AccountIdOfOwner), saves it to the DSS while they build and checks it in when it unloads.
class UFortLevelSaveComponent : public ULevelSaveComponent
{
    DefineUnrealClass(UFortLevelSaveComponent, FortniteGame)
    DefineUFunction(CloudLoadFinished)
    DefineUFunction(OnAsyncLoadingOfPlaysetComplete)

public:
    DefineUProperty(FMulticastScriptDelegate, OnStartPlotLoadForUser)
    DefineUProperty(UObject*, RestrictedPlotDefinition)
    DefineUStructProperty(FUniqueNetIdRepl, AccountIdOfOwner)
    DefineUProperty(UFortCreativeRealEstatePlotItem*, LoadedPlot)
    DefineUProperty(FString, LoadedPlotInstanceId)
    DefineUProperty(bool, bIsLoaded)
    DefineUProperty(bool, bIsSaving)

    static bool IsAvailable() { return StaticClass() && Member_LoadedPlot.IsAvailable() && Member_AccountIdOfOwner.IsAvailable(); }

    // Restored (7.00 strips it with the server's half of the rifts; its log lines are left in the build, unreferenced):
    // loads UserId's island PlotItem into the volume. The island's own level (the plot's BasePlayset, through the
    // volume's playset stream component) streams in, the island's file is checked out of the DSS with its bytes, the
    // record is read and applied, and CloudLoadFinished reports it (the volume is Ready, the rift opens). The island
    // is then saved on the component's timer by the game's own save (SaveOutLevelRecord, 0xF5B8F0).
    void LoadPlotForUser(const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem);
    // Core's: the island's last save and checkin, then the volume back to its template for the next player (the
    // component's own EndPlay does the first half, 0xF41600, but only as the volume goes).
    void UnloadPlotForUser();
    // ServerReloadCreativePlot's: the island as it was last saved.
    void ReloadPlot();
    // The island's file, as the game names it (0xF476E0): islands/personal/<owner's account id>/<plot item id>.island.
    FString GetCloudSaveFileName() const;

    AFortVolume* GetVolume() const;
    // The volume's own component (one per island space).
    static UFortLevelSaveComponent* GetForVolume(AActor* Volume);
    // Whether UserId owns the island loaded here (AccountIdOfOwner).
    bool IsOwnedBy(const FUniqueNetIdRepl& UserId) const;

private:
    void FinishPlotLoad(bool bSucceeded, const std::wstring& ErrorCode);
};
