#include "pch.h"
#include "Creative/FortLevelSaveComponent.h"
#include "AsyncTaskQueue.h"
#include "BackendRequest.h"
#include "Creative/FortVolume.h"
#include "Engine/World.h"
#include "FortGameInstance.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetTextLibrary.h"
#include "Log.h"
#include "GameThread.h"
#include "Natives.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "PlaysetLevelStreamComponent.h"
#include "ReflectionParams.h"
#include "SidecarSys.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // The DSS app the islands are kept in: the game's own (7.00's app_name global, 0x512EDE0, set to "creative").
    constexpr const wchar_t* CreativeAppName = L"creative";

    // An FAsyncTaskResult (the island steps' delegates take it as their one parameter), built and destroyed through its
    // script struct.
    class FAsyncTaskResultStorage
    {
    public:
        FAsyncTaskResultStorage(bool bSucceeded, const std::wstring& ErrorCode)
        {
            UScriptStruct* Struct = FAsyncTaskResult::StaticStruct();
            if (!Struct)
            {
                return;
            }
            Storage.assign(Struct->GetStructureSize(), 0);
            Struct->InitializeStruct(Storage.data());
            Get().bSucceeded = bSucceeded;
            Get().ErrorCode = FString(ErrorCode.c_str());
        }

        ~FAsyncTaskResultStorage()
        {
            if (!Storage.empty())
            {
                FAsyncTaskResult::StaticStruct()->DestroyStruct(Storage.data());
            }
        }

        bool IsValid() const { return !Storage.empty(); }
        FAsyncTaskResult& Get() { return *reinterpret_cast<FAsyncTaskResult*>(Storage.data()); }

    private:
        std::vector<uint8_t> Storage;
    };

    void CallNoParams(UObject* Object, UFunction* Function)
    {
        Reflection::FParams Params(Function, true);
        if (Object && Function)
        {
            Params.Call(Object);
        }
    }
}

FString UFortCreativeRealEstatePlotItem::GetItemId() const
{
    // UMcpItem's id getter, returned by value (the caller's FString), as ServerLoadPlotForPortal_Implementation and the
    // island's file name (0xF475F0) call it.
    FString Result;
    if (Natives::UMcpItem_GetItemIdIndex < 0)
    {
        // 7.20 strips ServerLoadPlotForPortal_Implementation, where Core finds the getter's slot: the getter
        // (UFortAccountItem::GetInstanceId, 7.20 slot 0x83 0x85E350: lea rdx, [rcx+0B8h]) copies the reflected InstanceId.
        if (Member_InstanceId.IsAvailable())
        {
            Result = InstanceId;
        }
        return Result;
    }
    using FGetItemId = FString* (*)(const UObject* This, FString* Out);
    void** VTable = *reinterpret_cast<void** const*>(this);
    reinterpret_cast<FGetItemId>(VTable[Natives::UMcpItem_GetItemIdIndex])(this, &Result);
    return Result;
}

FText UFortCreativeRealEstatePlotItem::GetIslandDisplayName() const
{
    // The item's islandName attribute once the island has a name of its own (the client's SetCreativePlotIslandName,
    // which 7.00 can't send, so none has); else the template's "Island {islandIndex}" with the item's number.
    std::wstring Name = Member_IslandName.IsAvailable() ? IslandName.ToStdString() : std::wstring();
    if (Name.empty())
    {
        UObject* Definition = Member_ItemDefinition.IsAvailable() ? static_cast<UObject*>(ItemDefinition) : nullptr;
        UFortCreativeRealEstatePlotItemDefinition* PlotDefinition = Cast<UFortCreativeRealEstatePlotItemDefinition>(Definition);
        if (PlotDefinition)
        {
            Name = UKismetTextLibrary::Conv_TextToString(PlotDefinition->UIDisplayName).ToStdString();
        }
        const std::wstring Number = std::to_wstring(Member_IslandIndex.IsAvailable() ? static_cast<int32_t>(IslandIndex) : 0);
        constexpr std::wstring_view Token = L"{islandIndex}";
        for (size_t At = Name.find(Token); At != std::wstring::npos; At = Name.find(Token, At + Number.size()))
        {
            Name.replace(At, Token.size(), Number);
        }
    }
    if (Name.empty())
    {
        Name = GetItemId().ToStdString();
    }
    return UKismetTextLibrary::Conv_StringToText(FString(Name.c_str()));
}

