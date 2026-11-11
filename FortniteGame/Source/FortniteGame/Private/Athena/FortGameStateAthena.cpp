#include "pch.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Log.h"
#include "Misc/StringAssetReference.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/AssetPtr.h"
#include "UObject/Class.h"

namespace
{
#if WITH_SERVER_CODE
    // The world's Battle Royale game mode, whose PlaylistManager has the playlists (5.21 0xBD5BBD: the game state's
    // world's AuthorityGameMode, if it is an AFortGameModeAthena).
    AFortGameModeAthena* AthenaGameModeOf(AFortGameStateAthena* GameState)
    {
        UWorld* World = GameState->GetWorld();
        return World ? Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    }

    bool IsPlaylistLoadedOrLoading(AFortGameStateAthena* GameState)
    {
        return AFortGameStateAthena::HasPlaylistDataLoadState() && (GameState->bPlaylistDataIsLoaded || GameState->bPlaylistDataIsActivelyLoading);
    }

    // What both of SetCurrentPlaylistId's copies in 5.21's OnGivenMatchAssignment do with the playlist they found (from
    // 0xBD5DEA): it becomes CurrentPlaylistData with its aircraft behaviour, the game mode takes its AI settings, then
    // the game state's values from it (0xBD2710) and LoadCurrentPlaylistData (0xBD3AD0), then its additional levels
    // (0xBE7080).
    void TakePlaylist(AFortGameStateAthena* GameState, AFortGameModeAthena* GameMode, UFortPlaylistAthena* Playlist)
    {
        GameState->CurrentPlaylistData = Playlist;
        if (AFortGameStateAthena::HasAirCraftBehavior())
        {
            GameState->AirCraftBehavior = Playlist->AirCraftBehavior;
        }
        // AFortGameModeAthena::SetAISettings (5.21 0xBD80F0), stripped with it from 5.30, also hands them to the world's
        // AI system; Battle Royale's playlists have none.
        UObject* PlaylistAISettings = UFortPlaylistAthena::HasAISettings() ? static_cast<UObject*>(Playlist->AISettings) : nullptr;
        if (PlaylistAISettings && GameMode && AFortGameModeAthena::HasAISettings())
        {
            GameMode->AISettings = PlaylistAISettings;
            Log::Warning(L"PLAYLIST: {} has AI settings ({}); the game mode takes them, the world's AI system doesn't", static_cast<FName>(Playlist->PlaylistName).ToStdString(),
                PlaylistAISettings->GetName().ToStdString());
        }
        // 0xBD2710 and LoadCurrentPlaylistData are what OnRep_CurrentPlaylistData does (5.21 0xBD7770, 5.30 0xBEC6C0),
        // whose "(Client Only)" log shows on the server.
        if (UFunction* OnRep = AFortGameStateAthena::FindStaticFunction_OnRep_CurrentPlaylistData())
        {
            Reflection::FParams Params(OnRep);
            Params.Call(GameState);
        }
        else
        {
            Log::Error(L"PLAYLIST: AFortGameStateAthena::OnRep_CurrentPlaylistData isn't on this build, so {} doesn't load", static_cast<FName>(Playlist->PlaylistName).ToStdString());
        }
        GameState->StreamAdditionalPlaylistLevels();
    }
#endif
}

