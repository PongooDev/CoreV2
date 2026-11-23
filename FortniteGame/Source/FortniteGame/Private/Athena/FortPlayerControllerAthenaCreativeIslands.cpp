#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "BackendRequest.h"
#include "Creative/FortAthenaCreativePortal.h"
#include "Creative/FortLevelSaveComponent.h"
#include "Creative/FortVolume.h"
#include "Finder.h"
#include "FortRuntimeOptions.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "Online/FortMcpProfileCreative.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/Stack.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <chrono>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <Windows.h>

// 7.10's (CL-4667333) island tools (PongooDev/CoreV2#488): the island menu's Publish Island, Backup Island and Restore
// (UFortCreativePublishOptions), and an island code entered at a rift (UFortCreativeIslandLinkScreen). The client sends
// AFortPlayerControllerAthena's PublishMyCurrentCreativePlot, ServerSaveIslandCheckpoint, ServerLoadIslandCheckpoint and
// ServerLoadPlotForPortalFromMnemonic; 7.10's server keeps the last one (0xC84E10) but for its call into the rift, and
// strips the others' bodies (0x3918A0, which only destroys the publish's by-value options; the shared empty function for
// both checkpoints). What the build keeps around them:
// - the published island's load: UFortLevelSaveComponent's mnemonic load (0xF7EF90) asks the links service for the code
//   (0xF85FC0, FOnlineLinksServiceMcp's QueryLinkByMnemonic), loads the link's plot playset (metadata islandType) and its
//   file from the DSS (metadata _filePath, 0xF7DF50), as the featured rifts do from their BeginPlay (0xF5E610);
// - the checkpoint's file name ("islands/personal/%s/%s.%s" with "checkpoint.island", 0xF76CE0, whose checkpoint caller
//   is gone), the publish's ("islands/public/%s/%s/%s.island", no reference left) and the links service's CreateLink
//   (POST /api/`namespace/author/`accountid, 0x4A0390, whose only caller was the server's publish);
// - the save, its end OnFinishedWritingCloudFiles (broadcast by 0xF619A0 from the upload's answer), and the replicated
//   BackupSaveState, RestoringState and LastCheckpointDateUtc the island menu shows.
// Core's restorations, through the game's own save and Core-Backend's links service and DSS (Core's own requests to it
// with the DedicatedServer client, as SidecarSys.cpp's checkout):
// - publish: the island saved (the game's cloud save, OnFinishedWritingCloudFiles), then CreateLink for the player with
//   the island's file as its source (the backend copies the save to the code's islands/public file), and the client told
//   (ClientPublishMyCurrentCreativePlotComplete: no error code and the link's JSON, or the error);
// - backup: the island saved, then copied to its checkpoint (the DSS restore operation, from the island's file);
// - restore: the checkpoint copied back over the island's file, and the island loaded again from it (ReloadPlot);
// - a code at a rift: the rift's island space loads it (UFortLevelSaveComponent::LoadPlotFromMnemonic).
// Nothing here runs before 7.10: 7.00 has none of these RPCs, and HookCreativeIslandTools returns at once.

namespace
{
    namespace EBackupSaveState
    {
        inline constexpr uint8_t Ready = 0;
        inline constexpr uint8_t InProgress = 1;
    }

    enum class EIslandSaveFor : uint8_t
    {
        Publish,
        Checkpoint
    };

    // What an island's save is waiting to be finished for (one at a time per island).
    struct FPendingIslandSave
    {
        EIslandSaveFor For = EIslandSaveFor::Publish;
        FWeakObjectPtr Controller;
        std::wstring AccountId;
        std::wstring PlotItemId;
        std::wstring IslandType;
        std::wstring IslandFile;
        std::wstring CheckpointFile;
        std::wstring Title;
        std::wstring Tagline;
        std::wstring Locale;
    };
    std::map<UObject*, FPendingIslandSave> PendingSaves;
    // When each island last published, backed up or restored (the component's PublishRateLimitSeconds and the rest).
    std::map<std::pair<UObject*, int>, std::chrono::steady_clock::time_point> LastToolUse;
    UFunction* IslandSavedHandler = nullptr;

    std::wstring PlayerNameOf(AFortPlayerControllerAthena* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(reinterpret_cast<AController*>(Controller)->PlayerState) : nullptr;
        return PlayerState ? PlayerState->GetPlayerName().ToStdString() : std::wstring(L"a player");
    }

    const FUniqueNetIdRepl* UniqueIdOf(AFortPlayerControllerAthena* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(reinterpret_cast<AController*>(Controller)->PlayerState) : nullptr;
        return PlayerState ? &static_cast<const FUniqueNetIdRepl&>(PlayerState->UniqueId) : nullptr;
    }

    // A string for a JSON body: quoted, with everything outside printable ASCII as \u escapes (UTF-16 units, as JSON reads them).
    std::string JsonString(const std::wstring& Text)
    {
        std::string Out = "\"";
        for (const wchar_t Character : Text)
        {
            if (Character == L'"' || Character == L'\\')
            {
                Out.push_back('\\');
                Out.push_back(static_cast<char>(Character));
            }
            else if (Character >= 0x20 && Character < 0x7F)
            {
                Out.push_back(static_cast<char>(Character));
            }
            else
            {
                Out += std::format("\\u{:04x}", static_cast<uint32_t>(static_cast<uint16_t>(Character)));
            }
        }
        return Out + "\"";
    }