bool ULevelSaveRecord::ReadFromBuffer(const std::string& Bytes)
{
    if (!Natives::ULevelSaveRecord_ReadFromBuffer || Bytes.empty())
    {
        return false;
    }
    TArray<uint8_t> Buffer;
    Buffer.Reserve(static_cast<int32_t>(Bytes.size()));
    for (const char Byte : Bytes)
    {
        Buffer.Add(static_cast<uint8_t>(Byte));
    }
    // ReadFromArchive's answer (7.00 0x10898F0: false on data it can't decompress).
    return Natives::ULevelSaveRecord_ReadFromBuffer(this, Buffer);
}

void ULevelSaveRecord::NotifyFinishedRead(bool bSucceeded)
{
    if (Natives::ULevelSaveRecord_NotifyFinishedRead)
    {
        Natives::ULevelSaveRecord_NotifyFinishedRead(this, bSucceeded);
    }
}

void ULevelSaveBaseComponent::ApplySaveToWorld()
{
    // 7.10 (CL-4667333) takes bComplexShouldSpawn; the game's own native call of it (0x13649A3) passes false.
    UFunction* Function = FindStaticFunction_ApplySaveToWorld();
    Reflection::FParams Params(Function, true);
    if (Function && Params.Has(L"bComplexShouldSpawn"))
    {
        Params.Set(L"bComplexShouldSpawn", false);
    }
    if (Function)
    {
        Params.Call(this);
    }
}

const TAssetPtr<UObject>* UFortCreativeRealEstatePlotItem::GetBasePlayset() const
{
    UObject* Definition = ItemDefinition;
    UClass* DefinitionClass = UFortCreativeRealEstatePlotItemDefinition::StaticClass();
    if (!Definition || !DefinitionClass || !Definition->IsA(DefinitionClass))
    {
        return nullptr;
    }
    return &static_cast<const TAssetPtr<UObject>&>(static_cast<UFortCreativeRealEstatePlotItemDefinition*>(Definition)->BasePlayset);
}

void ULevelSaveBaseComponent::ConstructNewLevelRecord(AActor* Owner)
{
    Reflection::FParams Params(FindStaticFunction_ConstructNewLevelRecord(), true);
    if (Params.Set(L"Owner", Owner))
    {
        Params.Call(this);
    }
}

void ULevelSaveBaseComponent::SaveOutLevelRecord()
{
    CallNoParams(this, FindStaticFunction_SaveOutLevelRecord());
}

void ULevelSaveBaseComponent::StartPeriodicSaveTimer()
{
    CallNoParams(this, FindStaticFunction_StartPeriodicSaveTimer());
}

void ULevelSaveBaseComponent::StopPeriodicSaveTimer()
{
    CallNoParams(this, FindStaticFunction_StopPeriodicSaveTimer());
}

AFortVolume* UFortLevelSaveComponent::GetVolume() const
{
    return Cast<AFortVolume>(reinterpret_cast<UObject*>(GetOwner()));
}

UFortLevelSaveComponent* UFortLevelSaveComponent::GetForVolume(AActor* Volume)
{
    UClass* ComponentClass = StaticClass();
    const TArray<UActorComponent*> Components = Volume && ComponentClass ? Volume->GetComponentsByClass(ComponentClass) : TArray<UActorComponent*>();
    return Components.Num() > 0 ? static_cast<UFortLevelSaveComponent*>(Components[0]) : nullptr;
}

bool UFortLevelSaveComponent::IsOwnedBy(const FUniqueNetIdRepl& UserId) const
{
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    return UserId.IsValid() && Owner.IsValid() && Owner.ToString().ToStdString() == UserId.ToString().ToStdString();
}

FString UFortLevelSaveComponent::GetCloudSaveFileName() const
{
    // 0xF476E0: islands/personal/%s/%s, the owner's account id and the plot's "%s.island" (0xF475F0).
    UFortCreativeRealEstatePlotItem* Plot = LoadedPlot;
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    if (!Plot || !Owner.IsValid())
    {
        return FString();
    }
    return FString(std::format(L"islands/personal/{}/{}.island", Owner.ToString().ToStdString(), Plot->GetItemId().ToStdString()).c_str());
}

