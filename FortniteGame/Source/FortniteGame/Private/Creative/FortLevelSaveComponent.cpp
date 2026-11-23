#include "pch.h"
#include "Creative/FortLevelSaveComponent.h"
#include "AsyncTaskQueue.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "BackendRequest.h"
#include "Creative/FortVolume.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameInstance.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
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
#include <cwchar>
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
    // which 7.00 can't send, so none has), from 7.40 its title (SetCreativePlotMetadata); else the template's
    // "Island {islandIndex}" with the item's number.
    std::wstring Name = Member_IslandName.IsAvailable() ? IslandName.ToStdString() : IslandTitle.ToStdString();
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

bool ULevelSaveBaseComponent::RegisterCreationToRollback(AActor* Actor)
{
    Reflection::FParams Params(FindStaticFunction_RegisterCreationToRollback(), true);
    return Params.Set(L"Actor", reinterpret_cast<UObject*>(Actor)) && Params.Call(this);
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

bool UFortLevelSaveComponent::DoesPlayerHavePermissionToEdit(const FUniqueNetIdRepl& UserRequestingPermission) const
{
    UFunction* Function = FindStaticFunction_DoesPlayerHavePermissionToEdit();
    Reflection::FParams Params(Function, true);
    if (!Function || !Params.CopyStruct(L"UserRequestingPermission", &UserRequestingPermission) || !Params.Call(const_cast<UFortLevelSaveComponent*>(this)))
    {
        return false;
    }
    const bool* Result = Params.Get<bool>(L"ReturnValue");
    return Result && *Result;
}

std::wstring UFortLevelSaveComponent::DescribePlotPermissions() const
{
    // An account id as the player with it is named in the match ("Marvelco"), else the id itself.
    const auto NameOf = [this](const std::wstring& AccountId)
    {
        UWorld* World = GetWorld();
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        if (GameState && !AccountId.empty())
        {
            const TArray<APlayerState*>& Players = GameState->PlayerArray;
            for (int32_t Index = 0; Index < Players.Num(); ++Index)
            {
                APlayerState* Player = Players[Index];
                const FUniqueNetIdRepl* PlayerId = Player ? &static_cast<const FUniqueNetIdRepl&>(Player->UniqueId) : nullptr;
                if (PlayerId && PlayerId->IsValid() && _wcsicmp(PlayerId->ToString().ToStdString().c_str(), AccountId.c_str()) == 0)
                {
                    return Player->GetPlayerName().ToStdString();
                }
            }
        }
        return AccountId;
    };
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    const std::wstring OwnerName = Owner.IsValid() ? NameOf(Owner.ToString().ToStdString()) : std::wstring(L"no one");
    if (!HasPlotPermissions())
    {
        return std::format(L"owner {}", OwnerName);
    }
    const FFortCreativePlotPermissionData& Permissions = PlotPermissions;
    std::wstring WhiteList;
    const TArray<FString>& Ids = Permissions.WhiteList;
    for (int32_t Index = 0; Index < Ids.Num(); ++Index)
    {
        WhiteList += (WhiteList.empty() ? L"" : L", ") + NameOf(Ids[Index].ToStdString());
    }
    // EFortCreativePlotPermission: Private 0, Public 1.
    return std::format(L"owner {}, {}", OwnerName, static_cast<uint8_t>(Permissions.Permission) == 1 ? std::wstring(L"Public (everyone may edit)")
        : std::format(L"Private (whitelist: {})", WhiteList.empty() ? std::wstring(L"empty") : WhiteList));
}

void UFortLevelSaveComponent::OnModifyCreativePlotPermissionsCompleteHK(UFortLevelSaveComponent* This, const FMcpQueryResult* Result, void* NewPermissions)
{
    This->OnModifyCreativePlotPermissionsComplete(*Result, NewPermissions);
}

void UFortLevelSaveComponent::OnModifyCreativePlotPermissionsComplete(const FMcpQueryResult& Result, void* NewPermissions)
{
    // FMcpQueryResult's first member is bSucceeded, what the build's own tests first (7.30 0x108EE7F).
    const bool bSucceeded = *reinterpret_cast<const bool*>(&Result);
    // The build's own: bPermissionsSaveInProgress cleared, then on success PlotPermissions takes the new permissions
    // (7.30 0x108EE9B), which replicate to the island's players; on failure "Error changing perms %s". It takes the new
    // permissions by value and destroys them.
    Natives::UFortLevelSaveComponent_OnModifyCreativePlotPermissionsComplete(this, &Result, NewPermissions);

#if WITH_SERVER_CODE
    // What it lost: 7.10's and 7.20's end of a permissions change (AFortPlayerStateAthena's MCP completion, 7.10 0xCAC630,
    // 7.20 0xD89D50: ProcessMulticastDelegate on each controller's OnCreativePermissionsChanged, +0x2118 and +0x2148)
    // told every player in the owner's island its permissions had changed. 7.30 (CL-4834550) moved the permissions onto
    // the island's component and this completion has no such broadcast, nor has anything else in the build: the only
    // broadcaster of the delegate left is the controller's OnMinigameVolumeStateChanged (0xD70510, the island's state
    // changing for a game). Athena_PlayerController still binds PermissionsChanged to it, and its HandlePermissions is
    // what gives and takes the Creative phone; its recheck timer only runs for a player who already has the phone. So a
    // player given island edit got the building and flying the game's own checks allow (DoesPlayerHavePermissionToEdit,
    // 0x106FBE0, reads the whitelist) but no phone until they entered the island again (AFortVolumeManager's
    // HandleActorEnteredVolume -> OnEnterVolume), and one whose edit was taken kept it up to the timer's 10 s.
    // Core's: the broadcast restored here, after the build's own, to the players in this island (the volume manager's
    // GetAllPlayersInVolume; 7.20 asked it for the players in the volume its owner stood in). It runs on a failed change
    // too, as 7.20's did: they are re-checked against the permissions the island has.
    AFortVolume* Island = GetVolume();
    AFortVolumeManager* Manager = Island ? AFortVolumeManager::Get(this) : nullptr;
    const std::vector<AController*> Players = Manager ? Manager->GetAllPlayersInVolume(Island) : std::vector<AController*>();
    std::wstring Told;
    int32_t Count = 0;
    for (AController* Controller : Players)
    {
        AFortPlayerControllerAthena* Player = Controller ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)) : nullptr;
        if (!Player)
        {
            continue;
        }
        const FMulticastScriptDelegate& PermissionsChanged = Player->OnCreativePermissionsChanged;
        PermissionsChanged.ProcessMulticastDelegate(nullptr);
        APlayerState* State = static_cast<APlayerState*>(reinterpret_cast<AController*>(Player)->PlayerState);
        const bool bMayEdit = State && DoesPlayerHavePermissionToEdit(State->UniqueId);
        Told += std::format(L"{}{} ({})", Told.empty() ? L"" : L", ", State ? State->GetPlayerName().ToStdString() : std::wstring(L"a player"),
            bMayEdit ? L"may edit" : L"may not edit");
        ++Count;
    }
    Log::Info(L"Creative: the island {}'s edit permissions {}: {}; {} player(s) on it told (OnCreativePermissionsChanged){}{}",
        Island ? Island->GetName().ToStdString() : std::wstring(L"(none)"), bSucceeded ? L"changed" : L"didn't change (the MCP refused ModifyCreativePlotPermissions)",
        DescribePlotPermissions(), Count, Told.empty() ? L"" : L": ", Told);