    std::wstring FromUtf8(const std::string& Text)
    {
        if (Text.empty())
        {
            return {};
        }
        const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
        std::wstring Wide(Length > 0 ? Length : 0, L'\0');
        if (Length > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Wide.data(), Length);
        }
        return Wide;
    }

    std::wstring Widen(const std::string& Text)
    {
        return std::wstring(Text.begin(), Text.end());
    }

    // Whether the island's tool (0 publish, 1 backup, 2 restore) is within its rate limit (the component's config, else a
    // minute), and if not, records its use now.
    bool RateLimited(UFortLevelSaveComponent* Component, int Tool)
    {
        const int32_t Seconds = Component->GetToolRateLimitSeconds(Tool, 60);
        const auto Now = std::chrono::steady_clock::now();
        const auto Key = std::make_pair(static_cast<UObject*>(Component), Tool);
        const auto Found = LastToolUse.find(Key);
        if (Found != LastToolUse.end() && Now - Found->second < std::chrono::seconds(std::max<int32_t>(Seconds, 0)))
        {
            return true;
        }
        LastToolUse[Key] = Now;
        return false;
    }

    void SetBackupState(UFortLevelSaveComponent* Component, bool bRestore, uint8_t State)
    {
        if (!UFortLevelSaveComponent::HasBackupStates())
        {
            return;
        }
        if (bRestore)
        {
            Component->RestoringState = State;
        }
        else
        {
            Component->BackupSaveState = State;
        }
        if (AActor* Owner = reinterpret_cast<AActor*>(Component->GetVolume()))
        {
            Owner->ForceNetUpdate();
        }
    }

    // FDateTime's ticks of a Unix time in milliseconds (ticks are 100 ns since 0001-01-01; the Unix epoch is
    // 621355968000000000 of them).
    int64_t TicksOfUnixMs(int64_t Milliseconds)
    {
        return 621355968000000000LL + Milliseconds * 10000LL;
    }

    void SetCheckpointDate(UFortLevelSaveComponent* Component, int64_t UnixMs)
    {
        if (!UFortLevelSaveComponent::HasLastCheckpointDate())
        {
            return;
        }
        // No checkpoint is FDateTime::MaxValue, as the game has it (PongooDev/CoreV2#489).
        FDateTime Date = FDateTime::MaxValue();
        if (UnixMs > 0)
        {
            Date.Ticks = TicksOfUnixMs(UnixMs);
        }
        Component->LastCheckpointDateUtc = Date;
        if (AActor* Owner = reinterpret_cast<AActor*>(Component->GetVolume()))
        {
            Owner->ForceNetUpdate();
        }
    }

    int64_t UnixMsNow()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // The island's save is done: the publish or the backup it was for goes on.
    void OnIslandSaved(UFortLevelSaveComponent* Component, bool bSucceeded)
    {
        const auto Found = PendingSaves.find(reinterpret_cast<UObject*>(Component));
        if (Found == PendingSaves.end())
        {
            return;
        }
        const FPendingIslandSave Pending = Found->second;
        PendingSaves.erase(Found);
        AFortPlayerControllerAthena* Controller = static_cast<AFortPlayerControllerAthena*>(Pending.Controller.Get());
        const FWeakObjectPtr WeakController = Pending.Controller;
        const FWeakObjectPtr WeakComponent(reinterpret_cast<UObject*>(Component));
        if (Pending.For == EIslandSaveFor::Checkpoint)
        {
            if (!bSucceeded)
            {
                Log::Warning(L"Creative: {}'s island backup failed: its save failed", Pending.AccountId);
                SetBackupState(Component, false, EBackupSaveState::Ready);
                return;
            }
            // The island's file copied to its checkpoint: the DSS's restore operation on the checkpoint.
            const std::wstring Path = std::format(L"/api/dss/v1/creative/{}/restore?from={}", Pending.CheckpointFile, Pending.IslandFile);
            BackendRequest::PostJson(std::format(L"backup of {}", Pending.IslandFile), Path, std::string(), FFortMcpConfig::DedicatedServerClientId,
                FFortMcpConfig::DedicatedServerClientSecret, [WeakComponent, Pending](uint32_t Status, const std::string&)
            {
                GameThread::Enqueue([WeakComponent, Pending, Status]()
                {
                    UFortLevelSaveComponent* Saved = static_cast<UFortLevelSaveComponent*>(WeakComponent.Get());
                    if (!Saved)
                    {
                        return;
                    }
                    const bool bBackedUp = Status >= 200 && Status < 300;
                    if (bBackedUp)
                    {
                        SetCheckpointDate(Saved, UnixMsNow());
                    }
                    SetBackupState(Saved, false, EBackupSaveState::Ready);
                    Log::Info(L"Creative: {}'s island {} {}", Pending.AccountId, Pending.PlotItemId, bBackedUp ? L"is backed up (its checkpoint)" : L"wasn't backed up");
                });
            });
            return;
        }
        if (!Controller)
        {
            return;
        }
        if (!bSucceeded)
        {
            Controller->ClientPublishMyCurrentCreativePlotComplete(FString(L"save_failed"), FString());
            return;
        }
        // CreateLink for the player (the links service's POST /api/`namespace/author/`accountid), the island's file as its
        // source, whose save the backend publishes.
        const std::string Body = std::string("{\"linkType\":\"Creative:Island\",\"metadata\":{\"title\":") + JsonString(Pending.Title) + ",\"tagline\":"
            + JsonString(Pending.Tagline) + ",\"locale\":" + JsonString(Pending.Locale) + ",\"islandType\":" + JsonString(Pending.IslandType) + ",\"_sourcePath\":"
            + JsonString(Pending.IslandFile) + "}}";
        BackendRequest::PostJson(std::format(L"publish of {}", Pending.IslandFile), std::format(L"/links/api/fn/author/{}", Pending.AccountId), Body,
            FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [WeakController, Pending](uint32_t Status, const std::string& Answer)
        {
            GameThread::Enqueue([WeakController, Pending, Status, Answer]()
            {
                AFortPlayerControllerAthena* Publisher = static_cast<AFortPlayerControllerAthena*>(WeakController.Get());
                if (!Publisher)
                {
                    return;
                }
                if (Status == 200)
                {
                    const std::wstring Mnemonic = Widen(BackendRequest::JsonField(Answer, "mnemonic"));
                    Log::Info(L"Creative: {} published their island {} as {}", PlayerNameOf(Publisher), Pending.PlotItemId, Mnemonic);
                    Publisher->ClientPublishMyCurrentCreativePlotComplete(FString(), FString(FromUtf8(Answer).c_str()));
                    return;
                }
                const std::string Error = BackendRequest::JsonField(Answer, "errorCode");
                Log::Warning(L"Creative: {}'s publish of {} failed (HTTP {}): {}", PlayerNameOf(Publisher), Pending.PlotItemId, Status, Widen(Error));
                Publisher->ClientPublishMyCurrentCreativePlotComplete(FString(Error.empty() ? L"publish_failed" : Widen(Error).c_str()), FString());
            });
        });
    }

    // OnFinishedWritingCloudFiles' handler on the island's component (Core's native, the delegate's signature): its
    // FAsyncTaskResult's bSucceeded, read where the broadcast put it (the frame's Locals).
    DEFINE_FUNCTION(ExecHandleIslandSaved)
    {
        bool bSucceeded = false;
        uint8_t* const Locals = Natives::FFrame_Locals >= 0 ? P_STACK.Locals : nullptr;
        for (const UField* Field = Locals && IslandSavedHandler ? static_cast<UField*>(IslandSavedHandler->Children) : nullptr; Field; Field = Field->Next)
        {
            if (!Field->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty))
            {
                continue;
            }
            const UStructProperty* Property = reinterpret_cast<const UStructProperty*>(Field);
            UScriptStruct* Struct = Property->Struct;
            const UProperty* Succeeded = Struct ? Struct->FindPropertyByName(FName(L"bSucceeded", FNAME_Find)) : nullptr;
            if (Succeeded)
            {
                bSucceeded = *(Locals + Property->GetOffset_ForInternal() + Succeeded->GetOffset_ForInternal()) != 0;
            }
            break;
        }
        P_FINISH;
        P_NATIVE_BEGIN;
        OnIslandSaved(static_cast<UFortLevelSaveComponent*>(Context), bSucceeded);
        P_NATIVE_END;
    }

    // The island saved now for Pending (the game's own cloud save, as its timer saves it), or once the save in progress
    // ends: OnFinishedWritingCloudFiles calls Core's handler either way. False when it can't be saved.
    bool SaveIslandFor(UFortLevelSaveComponent* Component, FPendingIslandSave Pending)
    {
        if (!IslandSavedHandler || !Natives::UFortLevelSaveComponent_CloudSave || !UFortLevelSaveComponent::HasOnFinishedWritingCloudFiles())
        {
            return false;
        }
        UObject* Key = reinterpret_cast<UObject*>(Component);
        if (PendingSaves.contains(Key))
        {
            return false;
        }
        FMulticastScriptDelegate& Written = Component->OnFinishedWritingCloudFiles;
        Written.AddUniqueDynamic(Key, IslandSavedHandler->GetFName());
        PendingSaves[Key] = std::move(Pending);
        const bool bSaving = Component->IsAutoSaving();
        if (!bSaving)
        {
            if (Natives::UFortLevelSaveComponent_CloudSaveTakesCheckinOnly)
            {
                reinterpret_cast<bool (*)(UObject*, bool)>(Natives::UFortLevelSaveComponent_CloudSave)(Key, false);
            }
            else
            {
                const FUniqueNetIdRepl& Owner = Component->AccountIdOfOwner;
                Natives::UFortLevelSaveComponent_CloudSave(Key, Component->LoadedPlot, Owner, false);
            }
        }
        return true;
    }

    // Helpers as HookCreativeVTables has them: the slot's stub swapped in every controller class that has it there.
    void SwapInSubclasses(int32_t Index, void* Stub, void* Replacement, const std::wstring& HookName)
    {
        UClass* AthenaClass = AFortPlayerControllerAthena::StaticClass();
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Stub)
            {
                Hooks::SwapVTable(HookName, VTable, Index, Replacement);
            }
        }
    }

    void** ControllerVTable()
    {
        UObject* Default = AFortPlayerControllerAthena::StaticClass() ? AFortPlayerControllerAthena::StaticClass()->GetDefaultObject(false) : nullptr;
        return Default ? *reinterpret_cast<void***>(Default) : nullptr;
    }

    // The RPC's _Validate restored where the build's is a stub (a return-zero one refuses every call and disconnects).
    void RestoreValidate(UFunction* Function, void* Replacement, const wchar_t* Name)
    {
        void** VTable = ControllerVTable();
        const int32_t Index = Function ? Finder::ValidateIndex(Function) : -1;
        if (!VTable || Index < 0 || !Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            return;
        }
        SwapInSubclasses(Index, VTable[Index], Replacement, std::format(L"AFortPlayerControllerAthena::{}_Validate", Name));
    }

    // The RPC's _Implementation restored where the build's is a stub; false when it is the game's.
    bool RestoreImplementation(UFunction* Function, void* Replacement, const wchar_t* Name)
    {
        void** VTable = ControllerVTable();
        const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
        if (!VTable || Index < 0)
        {
            Log::Error(L"Finder: AFortPlayerControllerAthena::{}_Implementation slot not found", Name);
            return false;
        }
        if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            Log::Info(L"Finder: AFortPlayerControllerAthena::{}_Implementation (slot 0x{:X}) is native in this build", Name, Index);
            return false;
        }
        SwapInSubclasses(Index, VTable[Index], Replacement, std::format(L"AFortPlayerControllerAthena::{}_Implementation", Name));
        return true;
    }

    uintptr_t FunctionReferencing(const wchar_t* Text)
    {
        const std::vector<uintptr_t> Refs = Finder::StringRefs(Text);
        return Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    }
}