FString UFortLevelSaveComponent::GetCheckpointFileName() const
{
    // 7.10's 0xF76CE0 with its checkpoint flag: islands/personal/%s/%s.%s, "checkpoint.island" in place of "island".
    UFortCreativeRealEstatePlotItem* Plot = LoadedPlot;
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    if (!Plot || !Owner.IsValid())
    {
        return FString();
    }
    return FString(std::format(L"islands/personal/{}/{}.checkpoint.island", Owner.ToString().ToStdString(), Plot->GetItemId().ToStdString()).c_str());
}

void UFortLevelSaveComponent::LoadPlotFromMnemonic(const FUniqueNetIdRepl& UserId, const FString& Mnemonic)
{
#if WITH_SERVER_CODE
    if (!Natives::UFortLevelSaveComponent_LoadPlotFromMnemonic)
    {
        Log::Error(L"Creative: the island {} can't load: UFortLevelSaveComponent's mnemonic load wasn't found", Mnemonic.ToStdString());
        return;
    }
    Natives::UFortLevelSaveComponent_LoadPlotFromMnemonic(this, UserId, Mnemonic);
#endif
}

void UFortLevelSaveComponent::RefreshCheckpointDate()
{
#if WITH_SERVER_CODE
    // Core-Backend's DSS answers ?info=true with the file's last write (updatedUnixMs), not its bytes; no checkpoint is
    // a 404, and the date stays empty (the menu's Restore stays off).
    if (!Member_LastCheckpointDateUtc.IsAvailable() || !Member_BackupSaveState.IsAvailable())
    {
        return;
    }
    const std::wstring File = GetCheckpointFileName().ToStdString();
    if (File.empty())
    {
        return;
    }
    const FWeakObjectPtr WeakThis(this);
    BackendRequest::GetJson(std::format(L"checkpoint date of {}", File), std::format(L"/api/dss/v1/creative/{}?info=true", File),
        FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [WeakThis](uint32_t Status, const std::string& Body)
    {
        const long long UnixMs = Status == 200 ? std::atoll(BackendRequest::JsonField(Body, "updatedUnixMs").c_str()) : 0;
        GameThread::Enqueue([WeakThis, UnixMs]()
        {
            UFortLevelSaveComponent* Component = static_cast<UFortLevelSaveComponent*>(WeakThis.Get());
            if (!Component)
            {
                return;
            }
            FDateTime Date;
            // FDateTime's ticks: 100 ns since 0001-01-01, the Unix epoch at 621355968000000000.
            Date.Ticks = UnixMs > 0 ? 621355968000000000LL + UnixMs * 10000LL : 0;
            Component->LastCheckpointDateUtc = Date;
            if (AActor* Owner = reinterpret_cast<AActor*>(Component->GetVolume()))
            {
                Owner->ForceNetUpdate();
            }
        });
    });
#endif
}

