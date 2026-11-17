#pragma once

#include "Components/ActorComponent.h"
#include "Containers/UnrealString.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Internationalization/Text.h"
#include "Reflection.h"
#include "Misc/DateTime.h"
#include "UObject/AssetPtr.h"
#include "UObject/ScriptDelegates.h"

#include <cstdint>
#include <string>

class AActor;
class AFortPlayerController;
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
    // The island's playset (7.10 passes it to UFortLevelSaveComponent::OnAsyncLoadingOfPlaysetComplete).
    DefineUProperty(TAssetPtr<UObject>, BasePlayset)
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
    // UFortAccountItem's: the item's template, and its id (the MCP item's instance id), which GetInstanceId returns.
    DefineUProperty(UObject*, ItemDefinition)
    DefineUProperty(FString, InstanceId)
    // The item's id, through UMcpItem's virtual getter (the one ServerLoadPlotForPortal_Implementation names the plot by).
    FString GetItemId() const;
    // The island's name as the player sees it: its own (islandName, once named), else its template's UIDisplayName with
    // its number ("Island 2").
    FText GetIslandDisplayName() const;
    // The plot's playset, from its definition (null without one).
    const TAssetPtr<UObject>* GetBasePlayset() const;
};

// The record of what was built on an island (from 6.20's playspaces): the actors the playset came with and those placed,
// moved or deleted since, which it writes to a buffer (WriteToBuffer, 7.00 0x10941F0) and reads back (ReadFromBuffer).
class ULevelSaveRecord : public UObject
{
    DefineUnrealClass(ULevelSaveRecord, FortniteGame)

public:
    // The record from a saved file's bytes (Natives::ULevelSaveRecord_ReadFromBuffer). False without the native or
    // when the bytes don't read.
    bool ReadFromBuffer(const std::string& Bytes);
    // The end of a read, the game's own (Natives::ULevelSaveRecord_NotifyFinishedRead): the records tidied, and
    // OnFinishedRead broadcast, which the component that made the record answers (ULevelSaveBaseComponent::
    // OnFinishedRecordRead: bValidRecord, and on 7.00 the record applied when bApplySaveAfterLoad).
    void NotifyFinishedRead(bool bSucceeded);
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
    // 7.00: the record is applied as soon as it is read (OnFinishedRecordRead). 7.10 (CL-4667333) drops it, and its
    // OnFinishedRecordRead applies nothing.
    DefineBitfieldUProperty(bApplySaveAfterLoad)
    DefineUProperty(ULevelSaveRecord*, LevelRecord)

    void ApplySaveToWorld();
    void ConstructNewLevelRecord(AActor* Owner);
    void SaveOutLevelRecord();
    void StartPeriodicSaveTimer();
    void StopPeriodicSaveTimer();
    // Whether the record the component reads is applied by the read's own end (7.00 with bApplySaveAfterLoad).
    bool AppliesSaveAfterLoad() const { return Member_bApplySaveAfterLoad.IsAvailable() && bApplySaveAfterLoad; }
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
    DefineUProperty(FMulticastScriptDelegate, OnFinishedLoadingCloudFiles)
    DefineUProperty(UObject*, RestrictedPlotDefinition)
    DefineUProperty(bool, bLoadPlaysetFromPlot)
    DefineUStructProperty(FUniqueNetIdRepl, AccountIdOfOwner)
    DefineUProperty(UFortCreativeRealEstatePlotItem*, LoadedPlot)
    DefineUProperty(FString, LoadedPlotInstanceId)
    DefineUProperty(bool, bIsLoaded)
    DefineUProperty(bool, bIsSaving)
    // 7.10's (CL-4667333; PongooDev/CoreV2#488): the save in progress (bIsSaving's new name: the cloud save sets it,
    // 0xF887F3), the island menu's backup and restore states (EBackupSaveState Ready 0, InProgress 1, OnCooldown 2) and
    // the checkpoint's date, which replicate to the island's players, and the save's end.
    DefineUProperty(bool, bIsAutoSaving)
    DefineUProperty(uint8_t, BackupSaveState)
    DefineUProperty(uint8_t, RestoringState)
    DefineUProperty(FDateTime, LastCheckpointDateUtc)
    DefineUProperty(int32_t, PublishRateLimitSeconds)
    DefineUProperty(int32_t, BackupRateLimitSeconds)
    DefineUProperty(int32_t, RestoreRateLimitSeconds)
    DefineUProperty(FMulticastScriptDelegate, OnFinishedWritingCloudFiles)
    static bool HasBackupStates() { return Member_BackupSaveState.IsAvailable() && Member_RestoringState.IsAvailable(); }
    static bool HasLastCheckpointDate() { return Member_LastCheckpointDateUtc.IsAvailable(); }
    static bool HasOnFinishedWritingCloudFiles() { return Member_OnFinishedWritingCloudFiles.IsAvailable(); }
    bool IsAutoSaving() const { return Member_bIsAutoSaving.IsAvailable() && bIsAutoSaving; }
    // The island tools' rate limits (config: 0 publish, 1 backup, 2 restore), or Fallback where the build has none.
    int32_t GetToolRateLimitSeconds(int32_t Tool, int32_t Fallback) const
    {
        if (Tool == 0 && Member_PublishRateLimitSeconds.IsAvailable()) return PublishRateLimitSeconds;
        if (Tool == 1 && Member_BackupRateLimitSeconds.IsAvailable()) return BackupRateLimitSeconds;
        if (Tool == 2 && Member_RestoreRateLimitSeconds.IsAvailable()) return RestoreRateLimitSeconds;
        return Fallback;
    }