void AFortPlayerControllerAthena::ClientPublishMyCurrentCreativePlotComplete(const FString& ResultErrorCode, const FString& LinkJson)
{
    // 7.40 (CL-5046157) names it ClientPublishCreativePlotComplete, with the same parameters.
    UFunction* Complete = FindStaticFunction_ClientPublishMyCurrentCreativePlotComplete();
    Reflection::FParams Params(Complete ? Complete : FindStaticFunction_ClientPublishCreativePlotComplete(), true);
    if (Params.Set(L"ResultErrorCode", ResultErrorCode) && Params.Set(L"LinkJson", LinkJson))
    {
        Params.Call(this);
    }
}

UFortLevelSaveComponent* AFortPlayerControllerAthena::GetOwnLoadedIsland(AFortAthenaCreativePortal* Portal, const wchar_t*& Why)
{
    return GetOwnLoadedIslandIn(Portal ? static_cast<AFortVolume*>(Portal->LinkedVolume) : GetCreativeVolume(), Why);
}

UFortLevelSaveComponent* AFortPlayerControllerAthena::GetOwnLoadedIslandIn(AFortVolume* Volume, const wchar_t*& Why)
{
    UFortLevelSaveComponent* Component = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    Why = !Component ? L"not on an island"
        : !UserId || !Component->IsOwnedBy(*UserId) ? L"the island isn't theirs"
        : !static_cast<UFortCreativeRealEstatePlotItem*>(Component->LoadedPlot) || !Component->bIsLoaded ? L"the island isn't loaded"
        : nullptr;
    return Why ? nullptr : Component;
}