void UFortLevelSaveComponent::LoadPlotForUser(const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem)
{
#if WITH_SERVER_CODE
    // What the build keeps of the stripped load, its log lines (7.00 0x3C79350 to 0x3C799B0), in their order: the plot
    // checks, "Attempting to load playset plot for user %s, cloudsave=%s.", CloudLoadFinished bound, and the default
    // record when the DSS has no file. The steps run on the volume's task queue, as the game's own restricted plot load
    // (BeginRestrictedPlotLoad, 0xF363B0) and reset (AFortVolumeManager::ResetVolumeAndLevelRecord, 0xDA0E70) run theirs:
    // CloudLoadFinished (0xF38F80) ends the load only once the queue is empty (bIsLoaded, the volume Ready, the rift told
    // through OnFinishedReadingCloudFiles), so the load's steps go after any a reset left to run.
    AFortVolume* Volume = GetVolume();
    if (!Volume || reinterpret_cast<AActor*>(Volume)->Role != ROLE_Authority || !IsAvailable())
    {
        return;
    }
    const std::wstring User = UserId.ToString().ToStdString();
    if (!UserId.IsValid())
    {
        Log::Warning(L"Creative: Rejecting plot load for user {}: {}", User, L"invalid user");
        return;
    }
    if (!PlotItem)
    {
        Log::Warning(L"Creative: Rejecting plot load for user {}. (plot {} not found)", User, L"None");
        return;
    }
    if (static_cast<UObject*>(RestrictedPlotDefinition))
    {
        // The hub's featured islands: a plot of their own, no player's.
        Log::Warning(L"Creative: Unable to load plot for user {}, requested plot doesn't match plot restriction.", User);
        return;
    }
    UAsyncTaskQueue* Queue = UAsyncTaskQueue::IsAvailable() ? Volume->GetTaskQueue() : nullptr;
    if (!Queue || !Natives::AFortVolume_ClearVolumeOnServer || !Natives::UFortLevelSaveComponent_ResetLevelRecord || !Natives::ULevelSaveRecord_NotifyFinishedRead
        || !Natives::UFortLevelSaveComponent_LoadPlaysetAsync || !Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback
        || !Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback)
    {
        Log::Error(L"Creative: {}'s island {} can't load: the volume's task queue or one of the island's steps wasn't found (see the \"Finder: Creative's island load\" line)",
            User, PlotItem->GetItemId().ToStdString());
        return;
    }

    // The island the volume had is saved and checked in first; its actors go with the load's first step.
    if (UFortCreativeRealEstatePlotItem* Previous = LoadedPlot; Previous && Previous != PlotItem)
    {
        CheckInLoadedPlot();
    }
    StopPeriodicSaveTimer();
    if (UScriptStruct* NetIdStruct = FUniqueNetIdRepl::StaticStruct())
    {
        NetIdStruct->CopyScriptStruct(&static_cast<FUniqueNetIdRepl&>(AccountIdOfOwner), &UserId);
    }
    LoadedPlot = PlotItem;
    LoadedPlotInstanceId = PlotItem->GetItemId();
    bIsLoaded = false;
    reinterpret_cast<AActor*>(Volume)->ForceNetUpdate();

    Log::Info(L"Creative: Attempting to load playset plot for user {}, cloudsave={}.", User, GetCloudSaveFileName().ToStdString());
    FAsyncTaskResultStorage Started(true, L"");
    if (Started.IsValid())
    {
        const FMulticastScriptDelegate& OnStart = OnStartPlotLoadForUser;
        OnStart.ProcessMulticastDelegate(&Started.Get());
    }

    // The volume emptied of what the last island (or the template a reset put back) left there, by the volume's own
    // clear: the step the game's reset starts with (0xD83860, ServerClearVolume; OnFinishVolumeClear once the deferred
    // destroys are done).
    Queue->Add(reinterpret_cast<UObject*>(Volume), UAsyncTaskQueue::FAsyncTaskDelegate::CreateUObject(Volume, &AFortVolume::ClearVolumeOnServer));
    // The island's own level, as BeginRestrictedPlotLoad queues a featured island's (bLoadPlaysetFromPlot): a playset the
    // stream component already holds is set again (it is loaded), another one replaces the streamed one (the old level
    // out, the plot's playset loaded and the volume's, its level in).
    UPlaysetLevelStreamComponent* Stream = UPlaysetLevelStreamComponent::GetForActor(reinterpret_cast<AActor*>(Volume));
    if (Stream && LoadsPlaysetFromPlot())
    {
        const TAssetPtr<UObject>* Playset = PlotItem->GetBasePlayset();
        const std::wstring PlaysetPath = Playset && Playset->ToStringReference().IsValid() ? Playset->ToStringReference().ToString().ToStdString() : std::wstring();
        UObject* Current = Stream->CurrentPlayset;
        if (Current && !PlaysetPath.empty() && _wcsicmp(Current->GetPathName().ToStdString().c_str(), PlaysetPath.c_str()) == 0)
        {
            Stream->SetPlayset(Current);
        }
        else
        {
            if (Current)
            {
                Queue->Add(reinterpret_cast<UObject*>(Stream),
                    UAsyncTaskQueue::FAsyncTaskDelegate::CreateUObject(Stream, &UPlaysetLevelStreamComponent::UnloadPlaysetAsyncNoCallback));
            }
            if (Natives::UFortLevelSaveComponent_LoadPlaysetAsyncTakesPlayset)
            {
                // 7.10+: the plot's playset is the step's payload, as BeginRestrictedPlotLoad binds it (7.20 0x1068AF0).
                const FSoftObjectPtr PlaysetToLoad = Playset ? FSoftObjectPtr(*Playset) : FSoftObjectPtr();
                Queue->Add(this, UAsyncTaskQueue::FAsyncTaskDelegate::CreateUObject(this, &UFortLevelSaveComponent::LoadPlaysetAsync, PlaysetToLoad));
            }
            else
            {
                Queue->Add(this, UAsyncTaskQueue::FAsyncTaskDelegate::CreateUObject(this, &UFortLevelSaveComponent::LoadPlaysetAsync));
            }
            Queue->Add(reinterpret_cast<UObject*>(Stream),
                UAsyncTaskQueue::FAsyncTaskDelegate::CreateUObject(Stream, &UPlaysetLevelStreamComponent::LoadPlaysetAsyncNoCallback));
        }
    }
    else
    {
        Log::Warning(L"Creative: {}'s island streams no playset: the volume {} has {}", User, Volume->GetName().ToStdString(),
            !Stream ? L"no UPlaysetLevelStreamComponent" : L"bLoadPlaysetFromPlot off");
    }
    // Then the island's file, last: its end (CloudLoadFinished) finds the queue empty and finishes the load.
    Queue->Add(this, UAsyncTaskQueue::FAsyncTaskDelegate::CreateUObject(this, &UFortLevelSaveComponent::LoadFromDssAsync));
    Queue->Start();
#endif
}

