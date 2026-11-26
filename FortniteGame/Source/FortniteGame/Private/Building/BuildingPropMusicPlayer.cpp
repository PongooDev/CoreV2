#include "pch.h"
#include "Building/BuildingPropMusicPlayer.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Misc/CoreMiscDefines.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortMusic, Log, All);

namespace
{
    // A parameterless exec ("Code += !!Code", then "jmp Body") whose body is the shared empty function.
    bool ExecJumpsToStub(UFunction* Function)
    {
        const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        if (!Exec)
        {
            return false;
        }
        static constexpr uint8_t TailJump[] = { 0x4C, 0x89, 0x42, 0x20, 0xE9 }; // mov [rdx+20h], r8; jmp rel32
        for (uintptr_t Address = Exec; Address < Exec + 0x20; ++Address)
        {
            if (std::memcmp(reinterpret_cast<const void*>(Address), TailJump, sizeof(TailJump)) == 0)
            {
                return Finder::IsStubFunction(Finder::RelativeTarget(Address + 4, 1, 5));
            }
        }
        return false;
    }
}

// 7.40 to 9.01 strip the body: the exec (9.00 CL-6337466 0xA1E060, 8.51 0xA40010, 9.01 0xA1E5A0) is the shared
// parameterless one that jumps to the empty function, so a radio kept the track its BeginPlay gave it whatever its
// options said. The options themselves are applied by the intact ServerUpdateActorOptions_Implementation (9.00 0x17BB450)
// through UFortActorOptionsComponent::SetPropertyOverrides (0x12E02F0), which broadcasts OnAnyPropertyChanged.
// SavedTrackName is the track's asset name: the client's spinner writes it from the chosen row's MusicTrackPath
// (TrackNameToAssetName, 9.00 0x143D6B0), and MusicArray holds the enabled rows' MusicTrackPaths in order (BeginPlay
// 0x1196AA0), which SongIndex indexes on the clients.
void ABuildingPropMusicPlayer::StartSongForOptionsChange()
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }
    FMusicPlayerData& SongData = GetMember_PlayingSongData();
    int32_t NewSongIndex = INDEX_NONE;
    if (PlaybackType == EMusicTrackPlayback::Enabled)
    {
        const TArray<TAssetPtr<UObject>>& Songs = GetMember_MusicArray();
        // Core's: a track name that matches none of the tracks (none chosen yet) keeps the one playing, or the first.
        NewSongIndex = SongData.SongIndex >= 0 && SongData.SongIndex < Songs.Num() ? SongData.SongIndex : (Songs.Num() > 0 ? 0 : INDEX_NONE);
        const FString& TrackName = GetMember_SavedTrackName();
        for (int32_t Index = 0; TrackName.Len() > 0 && Index < Songs.Num(); ++Index)
        {
            if (_wcsicmp(*SoftElementAt(Songs, Index).ToStringReference().GetAssetName(), *TrackName) == 0)
            {
                NewSongIndex = Index;
                break;
            }
        }
    }
    if (NewSongIndex == SongData.SongIndex)
    {
        return;
    }
    FlushNetDormancy();
    SongData.SongIndex = NewSongIndex;
    UWorld* World = GetWorld();
    SongData.ServerTimeSongStarted = World ? World->GetTimeSeconds() : 0.0f;
    ForceNetUpdate();
    UE_LOG(LogFortMusic, Log, TEXT("StartSongForOptionsChange: %s plays song %d (%s)"), *GetName(), NewSongIndex, *GetMember_SavedTrackName());
#endif
}

DEFINE_FUNCTION(ABuildingPropMusicPlayer::execStartSongForOptionsChange)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<ABuildingPropMusicPlayer*>(Context)->StartSongForOptionsChange();
    P_NATIVE_END;
}

void ABuildingPropMusicPlayer::HookNatives()
{
#if WITH_SERVER_CODE
    // 7.40 adds the class.
    UFunction* Function = StaticClass() ? FindStaticFunction_StartSongForOptionsChange() : nullptr;
    if (!Function)
    {
        return;
    }
    if (!ExecJumpsToStub(Function))
    {
        Log::Info(L"Finder: ABuildingPropMusicPlayer::StartSongForOptionsChange is native in this build");
        return;
    }
    Function->SetNativeFunc(&execStartSongForOptionsChange);
    Log::Info(L"Hook ABuildingPropMusicPlayer::StartSongForOptionsChange: native");
#endif
}