void AFortPlayerControllerAthena::PublishMyCurrentCreativePlot_Implementation(const FCreativePublishOptions& MyPublishOptions)
{
    PublishCreativePlot_Implementation(GetCreativeVolume(), MyPublishOptions);
}

void AFortPlayerControllerAthena::PublishCreativePlot_Implementation(AFortVolume* VolumeToPublish, const FCreativePublishOptions& MyPublishOptions)
{
#if WITH_SERVER_CODE
    // The game's _Validate (7.10 0xC81590, 7.40 0xDA39A0) has checked the lengths (a title of 3 to 40, a description up to
    // 150, a locale up to 10). The island published is the one in the volume (7.10 to 7.30: the one the player is on),
    // theirs and loaded.
    const wchar_t* Why = nullptr;
    UFortLevelSaveComponent* Component = GetOwnLoadedIslandIn(VolumeToPublish, Why);
    if (!Member_bIsAllowedToPublish.IsAvailable() || !bIsAllowedToPublish)
    {
        Why = L"they may not publish (no Support-A-Creator code)";
    }
    else if (Component && RateLimited(Component, 0))
    {
        Why = L"they published a moment ago";
    }
    if (Why || !Component)
    {
        Log::Warning(L"Creative: {}'s publish refused: {}", PlayerNameOf(this), Why ? Why : L"no island");
        ClientPublishMyCurrentCreativePlotComplete(FString(L"publish_refused"), FString());
        return;
    }
    UFortCreativeRealEstatePlotItem* Plot = Component->LoadedPlot;
    UObject* Definition = static_cast<UObject*>(Plot->ItemDefinition);
    FPendingIslandSave Pending;
    Pending.For = EIslandSaveFor::Publish;
    Pending.Controller = FWeakObjectPtr(reinterpret_cast<UObject*>(this));
    Pending.AccountId = static_cast<const FUniqueNetIdRepl&>(Component->AccountIdOfOwner).ToString().ToStdString();
    Pending.PlotItemId = Plot->GetItemId().ToStdString();
    // The plot definition's primary asset id, as the link's islandType names it (7.10 0xE7BA30 splits it at ':').
    Pending.IslandType = Definition ? L"CreativePlot:" + Definition->GetName().ToStdString() : std::wstring();
    Pending.IslandFile = Component->GetCloudSaveFileName().ToStdString();
    Pending.Title = MyPublishOptions.UserTitle.ToStdString();
    Pending.Tagline = MyPublishOptions.UserDescription.ToStdString();
    Pending.Locale = MyPublishOptions.UserLocale.ToStdString();
    if (!SaveIslandFor(Component, std::move(Pending)))
    {
        Log::Warning(L"Creative: {}'s publish refused: the island is being saved for another tool, or its save wasn't found", PlayerNameOf(this));
        ClientPublishMyCurrentCreativePlotComplete(FString(L"save_failed"), FString());
        return;
    }
    Log::Info(L"Creative: {} publishes their island {} (\"{}\"): saving it first", PlayerNameOf(this), Plot->GetItemId().ToStdString(), MyPublishOptions.UserTitle.ToStdString());
#endif
}

void AFortPlayerControllerAthena::UpdateCreativePlotData_Implementation(AFortVolume* VolumeToPublish, const FCreativeIslandInfo& MyInfo)
{
#if WITH_SERVER_CODE
    // 7.40's island menu, Description tab (UFortCreativeMoreInfoPanel's Apply, 0x3200F90): the client binds its
    // OnPublishComplete to the controller's publish-complete delegate (+0x2600), shows its spinner and sends this with
    // its CreativePlotLinkedVolume; ClientPublishCreativePlotComplete (0xD80DC0) broadcasts it, and an empty result
    // closes the tab as saved, any other shows "Try Again later. Error: <result>". The game's _Validate (0xDB4340) has
    // checked the lengths (a title of 3 to 40, an introduction up to 1024, a locale up to 10). The island is the
    // volume's, theirs and loaded; FCreativeIslandInfo has no tagline or video, so the plot keeps its own.
    const wchar_t* Why = nullptr;
    UFortLevelSaveComponent* Component = GetOwnLoadedIslandIn(VolumeToPublish, Why);
    UMcpProfile* Profile = Component ? GetCreativeProfile() : nullptr;
    UFortMcpProfileCreative* CreativeProfile = Profile && UFortMcpProfileCreative::StaticClass() && reinterpret_cast<UObject*>(Profile)->IsA(UFortMcpProfileCreative::StaticClass())
        ? static_cast<UFortMcpProfileCreative*>(Profile) : nullptr;
    if (Component && !CreativeProfile)
    {
        Why = L"the server has no creative profile for them";
    }
    UFortCreativeRealEstatePlotItem* Plot = Component ? static_cast<UFortCreativeRealEstatePlotItem*>(Component->LoadedPlot) : nullptr;
    if (!Why && Plot)
    {
        UFortMcpProfileCreative::FOnQueryComplete Callback = UFortMcpProfileCreative::FOnQueryComplete::CreateUObject(this, &AFortPlayerControllerAthena::OnUpdateCreativePlotDataComplete);
        const FString Tagline = Plot->IslandTagline;
        const FString Video = Plot->YoutubeVideoId;
        if (CreativeProfile->SetCreativePlotMetadata(Plot->GetItemId(), MyInfo.UserLocale, MyInfo.IslandTitle, Tagline, Video, MyInfo.IslandIntroduction, Callback))
        {
            Log::Info(L"Creative: {} renames their island {} to \"{}\"", PlayerNameOf(this), Plot->GetItemId().ToStdString(), MyInfo.IslandTitle.ToStdString());
            return;
        }
        Why = L"UFortMcpProfileCreative::SetCreativePlotMetadata wasn't sent (not found, or its context isn't 4.2's)";
    }
    Log::Warning(L"Creative: {}'s island description refused: {}", PlayerNameOf(this), Why ? Why : L"no island");
    ClientPublishMyCurrentCreativePlotComplete(FString(L"update_refused"), FString());
#endif
}