FMulticastScriptDelegate* UFortLevelSaveComponent::LoadPlaysetAsync()
{
    // 7.00 0xF54660: the plot's BasePlayset, loaded (RequestAsyncLoad) unless it is already, then
    // OnAsyncLoadingOfPlaysetComplete (0xF55180) makes it the stream component's and the volume's and broadcasts
    // OnFinishedPlaysetLoad, which it returns.
    return Natives::UFortLevelSaveComponent_LoadPlaysetAsync
        ? static_cast<FMulticastScriptDelegate*>(Natives::UFortLevelSaveComponent_LoadPlaysetAsync(this, nullptr)) : nullptr;
}

FMulticastScriptDelegate* UFortLevelSaveComponent::LoadPlaysetAsync(FSoftObjectPtr PlaysetToLoad)
{
    // 7.10+ (0xF7EA90; 7.20 0x10646C0): the same with the playset handed in. The argument is by value, which MSVC passes
    // as a pointer to the caller's copy that the callee destroys (7.20 frees its SubPathString on every return), so the
    // native gets a copy of its own that Core doesn't destroy after it.
    if (!Natives::UFortLevelSaveComponent_LoadPlaysetAsync)
    {
        return nullptr;
    }
    alignas(FSoftObjectPtr) uint8_t Argument[sizeof(FSoftObjectPtr)];
    new (Argument) FSoftObjectPtr(PlaysetToLoad);
    return static_cast<FMulticastScriptDelegate*>(Natives::UFortLevelSaveComponent_LoadPlaysetAsync(this, Argument));
}