void AFortGameStateAthena::SetCurrentPlaylistId(int32_t PlaylistId)
{
#if WITH_SERVER_CODE
    if (!HasCurrentPlaylistData())
    {
        return;
    }
    // The game's own where it is a function of its own.
    const ptrdiff_t StormReachedFinalPositionOffset = HasStormReachedFinalPosition() ? Member_bStormReachedFinalPosition.GetOffsetChecked() : 0;
    if (Finder::ResolveSetCurrentPlaylistIdLive(StormReachedFinalPositionOffset))
    {
        Natives::AFortGameStateAthena_SetCurrentPlaylistId(this, PlaylistId);
        return;
    }
    // 5.21's copy of the id's (0xBD5CBA): the storm hasn't reached its final position; then, on the server, unless the
    // game state has the playlist with that id already, loaded or loading, the game mode's playlist manager's with that
    // id becomes its playlist. 5.30's game state has no CurrentPlaylistId of its own: its playlist's is the one compared.
    if (HasStormReachedFinalPosition())
    {
        bStormReachedFinalPosition = false;
    }
    UFortPlaylistAthena* Current = CurrentPlaylistData;
    const int32_t CurrentId = HasCurrentPlaylistId() ? static_cast<int32_t>(CurrentPlaylistId) : Current ? static_cast<int32_t>(Current->PlaylistId) : -1;
    if (Role != ROLE_Authority || (CurrentId == PlaylistId && IsPlaylistLoadedOrLoading(this)))
    {
        return;
    }
    if (HasCurrentPlaylistId())
    {
        CurrentPlaylistId = PlaylistId;
    }
    AFortGameModeAthena* GameMode = AthenaGameModeOf(this);
    UFortPlaylistManager* Playlists = GameMode ? static_cast<UFortPlaylistManager*>(GameMode->PlaylistManager) : nullptr;
    UFortPlaylistAthena* Playlist = Playlists ? Playlists->GetPlaylist(PlaylistId) : nullptr;
    if (!Playlist)
    {
        Log::Warning(L"PLAYLIST: playlist {} isn't one of the game mode's playlist manager's, so the game state's stays {}", PlaylistId,
            Current ? static_cast<FName>(Current->PlaylistName).ToStdString() : std::wstring(L"none"));
        return;
    }
    Log::Info(L"PLAYLIST: Playlist Object found in AFortGameStateAthena::SetCurrentPlaylistId() PlaylistName is {} (Server Only)", static_cast<FName>(Playlist->PlaylistName).ToStdString());
    TakePlaylist(this, GameMode, Playlist);
#endif
}

void AFortGameStateAthena::SetCurrentPlaylistId(FName PlaylistName)
{
#if WITH_SERVER_CODE
    if (!HasCurrentPlaylistData() || PlaylistName.IsNone())
    {
        return;
    }
    // 5.21's copy of the name's (0xBD5B68): unless the game state has the playlist of that name already, loaded or
    // loading, the game mode's playlist manager's of that name becomes its playlist, and its id the game state's and the
    // game mode's CurrentPlaylistId.
    UFortPlaylistAthena* Current = CurrentPlaylistData;
    if (Current && static_cast<FName>(Current->PlaylistName) == PlaylistName && IsPlaylistLoadedOrLoading(this))
    {
        return;
    }
    AFortGameModeAthena* GameMode = AthenaGameModeOf(this);
    UFortPlaylistManager* Playlists = GameMode ? static_cast<UFortPlaylistManager*>(GameMode->PlaylistManager) : nullptr;
    UFortPlaylistAthena* Playlist = Playlists ? Playlists->GetPlaylist(PlaylistName) : nullptr;
    if (!Playlist)
    {
        Log::Warning(L"PLAYLIST: {} isn't one of the game mode's playlist manager's, so the game state's stays {}", PlaylistName.ToStdString(),
            Current ? static_cast<FName>(Current->PlaylistName).ToStdString() : std::wstring(L"none"));
        return;
    }
    Log::Info(L"PLAYLIST: Playlist Object found in AFortGameStateAthena::SetCurrentPlaylistId() PlaylistName is {} (Server Only)", PlaylistName.ToStdString());
    if (HasCurrentPlaylistId())
    {
        CurrentPlaylistId = Playlist->PlaylistId;
    }
    GameMode->CurrentPlaylistId = Playlist->PlaylistId;
    TakePlaylist(this, GameMode, Playlist);
#endif
}

void AFortGameStateAthena::StreamAdditionalPlaylistLevels()
{
#if WITH_SERVER_CODE
    UFortPlaylistAthena* Playlist = HasCurrentPlaylistData() && UFortPlaylist::HasAdditionalLevels() && Role == ROLE_Authority ? static_cast<UFortPlaylistAthena*>(CurrentPlaylistData) : nullptr;
    if (!Playlist)
    {
        return;
    }
    const TArray<TAssetPtr<UWorld>>& Levels = Playlist->AdditionalLevels;
    if (Levels.Num() > 0 && !Natives::AFortGameState_StartStreamingAdditionalPlaylistLevel)
    {
        Log::Error(L"PLAYLIST: AFortGameState::StartStreamingAdditionalPlaylistLevel wasn't found, so {}'s {} additional levels don't stream in",
            static_cast<FName>(Playlist->PlaylistName).ToStdString(), Levels.Num());
        return;
    }
    for (int32_t Index = 0; Index < Levels.Num(); ++Index)
    {
        const FStringAssetReference& Level = SoftElementAt(Levels, Index).ToStringReference();
        if (Level.IsValid())
        {
            const FString PackageName = Level.GetLongPackageName();
            Natives::AFortGameState_StartStreamingAdditionalPlaylistLevel(reinterpret_cast<AFortGameState*>(this), FName(*PackageName));
        }
    }
#endif
}
