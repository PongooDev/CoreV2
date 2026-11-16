#include "pch.h"
#include "Creative/FortLevelSaveComponent.h"
#include "Creative/FortVolume.h"
#include "Engine/World.h"
#include "FortGameInstance.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetTextLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "SidecarSys.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

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
    Natives::ULevelSaveRecord_ReadFromBuffer(this, Buffer);
    return true;
}

void ULevelSaveBaseComponent::ApplySaveToWorld()
{
    CallNoParams(this, FindStaticFunction_ApplySaveToWorld());
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

void ULevelSaveBaseComponent::ClearAppliedSave()
{
    // ApplySaveToWorld (7.00 0x106D5F0) tests and sets bit 0 of +0x138, the first byte after ActorToAddAfterWrite.
    if (Member_ActorToAddAfterWrite.IsAvailable())
    {
        uint8_t* Flags = reinterpret_cast<uint8_t*>(this) + Member_ActorToAddAfterWrite.GetOffsetChecked() + sizeof(TArray<AActor*>);
        *Flags &= ~1u;
    }
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

void UFortLevelSaveComponent::LoadPlotForUser(const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem)
{
#if WITH_SERVER_CODE
    // What the build keeps of the stripped load, its log lines (7.00 0x3C79350 to 0x3C799B0), in their order: the plot
    // checks, "Attempting to load playset plot for user %s, cloudsave=%s.", CloudLoadFinished bound, and the default
    // record when the DSS has no file. The volume's own half is intact: OnAsyncLoadingOfPlaysetComplete (0xF55180)
    // streams the plot's BasePlayset into the volume's UPlaysetLevelStreamComponent ("Playset load complete for user %s,
    // cloudsave=%s."), and CloudLoadFinished (0xF38F80) sets bIsLoaded, makes the volume Ready and tells the rift
    // (OnFinishedReadingCloudFiles, which opens it).
    AActor* Volume = GetOwner();
    if (!Volume || Volume->Role != ROLE_Authority || !IsAvailable())
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
    if (UFortCreativeRealEstatePlotItem* Previous = LoadedPlot; Previous && Previous != PlotItem)
    {
        UnloadPlotForUser();
    }

    if (UScriptStruct* NetIdStruct = FUniqueNetIdRepl::StaticStruct())
    {
        NetIdStruct->CopyScriptStruct(&static_cast<FUniqueNetIdRepl&>(AccountIdOfOwner), &UserId);
    }
    LoadedPlot = PlotItem;
    LoadedPlotInstanceId = PlotItem->GetItemId();
    bIsLoaded = false;
    reinterpret_cast<AActor*>(Volume)->ForceNetUpdate();

    const FString FileName = GetCloudSaveFileName();
    Log::Info(L"Creative: Attempting to load playset plot for user {}, cloudsave={}.", User, FileName.ToStdString());
    FAsyncTaskResultStorage Started(true, L"");
    if (Started.IsValid())
    {
        const FMulticastScriptDelegate& OnStart = OnStartPlotLoadForUser;
        OnStart.ProcessMulticastDelegate(&Started.Get());
    }
    // The island's own level first, as the volume's BeginPlay does it for a plot it is given (0xF363B0).
    CallNoParams(this, FindStaticFunction_OnAsyncLoadingOfPlaysetComplete());

    UWorld* World = Volume->GetWorld();
    UFortGameInstance* GameInstance = World && UFortGameInstance::HasSidecarSys() ? Cast<UFortGameInstance>(reinterpret_cast<UObject*>(static_cast<UGameInstance*>(World->OwningGameInstance))) : nullptr;
    USidecarSys* Sidecar = GameInstance ? static_cast<USidecarSys*>(GameInstance->SidecarSys) : nullptr;
    if (!Sidecar)
    {
        Log::Error(L"Creative: Unable to get sidecar when loading cloud save {} for user {}", FileName.ToStdString(), User);
        FinishPlotLoad(false, L"no_sidecar");
        return;
    }

    const FWeakObjectPtr WeakThis(this);
    const FWeakObjectPtr WeakPlot(reinterpret_cast<UObject*>(PlotItem));
    Sidecar->Checkout(FString(CreativeAppName), FileName, USidecarSys::GetCheckoutPid(), true,
        [WeakThis, WeakPlot, User, File = FileName.ToStdString()](bool bCheckedOut, uint32_t Status, const std::string& Contents)
    {
        UFortLevelSaveComponent* This = static_cast<UFortLevelSaveComponent*>(WeakThis.Get());
        UObject* Plot = WeakPlot.Get();
        if (!This || !Plot || reinterpret_cast<UObject*>(static_cast<UFortCreativeRealEstatePlotItem*>(This->LoadedPlot)) != Plot)
        {
            // Another island took the volume meanwhile, or the player left.
            return;
        }
        if (!bCheckedOut)
        {
            Log::Error(L"Creative: Cloud failure while trying to read user file for load request: {} (HTTP {})", File, Status);
            This->FinishPlotLoad(false, L"checkout_failed");
            return;
        }
        if (Contents.empty())
        {
            Log::Info(L"Creative: No cloud save found. Loading default record for user {} cloudsave={} - {}.", User, File, Status);
        }
        else
        {
            if (!static_cast<ULevelSaveRecord*>(This->LevelRecord))
            {
                This->ConstructNewLevelRecord(This->GetOwner());
            }
            ULevelSaveRecord* Record = This->LevelRecord;
            if (!Record || !Record->ReadFromBuffer(Contents))
            {
                Log::Error(L"Creative: {}'s island {} ({} bytes) couldn't be read into the volume's record ({})", User, File, Contents.size(),
                    !Record ? L"no record" : L"ULevelSaveRecord::ReadFromBuffer not found");
                This->FinishPlotLoad(false, L"read_failed");
                return;
            }
            Log::Info(L"Creative: Successfully downloaded {} - {} ({} bytes): applying it to the island", File, User, Contents.size());
            This->ClearAppliedSave();
            This->ApplySaveToWorld();
        }
        This->FinishPlotLoad(true, L"");
    });
#endif
}

void UFortLevelSaveComponent::FinishPlotLoad(bool bSucceeded, const std::wstring& ErrorCode)
{
#if WITH_SERVER_CODE
    // The game's own end of a load: CloudLoadFinished(Result) (bIsLoaded, the volume Ready, the rift told). Then the
    // island is saved on the component's timer, by the game's own SaveOutLevelRecord (0xF5B8F0: the DSS upload).
    FAsyncTaskResultStorage Result(bSucceeded, ErrorCode);
    Reflection::FParams Params(FindStaticFunction_CloudLoadFinished(), true);
    if (Result.IsValid() && Params.CopyStruct(L"Result", &Result.Get()))
    {
        Params.Call(this);
    }
    if (bSucceeded)
    {
        StartPeriodicSaveTimer();
    }
    Log::Info(L"Creative: {}'s island {} {}", AccountIdOfOwner.ToString().ToStdString(), GetCloudSaveFileName().ToStdString(),
        bSucceeded ? L"is loaded; it saves to the DSS on the volume's timer and as the volume goes read-only for a game" : (L"didn't load: " + ErrorCode).c_str());
#endif
}

void UFortLevelSaveComponent::ReloadPlot()
{
#if WITH_SERVER_CODE
    UFortCreativeRealEstatePlotItem* Plot = LoadedPlot;
    AFortVolume* Volume = GetVolume();
    if (!Plot || !Volume)
    {
        return;
    }
    // The owner stays in AccountIdOfOwner, which the load copies onto itself.
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    // The island as last saved: the volume back to its playset, then the user's file again.
    StopPeriodicSaveTimer();
    if (AFortVolumeManager* Manager = AFortVolumeManager::Get(Volume))
    {
        Manager->ResetVolumeAndLevelRecord(Volume);
    }
    LoadedPlot = nullptr;
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
    const FUniqueNetIdRepl& Owner = AccountIdOfOwner;
    const std::wstring File = GetCloudSaveFileName().ToStdString();
    StopPeriodicSaveTimer();
    // The last save, with the checkin: the game's own cloud save as its EndPlay calls it (0xF41600, bCheckin true).
    if (Natives::UFortLevelSaveComponent_CloudSave && bIsLoaded)
    {
        Natives::UFortLevelSaveComponent_CloudSave(this, Plot, Owner, true);
    }
    Log::Info(L"Creative: {}'s island {} unloads{}", Owner.ToString().ToStdString(), File,
        bIsLoaded ? (Natives::UFortLevelSaveComponent_CloudSave ? L": saved and checked in" : L" unsaved: the cloud save wasn't found") : L" before it loaded: nothing to save");
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