#endif
}

void UFortLevelSaveComponent::HookPermissions()
{
#if WITH_SERVER_CODE
    // 7.30+ only: the permissions on the island's component. 7.00 to 7.20 keep them on the creative profile, and their
    // completion (on AFortPlayerStateAthena) makes the broadcast itself.
    if (!IsAvailable() || !HasPlotPermissions() || !AFortPlayerControllerAthena::HasCreativePermissionsChanged())
    {
        return;
    }
    // The completion is the function that logs its failure (7.30 0x108EE70, 7.40 0x10B6800); the one place that takes
    // its address is the request's (7.30 0x109B900, 7.40 0x10C35F0), which binds it with the new permissions as the
    // delegate's payload (lea rax at 7.30 0x109B9CF, 7.40 0x10C36BF).
    const std::vector<uintptr_t> Refs = Finder::StringRefs(L"Error changing perms %s");
    const uintptr_t Complete = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    const std::vector<uintptr_t> Binds = Complete ? Finder::AddressRefs(Complete) : std::vector<uintptr_t>();
    if (Binds.size() != 1 || !AFortVolumeManager::FindStaticFunction_GetAllPlayersInVolume() || !FindStaticFunction_DoesPlayerHavePermissionToEdit())
    {
        Log::Error(L"Finder: UFortLevelSaveComponent::OnModifyCreativePlotPermissionsComplete 0x{:X} ({} binding(s), AFortVolumeManager::GetAllPlayersInVolume {}), "
            L"so an island's players aren't told when its owner changes who may edit it (no Creative phone until they enter it again)",
            Complete ? Complete - Finder::ImageBase() : 0, Binds.size(), AFortVolumeManager::FindStaticFunction_GetAllPlayersInVolume() ? L"found" : L"missing");
        return;
    }
    Natives::UFortLevelSaveComponent_OnModifyCreativePlotPermissionsComplete = reinterpret_cast<decltype(Natives::UFortLevelSaveComponent_OnModifyCreativePlotPermissionsComplete)>(Complete);
    const bool bBound = Hooks::RedirectAddress(L"UFortLevelSaveComponent::OnModifyCreativePlotPermissionsComplete", reinterpret_cast<uint8_t*>(Binds.front()),
        reinterpret_cast<void*>(&OnModifyCreativePlotPermissionsCompleteHK));
    Log::Info(L"Finder: UFortLevelSaveComponent::OnModifyCreativePlotPermissionsComplete 0x{:X}, bound at 0x{:X}: {}", Complete - Finder::ImageBase(),
        Binds.front() - Finder::ImageBase(), bBound ? L"Core's tells the island's players their permissions changed" : L"not bound");
#endif
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
    // a 404 (an expected answer, not logged as a failure). No checkpoint is FDateTime::MaxValue, as the game has it (PongooDev/CoreV2#489): 7.10 sets that in the
    // component's constructor (0xF580F0), as a load starts (0xF7DF50), on a reset (0xF88310) and when the island's file
    // has no "checkpoint-at" meta (0xF80920). The island menu (UFortCreativePublishOptions, 0x30A46E0) shows
    // "N Days ago" from UtcNow minus the date and hides the restore row when that is negative; an empty date (0 ticks,
    // 0001-01-01) read as 739,936 days ago.
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
            FDateTime Date = FDateTime::MaxValue();
            if (UnixMs > 0)
            {
                // FDateTime's ticks: 100 ns since 0001-01-01, the Unix epoch at 621355968000000000.
                Date.Ticks = 621355968000000000LL + UnixMs * 10000LL;
            }
            Component->LastCheckpointDateUtc = Date;
            if (AActor* Owner = reinterpret_cast<AActor*>(Component->GetVolume()))
            {
                Owner->ForceNetUpdate();
            }
        });
    }, true);
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
        || (!Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback && !Natives::UPlaysetLevelStreamComponent_UnloadPlayset))
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