void AFortPlayerControllerAthena::OnUpdateCreativePlotDataComplete(const FMcpQueryResult& Result)
{
#if WITH_SERVER_CODE
    // FMcpQueryResult begins with FOnlineError: bSucceeded, ErrorRaw (+0x08), ErrorCode (+0x18) (7.40's
    // FOnlineError::ToLogString, 0x3A97D0). The profile's own answer has already updated the plot item, and its
    // ItemsUpdatedDelegate the controller's CreativeIslands (OnCreativeProfileItemsUpdated).
    const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(&Result);
    const bool bSucceeded = *reinterpret_cast<const bool*>(Bytes);
    FString ErrorCode = bSucceeded ? FString() : *reinterpret_cast<const FString*>(Bytes + 0x18);
    if (!bSucceeded && ErrorCode.IsEmpty())
    {
        ErrorCode = FString(L"update_failed");
    }
    if (!bSucceeded)
    {
        Log::Warning(L"Creative: {}'s island description wasn't saved: {}", PlayerNameOf(this), ErrorCode.ToStdString());
    }
    ClientPublishMyCurrentCreativePlotComplete(ErrorCode, FString());
#endif
}

void AFortPlayerControllerAthena::ServerSaveIslandCheckpoint_Implementation(AFortAthenaCreativePortal* Portal)
{
#if WITH_SERVER_CODE
    // The game's _Validate (7.10 0xC84DF0) has checked Portal is the player's own rift (OwnedPortal).
    const wchar_t* Why = nullptr;
    UFortLevelSaveComponent* Component = GetOwnLoadedIsland(Portal, Why);
    if (!Why && UFortLevelSaveComponent::HasBackupStates() && Component->BackupSaveState != EBackupSaveState::Ready)
    {
        Why = L"a backup is in progress";
    }
    if (!Why && RateLimited(Component, 1))
    {
        Why = L"they backed it up a moment ago";
    }
    if (Why)
    {
        Log::Warning(L"Creative: {}'s island backup refused: {}", PlayerNameOf(this), Why);
        return;
    }
    FPendingIslandSave Pending;
    Pending.For = EIslandSaveFor::Checkpoint;
    Pending.Controller = FWeakObjectPtr(reinterpret_cast<UObject*>(this));
    Pending.AccountId = static_cast<const FUniqueNetIdRepl&>(Component->AccountIdOfOwner).ToString().ToStdString();
    Pending.PlotItemId = static_cast<UFortCreativeRealEstatePlotItem*>(Component->LoadedPlot)->GetItemId().ToStdString();
    Pending.IslandFile = Component->GetCloudSaveFileName().ToStdString();
    Pending.CheckpointFile = Component->GetCheckpointFileName().ToStdString();
    SetBackupState(Component, false, EBackupSaveState::InProgress);
    if (!SaveIslandFor(Component, std::move(Pending)))
    {
        SetBackupState(Component, false, EBackupSaveState::Ready);
        Log::Warning(L"Creative: {}'s island backup refused: the island is being saved for another tool, or its save wasn't found", PlayerNameOf(this));
        return;
    }
    Log::Info(L"Creative: {} backs up their island: saving it first", PlayerNameOf(this));
#endif
}

void AFortPlayerControllerAthena::ServerLoadIslandCheckpoint_Implementation(AFortAthenaCreativePortal* Portal)
{
#if WITH_SERVER_CODE
    const wchar_t* Why = nullptr;
    UFortLevelSaveComponent* Component = GetOwnLoadedIsland(Portal, Why);
    if (!Why && UFortLevelSaveComponent::HasBackupStates() && Component->RestoringState != EBackupSaveState::Ready)
    {
        Why = L"a restore is in progress";
    }
    if (!Why && PendingSaves.contains(reinterpret_cast<UObject*>(Component)))
    {
        Why = L"the island is being saved for a backup or a publish";
    }
    if (!Why && RateLimited(Component, 2))
    {
        Why = L"they restored it a moment ago";
    }
    if (Why)
    {
        Log::Warning(L"Creative: {}'s island restore refused: {}", PlayerNameOf(this), Why);
        return;
    }
    // No save on the timer while the checkpoint goes over the island's file: the island loads again from it.
    Component->StopPeriodicSaveTimer();
    SetBackupState(Component, true, EBackupSaveState::InProgress);
    const std::wstring IslandFile = Component->GetCloudSaveFileName().ToStdString();
    const std::wstring Path = std::format(L"/api/dss/v1/creative/{}/restore?from={}", IslandFile, Component->GetCheckpointFileName().ToStdString());
    const FWeakObjectPtr WeakComponent(reinterpret_cast<UObject*>(Component));
    const FWeakObjectPtr WeakController(reinterpret_cast<UObject*>(this));
    BackendRequest::PostJson(std::format(L"restore of {}", IslandFile), Path, std::string(), FFortMcpConfig::DedicatedServerClientId,
        FFortMcpConfig::DedicatedServerClientSecret, [WeakComponent, WeakController, IslandFile](uint32_t Status, const std::string&)
    {
        GameThread::Enqueue([WeakComponent, WeakController, IslandFile, Status]()
        {
            UFortLevelSaveComponent* Restored = static_cast<UFortLevelSaveComponent*>(WeakComponent.Get());
            if (!Restored)
            {
                return;
            }
            const bool bRestored = Status >= 200 && Status < 300;
            if (bRestored)
            {
                Restored->ReloadPlot();
            }
            else
            {
                Restored->StartPeriodicSaveTimer();
            }
            SetBackupState(Restored, true, EBackupSaveState::Ready);
            Log::Info(L"Creative: {}'s island {} {}", PlayerNameOf(static_cast<AFortPlayerControllerAthena*>(WeakController.Get())), IslandFile,
                bRestored ? L"is restored from its checkpoint and loads again" : L"wasn't restored (no checkpoint, or the DSS refused)");
        });
    });
#endif
}

bool AFortPlayerControllerAthena::ServerLoadPlotForPortalFromMnemonic_Validate(AFortAthenaCreativePortal* Portal, const FString& Mnemonic)
{
#if WITH_SERVER_CODE
    // 7.10's (0xC84F20, ServerLoadPlotForPortal's own check): a rift with an island space that is no player's building or
    // the player's own. False disconnects the client (RPC_ValidateFailed).
    if (!Portal || !static_cast<AFortVolume*>(Portal->LinkedVolume) || Mnemonic.IsEmpty())
    {
        return false;
    }
    AFortPlayerState* RiftOwner = Portal->GetOwningPlayerState();
    APlayerState* PlayerState = static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState);
    return !RiftOwner || reinterpret_cast<UObject*>(RiftOwner) == reinterpret_cast<UObject*>(PlayerState);