FMulticastScriptDelegate* UFortLevelSaveComponent::LoadFromDssAsync()
{
#if WITH_SERVER_CODE
    const std::wstring User = AccountIdOfOwner.ToString().ToStdString();
    const FString FileName = GetCloudSaveFileName();
    AActor* Volume = GetOwner();
    UWorld* World = Volume ? Volume->GetWorld() : nullptr;
    UFortGameInstance* GameInstance = World && UFortGameInstance::HasSidecarSys() ? Cast<UFortGameInstance>(reinterpret_cast<UObject*>(static_cast<UGameInstance*>(World->OwningGameInstance))) : nullptr;
    USidecarSys* Sidecar = GameInstance ? static_cast<USidecarSys*>(GameInstance->SidecarSys) : nullptr;
    if (!Sidecar)
    {
        // The step ends on the next tick, once the queue waits for it.
        Log::Error(L"Creative: Unable to get sidecar when loading cloud save {} for user {}", FileName.ToStdString(), User);
        if (World)
        {
            const FWeakObjectPtr WeakThis(this);
            World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]()
            {
                if (UFortLevelSaveComponent* This = static_cast<UFortLevelSaveComponent*>(WeakThis.Get()))
                {
                    This->FinishPlotLoad(false, L"no_sidecar");
                }
            }));
        }
        return &static_cast<FMulticastScriptDelegate&>(OnFinishedLoadingCloudFiles);
    }

    const FWeakObjectPtr WeakThis(this);
    const FWeakObjectPtr WeakPlot(reinterpret_cast<UObject*>(static_cast<UFortCreativeRealEstatePlotItem*>(LoadedPlot)));
    Sidecar->Checkout(FString(CreativeAppName), FileName, USidecarSys::GetCheckoutPid(), true,
        [WeakThis, WeakPlot, User, File = FileName.ToStdString()](bool bCheckedOut, uint32_t Status, const std::string& Contents)
    {
        UFortLevelSaveComponent* This = static_cast<UFortLevelSaveComponent*>(WeakThis.Get());
        if (!This)
        {
            return;
        }
        UObject* Plot = WeakPlot.Get();
        if (!Plot || reinterpret_cast<UObject*>(static_cast<UFortCreativeRealEstatePlotItem*>(This->LoadedPlot)) != Plot)
        {
            // Another island took the volume meanwhile (its steps wait behind this one) or the player left: the step
            // ends without touching the volume, and the queue goes on.
            Log::Info(L"Creative: {}'s island {} was unloaded while it loaded; its file isn't read", User, File);
            FAsyncTaskResultStorage Ended(false, L"plot_unloaded");
            if (Ended.IsValid())
            {
                const FMulticastScriptDelegate& OnLoaded = This->OnFinishedLoadingCloudFiles;
                OnLoaded.ProcessMulticastDelegate(&Ended.Get());
            }
            return;
        }
        if (!bCheckedOut)
        {
            Log::Error(L"Creative: Failed to read user file {} - {} with error HTTP {}!", File, User, Status);
            This->FinishPlotLoad(false, L"checkout_failed");
            return;
        }
        if (Contents.empty())
        {
            This->LoadFromDefault(Status);
            return;
        }
        This->FinalizeDownload(Contents);
    });
    return &static_cast<FMulticastScriptDelegate&>(OnFinishedLoadingCloudFiles);
#else
    return nullptr;
#endif
}

void UFortLevelSaveComponent::FinalizeDownload(const std::string& Contents)
{
#if WITH_SERVER_CODE
    const std::wstring User = AccountIdOfOwner.ToString().ToStdString();
    const std::wstring File = GetCloudSaveFileName().ToStdString();
    Log::Info(L"Creative: Successfully downloaded {} - {}.", File, User);
    // A new record of the volume's for the island (the last one goes with its bindings), read from the file, then the
    // read's own end: the records tidied and OnFinishedRead, which the component answers (OnFinishedRecordRead:
    // bValidRecord, so the island saves; on 7.00 the record applied when bApplySaveAfterLoad).
    ConstructNewLevelRecord(GetOwner());
    ULevelSaveRecord* Record = LevelRecord;
    Log::Info(L"Creative: Loading level record of size {}", Contents.size());
    if (!Record || !Record->ReadFromBuffer(Contents))
    {
        // No OnFinishedRead: the record stays invalid, so nothing overwrites the player's file.
        Log::Error(L"Creative: {}'s island {} ({} bytes) couldn't be read into the volume's record ({})", User, File, Contents.size(),
            !Record ? L"no record" : Natives::ULevelSaveRecord_ReadFromBuffer ? L"ULevelSaveRecord::ReadFromArchive failed" : L"ULevelSaveRecord::ReadFromBuffer not found");
        FinishPlotLoad(false, L"read_failed");
        return;
    }
    Record->NotifyFinishedRead(true);
    if (!AppliesSaveAfterLoad())
    {
        ApplySaveToWorld();
    }
    FinishPlotLoad(true, L"");
#endif
}

void UFortLevelSaveComponent::LoadFromDefault(uint32_t Status)
{
#if WITH_SERVER_CODE
    // A new island: the record of its playset (the game's own reset of the record, which the reset's step of the same name
    // calls: the volume's playset's record, else a new one, valid), applied as the reset applies it.
    Log::Info(L"Creative: No cloud save found. Loading default record for user {} cloudsave={} - {}.", AccountIdOfOwner.ToString().ToStdString(),
        GetCloudSaveFileName().ToStdString(), Status);
    ResetLevelRecord();
    ApplySaveToWorld();
    FinishPlotLoad(true, L"");
#endif
}