    static bool IsAvailable() { return StaticClass() && Member_LoadedPlot.IsAvailable() && Member_AccountIdOfOwner.IsAvailable(); }
    // Whether the island's playset comes with its plot (bLoadPlaysetFromPlot: the stream component streams the plot's).
    bool LoadsPlaysetFromPlot() const { return Member_bLoadPlaysetFromPlot.IsAvailable() && bLoadPlaysetFromPlot; }

    // Restored (7.00 strips it with the server's half of the rifts; its log lines are left in the build, unreferenced):
    // loads UserId's island PlotItem into the volume, as steps on the volume's task queue, the way the game's own
    // restricted plot load (BeginRestrictedPlotLoad, 0xF363B0) queues a featured island: the volume emptied, the plot's
    // playset streamed in (LoadPlaysetAsync, then the stream component's LoadPlaysetAsyncNoCallback), then the island's
    // file from the DSS (LoadFromDssAsync). Its end, CloudLoadFinished, finds the queue empty and finishes the load
    // (bIsLoaded, the volume Ready, the rift told). Steps a reset queued before (a player leaving) run first.
    void LoadPlotForUser(const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem);
    // Core's: the island's last save and checkin, then the volume back to its template for the next player (the
    // component's own EndPlay does the first half, 0xF41600, but only as the volume goes).
    void UnloadPlotForUser();
    // ServerReloadCreativePlot's: the island as it was last saved.
    void ReloadPlot();

    // The plot's playset loaded (the game's own step, Natives::UFortLevelSaveComponent_LoadPlaysetAsync): returns
    // OnFinishedPlaysetLoad, which OnAsyncLoadingOfPlaysetComplete broadcasts once the playset is the volume's.
    FMulticastScriptDelegate* LoadPlaysetAsync();
    // 7.10+'s (TSoftObjectPtr<UFortPlaysetItemDefinition> by value): the playset to load, the plot's, which the queue's
    // delegate carries as its payload the way the game's own BeginRestrictedPlotLoad binds the step (7.20 0x1068AF0).
    FMulticastScriptDelegate* LoadPlaysetAsync(FSoftObjectPtr PlaysetToLoad);
    // Restored step (21.40's name): the island's file checked out of the DSS with its bytes; its answer goes to
    // FinalizeDownload, or LoadFromDefault when the island has no file yet. Returns OnFinishedLoadingCloudFiles, which
    // CloudLoadFinished broadcasts.
    FMulticastScriptDelegate* LoadFromDssAsync();
    // Restored (21.40's name): the downloaded island read into a new record ("Successfully downloaded %s - %s.",
    // "Loading level record of size %i with crc %i"), applied, and the load finished.
    void FinalizeDownload(const std::string& Contents);
    // Restored (21.40's name; "No cloud save found. Loading default record for user %s cloudsave=%s - %s."): a new
    // island gets its playset's own record (ResetLevelRecord), applied, and the load finishes.
    void LoadFromDefault(uint32_t Status);
    // The game's own (Natives::UFortLevelSaveComponent_ResetLevelRecord): the record back to the volume's playset's.
    void ResetLevelRecord();
    // The island's file, as the game names it (0xF476E0): islands/personal/<owner's account id>/<plot item id>.island.
    FString GetCloudSaveFileName() const;
    // Its checkpoint, the island menu's backup (7.10's "islands/personal/%s/%s.%s" with "checkpoint.island", 0xF76CE0).
    FString GetCheckpointFileName() const;
    // 7.10's (0xF7EF90, "Attempting to load plot from mnemonic %s"): a published island by its code, loaded for UserId
    // on the volume's queue (the links service's link, its playset, its file from the DSS), as the game's own featured
    // rifts load theirs (the component's BeginPlay, 0xF5E610).
    void LoadPlotFromMnemonic(const FUniqueNetIdRepl& UserId, const FString& Mnemonic);
    // Core's (PongooDev/CoreV2#488): the checkpoint's date from the DSS into LastCheckpointDateUtc, as the island loads,
    // so the island menu offers Restore for a backup made in an earlier match.
    void RefreshCheckpointDate();

    AFortVolume* GetVolume() const;
    // The volume's own component (one per island space).
    static UFortLevelSaveComponent* GetForVolume(AActor* Volume);
    // Whether UserId owns the island loaded here (AccountIdOfOwner).
    bool IsOwnedBy(const FUniqueNetIdRepl& UserId) const;

private:
    void FinishPlotLoad(bool bSucceeded, const std::wstring& ErrorCode);
    // The loaded island's last save, with its checkin (the game's own cloud save, as the component's EndPlay calls it).
    void CheckInLoadedPlot();
};