#else
    return true;
#endif
}

void AFortPlayerControllerAthena::ServerLoadPlotForPortalFromMnemonic_Implementation(AFortAthenaCreativePortal* Portal, const FString& Mnemonic)
{
#if WITH_SERVER_CODE
    // 7.10's (0xC84E10): a code of two or more characters, the rift no other player's, then the rift's load for the
    // player's id (0x12C45F0).
    if (!Portal || Mnemonic.Num() <= 1 || !ServerLoadPlotForPortalFromMnemonic_Validate(Portal, Mnemonic))
    {
        return;
    }
    if (const FUniqueNetIdRepl* UserId = UniqueIdOf(this))
    {
        Portal->LoadPlotFromMnemonicForUser(*UserId, Mnemonic);
    }
#endif
}

void AFortPlayerControllerAthena::QueryPublishingPermission()
{
#if WITH_SERVER_CODE
    // Core-Backend's /core/api/creative/publishing: an enabled Support-A-Creator creator may publish (the links service's
    // CreateLink takes no one else's island). The server code that set bIsAllowedToPublish is stripped from 7.10 (only
    // the constructor writes it, 0xC525AD).
    const FUniqueNetIdRepl* UserId = Member_bIsAllowedToPublish.IsAvailable() ? UniqueIdOf(this) : nullptr;
    if (!UserId || !UserId->IsValid() || (!FindStaticFunction_PublishMyCurrentCreativePlot() && !FindStaticFunction_PublishCreativePlot()))
    {
        return;
    }
    const std::wstring AccountId = UserId->ToString().ToStdString();
    const FWeakObjectPtr WeakController(reinterpret_cast<UObject*>(this));
    BackendRequest::GetJson(std::format(L"publishing permission of {}", AccountId), std::format(L"/core/api/creative/publishing/{}", AccountId),
        FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [WeakController](uint32_t Status, const std::string& Body)
    {
        const bool bAllowed = Status == 200 && BackendRequest::JsonField(Body, "allowed") == "true";
        const std::wstring Creator = Widen(BackendRequest::JsonField(Body, "creatorName"));
        GameThread::Enqueue([WeakController, bAllowed, Creator]()
        {
            AFortPlayerControllerAthena* Controller = static_cast<AFortPlayerControllerAthena*>(WeakController.Get());
            if (!Controller)
            {
                return;
            }
            Controller->bIsAllowedToPublish = bAllowed;
            Controller->ForceNetUpdate();
            if (bAllowed)
            {
                Log::Info(L"Creative: {} may publish islands (creator code {})", PlayerNameOf(Controller), Creator);
            }
        });
    });
#endif
}

void AFortPlayerControllerAthena::RecordCreativeIslandHistory(AFortVolume* Volume)
{
#if WITH_SERVER_CODE
    // 8.20's (CL-5625478) island code screen lists the islands the player played (FortMcpUtils: GET
    // <IslandHistoryUrl>/history/<accountId>?limit=%d, 0x144CB90, and DELETE .../history/<accountId>/<mnemonic>,
    // 0x1457770), but neither the client nor the server sends anything that adds to it. Epic's service learned the play
    // server side; the build only keeps the request's "history" and "%s/%s/%s/%s" strings, unreferenced. So the player
    // is recorded as Core's restored rift teleport links them to the island (SetCreativePlotLinkedVolume, whose server
    // calls with an island every build strips), to Core-Backend's /core/api/creative/history.
    UFortLevelSaveComponent* Island = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FString Mnemonic = Island ? Island->GetLoadedMnemonic() : FString();
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    if (Mnemonic.IsEmpty() || !UserId || !UserId->IsValid())
    {
        return;
    }
    const FString AccountId = UserId->ToString();
    BackendRequest::PostJson(FString::Printf(L"island history of %s (%s)", *AccountId, *Mnemonic).ToStdString(),
        FString::Printf(L"/core/api/creative/history/%s/%s", *AccountId, *Mnemonic).ToStdString(), std::string(),
        FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret);
    Log::Info(L"Creative: {} played the island {}", PlayerNameOf(this), Mnemonic.ToStdString());
#endif
}

void AFortPlayerControllerAthena::PublishMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This, FCreativePublishOptions* MyPublishOptions)
{
    if (This && MyPublishOptions)
    {
        This->PublishMyCurrentCreativePlot_Implementation(*MyPublishOptions);
    }
    // The stripped body: the options' destructor, as the callee owns its by-value parameters.
    PublishMyCurrentCreativePlot_ImplementationOG(This, MyPublishOptions);
}

void AFortPlayerControllerAthena::PublishCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This, AFortVolume* VolumeToPublish, FCreativePublishOptions* MyPublishOptions)
{
    if (This && MyPublishOptions)
    {
        This->PublishCreativePlot_Implementation(VolumeToPublish, *MyPublishOptions);
    }
    // The stripped body: the options' destructor.
    PublishCreativePlot_ImplementationOG(This, VolumeToPublish, MyPublishOptions);
}

void AFortPlayerControllerAthena::UpdateCreativePlotData_ImplementationHK(AFortPlayerControllerAthena* This, AFortVolume* VolumeToPublish, FCreativeIslandInfo* MyInfo)
{
    if (This && MyInfo)
    {
        This->UpdateCreativePlotData_Implementation(VolumeToPublish, *MyInfo);
    }
    // The stripped body: the info's destructor.
    UpdateCreativePlotData_ImplementationOG(This, VolumeToPublish, MyInfo);
}

void AFortPlayerControllerAthena::ServerSaveIslandCheckpoint_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal)
{
    This->ServerSaveIslandCheckpoint_Implementation(Portal);
}

void AFortPlayerControllerAthena::ServerLoadIslandCheckpoint_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal)
{
    This->ServerLoadIslandCheckpoint_Implementation(Portal);
}

bool AFortPlayerControllerAthena::ServerIslandCheckpoint_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal)
{
    // 7.10's (0xC84DF0): the rift is the player's own.
    return Portal && This->GetOwnedPortal() == Portal;
}

bool AFortPlayerControllerAthena::ServerLoadPlotForPortalFromMnemonic_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& Mnemonic)
{
    return This->ServerLoadPlotForPortalFromMnemonic_Validate(Portal, Mnemonic);
}