void UFortLevelSaveComponent::ResetLevelRecord()
{
    if (Natives::UFortLevelSaveComponent_ResetLevelRecord)
    {
        Natives::UFortLevelSaveComponent_ResetLevelRecord(this);
    }
}

void UFortLevelSaveComponent::FinishPlotLoad(bool bSucceeded, const std::wstring& ErrorCode)
{
#if WITH_SERVER_CODE
    // The game's own end of a load: CloudLoadFinished(Result) broadcasts OnFinishedLoadingCloudFiles (the queue's step
    // ends) and, the queue empty, finishes it (bIsLoaded, the volume Ready, the rift told). The island is then saved on
    // the component's timer, by the game's own SaveOutLevelRecord (0xF5B8F0: the DSS upload).
    FAsyncTaskResultStorage Result(bSucceeded, ErrorCode);
    Reflection::FParams Params(FindStaticFunction_CloudLoadFinished(), true);
    if (Result.IsValid() && Params.CopyStruct(L"Result", &Result.Get()))
    {
        Params.Call(this);
    }
    if (bSucceeded)
    {
        StartPeriodicSaveTimer();
        // 7.10's island menu offers Restore by the checkpoint's date (PongooDev/CoreV2#488).
        RefreshCheckpointDate();
    }
    Log::Info(L"Creative: {}'s island {} {}", AccountIdOfOwner.ToString().ToStdString(), GetCloudSaveFileName().ToStdString(),
        bSucceeded ? (bIsLoaded ? L"is loaded; it saves to the DSS on the volume's timer and as the volume goes read-only for a game"
            : L"is read, but the volume's queue has steps left, so the load isn't finished") : (L"didn't load: " + ErrorCode).c_str());
#endif
}

void UFortLevelSaveComponent::CheckInLoadedPlot()
{
#if WITH_SERVER_CODE
    UFortCreativeRealEstatePlotItem* Plot = LoadedPlot;
    if (!Plot)
    {
        return;
    }
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    const std::wstring File = GetCloudSaveFileName().ToStdString();
    StopPeriodicSaveTimer();
    // The last save, with the checkin: the game's own cloud save as its EndPlay calls it (0xF41600, bCheckin true).
    if (Natives::UFortLevelSaveComponent_CloudSave && bIsLoaded)
    {
        if (Natives::UFortLevelSaveComponent_CloudSaveTakesCheckinOnly)
        {
            // 7.10: the plot and its owner are the component's own (LoadedPlot, AccountIdOfOwner).
            reinterpret_cast<bool (*)(UObject*, bool)>(Natives::UFortLevelSaveComponent_CloudSave)(this, true);
        }
        else
        {
            Natives::UFortLevelSaveComponent_CloudSave(this, Plot, Owner, true);
        }
    }
    Log::Info(L"Creative: {}'s island {} unloads{}", Owner.ToString().ToStdString(), File,
        bIsLoaded ? (Natives::UFortLevelSaveComponent_CloudSave ? L": saved and checked in" : L" unsaved: the cloud save wasn't found") : L" before it loaded: nothing to save");
#endif
}

void UFortLevelSaveComponent::ReloadPlot()
{
#if WITH_SERVER_CODE
    UFortCreativeRealEstatePlotItem* Plot = LoadedPlot;
    if (!Plot || !GetVolume())
    {
        return;
    }
    // The island as last saved: the same plot loaded again (the volume cleared, the file read again). The owner stays in
    // AccountIdOfOwner, which the load copies onto itself.
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    LoadPlotForUser(Owner, Plot);
#endif
}

void UFortLevelSaveComponent::UnloadPlotForUser()
{
#if WITH_SERVER_CODE
    UFortCreativeRealEstatePlotItem* Plot = LoadedPlot;
    AFortVolume* Volume = GetVolume();
    if (!Plot || !Volume)
    {
        return;
    }
    CheckInLoadedPlot();
    // The volume back to its template for the next player: the game's own reset, on the volume's queue.
    if (AFortVolumeManager* Manager = AFortVolumeManager::Get(Volume))
    {
        Manager->ResetVolumeAndLevelRecord(Volume);
    }
    LoadedPlot = nullptr;
    LoadedPlotInstanceId = FString();
    bIsLoaded = false;
    reinterpret_cast<AActor*>(Volume)->ForceNetUpdate();
#endif
}