void AFortPlayerControllerAthena::ServerLoadPlotForPortalFromMnemonic_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& Mnemonic)
{
    This->ServerLoadPlotForPortalFromMnemonic_Implementation(Portal, Mnemonic);
}

void AFortPlayerControllerAthena::LoadCuratedLinkCodes()
{
#if WITH_SERVER_CODE
    // Epic's servers had the hotfix's CuratedLinkCodes before the hub loaded. Core's 7.10 server asks for its hotfix only
    // after the playlist's levels have begun play (a headless Creative server: the featured spaces' "Attempting to load
    // playset plot for restricted plot" at 16:44:13.575, DefaultRuntimeOptions.ini asked for at 16:44:15), so their
    // BeginPlay (0xF5E610) found the array empty and loaded the build's own featured islands. So the server sets the array
    // as it starts, from the same list the hotfix carries (Core-Backend /core/api/creative/featured); the hotfix's
    // ClearArray and codes later set it to the same.
    if (!UFortRuntimeOptions::HasCuratedLinkCodes())
    {
        return;
    }
    BackendRequest::GetJson(L"featured Creative islands", L"/core/api/creative/featured", FFortMcpConfig::DedicatedServerClientId,
        FFortMcpConfig::DedicatedServerClientSecret, [](uint32_t Status, const std::string& Body)
    {
        // {"codes":["1234-5678-9012",...]}: the codes are digits and dashes only.
        std::vector<std::wstring> Codes;
        const size_t List = Status == 200 ? Body.find('[') : std::string::npos;
        const size_t End = List == std::string::npos ? std::string::npos : Body.find(']', List);
        for (size_t At = List; At != std::string::npos && At < End;)
        {
            const size_t Open = Body.find('"', At + 1);
            const size_t Close = Open == std::string::npos || Open > End ? std::string::npos : Body.find('"', Open + 1);
            if (Close == std::string::npos)
            {
                break;
            }
            Codes.push_back(Widen(Body.substr(Open + 1, Close - Open - 1)));
            At = Close;
        }
        GameThread::Enqueue([Codes, Status]()
        {
            UFortRuntimeOptions* Options = const_cast<UFortRuntimeOptions*>(UFortRuntimeOptions::Get());
            if (!Options || Status != 200)
            {
                Log::Warning(L"Creative: the featured islands weren't read (HTTP {}), so the featured rifts load the build's own", Status);
                return;
            }
            TArray<FString>& Curated = Options->CuratedLinkCodes;
            Curated.Empty();
            for (const std::wstring& Code : Codes)
            {
                Curated.Add(FString(Code.c_str()));
            }
            std::wstring Listed;
            for (const std::wstring& Code : Codes)
            {
                Listed += (Listed.empty() ? L"" : L", ") + Code;
            }
            Log::Info(L"Creative: the featured rifts load {}", Codes.empty() ? std::wstring(L"the build's own islands (none featured)") : Listed);
        });
    });
#endif
}

void AFortPlayerControllerAthena::HookCreativeIslandTools()
{
#if WITH_SERVER_CODE
    // 7.10's RPCs; 7.00 and older have none of them.
    UFunction* FromMnemonic = FindStaticFunction_ServerLoadPlotForPortalFromMnemonic();
    if (!AFortAthenaCreativePortal::StaticClass() || !FromMnemonic || !ControllerVTable())
    {
        return;
    }
    LoadCuratedLinkCodes();
    const auto Address = [](uintptr_t Function) { return Function ? Function - Finder::ImageBase() : 0; };

    // The component's mnemonic load, which a code at a rift and the featured rifts use.
    Natives::UFortLevelSaveComponent_LoadPlotFromMnemonic = reinterpret_cast<decltype(Natives::UFortLevelSaveComponent_LoadPlotFromMnemonic)>(
        FunctionReferencing(L"Attempting to load plot from mnemonic %s"));

    // ServerLoadPlotForPortalFromMnemonic: 7.10's body is the game's but for the rift's load, a call to the shared empty
    // function with the rift, the player's id and the code (0xC84EA0, after the id from 0x12C45F0). Where the whole body
    // is a stub, Core's (and its _Validate where that is one too).
    void** VTable = ControllerVTable();
    const int32_t MnemonicIndex = Finder::ImplementationIndex(FromMnemonic);
    const uintptr_t MnemonicImplementation = MnemonicIndex >= 0 ? reinterpret_cast<uintptr_t>(VTable[MnemonicIndex]) : 0;
    std::wstring MnemonicState = L"slot not found";
    if (MnemonicImplementation && Finder::IsStubFunction(MnemonicImplementation))
    {
        RestoreValidate(FromMnemonic, reinterpret_cast<void*>(&ServerLoadPlotForPortalFromMnemonic_ValidateHK), L"ServerLoadPlotForPortalFromMnemonic");
        RestoreImplementation(FromMnemonic, reinterpret_cast<void*>(&ServerLoadPlotForPortalFromMnemonic_ImplementationHK), L"ServerLoadPlotForPortalFromMnemonic");
        MnemonicState = L"stripped: restored";
    }
    else if (MnemonicImplementation)
    {
        MnemonicState = L"its stripped rift call not found";
        for (const auto& [Site, Target] : Finder::DecodedDirectCalls(MnemonicImplementation))
        {
            if (Finder::IsStubFunction(Target))
            {
                Hooks::RedirectCall(L"AFortPlayerControllerAthena::ServerLoadPlotForPortalFromMnemonic_Implementation's AFortAthenaCreativePortal::LoadPlotFromMnemonicForUser",
                    reinterpret_cast<uint8_t*>(Site), reinterpret_cast<void*>(&AFortAthenaCreativePortal::LoadPlotFromMnemonicForUserHK));
                MnemonicState = std::format(L"0x{:X}, its rift call at 0x{:X} redirected", Address(MnemonicImplementation), Address(Site));
                break;
            }
        }
    }

    // PublishMyCurrentCreativePlot: 7.10's body (0x3918A0) only destroys the options it takes by value, a few dozen bytes;
    // the game's publish would be far longer. Swapped, with the stripped body kept to destroy them after.
    // 7.40 (CL-5046157) has PublishCreativePlot in its place (slot 0x457, 0xDA3950: the same destructor-only body).
    UFunction* Publish = FindStaticFunction_PublishMyCurrentCreativePlot();
    const bool bPublishTakesVolume = !Publish && FindStaticFunction_PublishCreativePlot();
    Publish = bPublishTakesVolume ? FindStaticFunction_PublishCreativePlot() : Publish;
    const int32_t PublishIndex = Publish ? Finder::ImplementationIndex(Publish) : -1;
    void* const PublishBody = PublishIndex >= 0 ? VTable[PublishIndex] : nullptr;
    const uintptr_t PublishEnd = PublishBody ? Finder::FunctionEnd(reinterpret_cast<uintptr_t>(PublishBody)) : 0;
    std::wstring PublishState = L"slot not found";
    if (PublishBody && PublishEnd && PublishEnd - reinterpret_cast<uintptr_t>(PublishBody) <= 0x80)
    {
        if (bPublishTakesVolume)
        {
            PublishCreativePlot_ImplementationOG = reinterpret_cast<decltype(PublishCreativePlot_ImplementationOG)>(PublishBody);
            SwapInSubclasses(PublishIndex, PublishBody, reinterpret_cast<void*>(&PublishCreativePlot_ImplementationHK), L"AFortPlayerControllerAthena::PublishCreativePlot_Implementation");
        }
        else
        {
            PublishMyCurrentCreativePlot_ImplementationOG = reinterpret_cast<decltype(PublishMyCurrentCreativePlot_ImplementationOG)>(PublishBody);
            SwapInSubclasses(PublishIndex, PublishBody, reinterpret_cast<void*>(&PublishMyCurrentCreativePlot_ImplementationHK), L"AFortPlayerControllerAthena::PublishMyCurrentCreativePlot_Implementation");
        }
        PublishState = std::format(L"stripped (0x{:X}, slot 0x{:X}): restored", Address(reinterpret_cast<uintptr_t>(PublishBody)), PublishIndex);
    }
    else if (PublishBody)
    {
        PublishState = std::format(L"native in this build (slot 0x{:X})", PublishIndex);
    }

    // UpdateCreativePlotData (7.40): its body (0xDB4300, slot 0x455) only destroys the info it takes by value.
    UFunction* UpdatePlot = FindStaticFunction_UpdateCreativePlotData();
    const int32_t UpdatePlotIndex = UpdatePlot ? Finder::ImplementationIndex(UpdatePlot) : -1;
    void* const UpdatePlotBody = UpdatePlotIndex >= 0 ? VTable[UpdatePlotIndex] : nullptr;
    const uintptr_t UpdatePlotEnd = UpdatePlotBody ? Finder::FunctionEnd(reinterpret_cast<uintptr_t>(UpdatePlotBody)) : 0;
    std::wstring UpdatePlotState = UpdatePlot ? L"slot not found" : L"not in this build";
    if (UpdatePlotBody && UpdatePlotEnd && UpdatePlotEnd - reinterpret_cast<uintptr_t>(UpdatePlotBody) <= 0x80)
    {
        UpdateCreativePlotData_ImplementationOG = reinterpret_cast<decltype(UpdateCreativePlotData_ImplementationOG)>(UpdatePlotBody);
        SwapInSubclasses(UpdatePlotIndex, UpdatePlotBody, reinterpret_cast<void*>(&UpdateCreativePlotData_ImplementationHK), L"AFortPlayerControllerAthena::UpdateCreativePlotData_Implementation");
        UpdatePlotState = std::format(L"stripped (0x{:X}, slot 0x{:X}): restored", Address(reinterpret_cast<uintptr_t>(UpdatePlotBody)), UpdatePlotIndex);
    }
    else if (UpdatePlotBody)
    {
        UpdatePlotState = std::format(L"native in this build (slot 0x{:X})", UpdatePlotIndex);
    }

    // The checkpoints.
    UFunction* SaveCheckpoint = FindStaticFunction_ServerSaveIslandCheckpoint();
    UFunction* LoadCheckpoint = FindStaticFunction_ServerLoadIslandCheckpoint();
    RestoreValidate(SaveCheckpoint, reinterpret_cast<void*>(&ServerIslandCheckpoint_ValidateHK), L"ServerSaveIslandCheckpoint");
    RestoreValidate(LoadCheckpoint, reinterpret_cast<void*>(&ServerIslandCheckpoint_ValidateHK), L"ServerLoadIslandCheckpoint");
    RestoreImplementation(SaveCheckpoint, reinterpret_cast<void*>(&ServerSaveIslandCheckpoint_ImplementationHK), L"ServerSaveIslandCheckpoint");
    RestoreImplementation(LoadCheckpoint, reinterpret_cast<void*>(&ServerLoadIslandCheckpoint_ImplementationHK), L"ServerLoadIslandCheckpoint");

    // The save's end, which the publish and the backup wait for: Core's native on the component's class, with
    // OnFinishedWritingCloudFiles' signature.
    UClass* SaveClass = UFortLevelSaveComponent::StaticClass();
    const UProperty* Written = SaveClass ? SaveClass->FindPropertyByName(FName(L"OnFinishedWritingCloudFiles")) : nullptr;
    const UFunction* Signature = Written && Written->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
        ? static_cast<UFunction*>(static_cast<const UMulticastDelegateProperty*>(Written)->SignatureFunction) : nullptr;
    IslandSavedHandler = Signature && Finder::ResolveClassFunctionMap() ? SaveClass->AddNativeFunction(L"HandleIslandSavedForTools", &ExecHandleIslandSaved, Signature) : nullptr;

    Log::Info(L"Finder: Creative's island tools: UFortLevelSaveComponent's mnemonic load 0x{:X}; ServerLoadPlotForPortalFromMnemonic {}; {} {}; "
        L"UpdateCreativePlotData {}; the save's end handler {}", Address(reinterpret_cast<uintptr_t>(Natives::UFortLevelSaveComponent_LoadPlotFromMnemonic)), MnemonicState,
        bPublishTakesVolume ? L"PublishCreativePlot" : L"PublishMyCurrentCreativePlot", PublishState, UpdatePlotState,
        IslandSavedHandler ? L"added" : L"not added");
    if (!Natives::UFortLevelSaveComponent_LoadPlotFromMnemonic || !IslandSavedHandler || !Natives::UFortLevelSaveComponent_CloudSave)
    {
        Log::Error(L"Finder: Creative's island tools are missing a part, so island codes, publishing or backups don't work (see the line above)");
    }
#endif
}
