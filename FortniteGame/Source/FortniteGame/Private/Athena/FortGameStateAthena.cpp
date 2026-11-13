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
    // 0xBD5DEA): it becomes CurrentPlaylistData (6.01: CurrentPlaylistInfo's) with its aircraft behaviour, the game mode
    // takes its AI settings, then the game state's values from it (0xBD2710) and LoadCurrentPlaylistData (0xBD3AD0),
    // then its additional levels (0xBE7080).
    void TakePlaylist(AFortGameStateAthena* GameState, AFortGameModeAthena* GameMode, UFortPlaylistAthena* Playlist)
    {
        GameState->SetCurrentPlaylist(Playlist);
        if (AFortGameStateAthena::HasAirCraftBehavior())
        {
            GameState->AirCraftBehavior = Playlist->AirCraftBehavior;
        }
        // AFortGameModeAthena::SetAISettings (5.21 0xBD80F0), stripped with it from 5.30: Battle Royale's own playlists
        // have none; 6.20's Fortnitemares (the Deimos playlists) bring the Cube Monsters' AI director with theirs.
        UObject* PlaylistAISettings = UFortPlaylistAthena::HasAISettings() ? static_cast<UObject*>(Playlist->AISettings) : nullptr;
        if (PlaylistAISettings && GameMode && AFortGameModeAthena::HasAISettings())
        {
            Log::Info(L"PLAYLIST: {} has AI settings ({}), the game mode takes them", static_cast<FName>(Playlist->PlaylistName).ToStdString(),
                PlaylistAISettings->GetName().ToStdString());
            GameMode->SetAISettings(PlaylistAISettings);
        }
        // 0xBD2710 and LoadCurrentPlaylistData are what OnRep_CurrentPlaylistData does (5.21 0xBD7770, 5.30 0xBEC6C0),
        // whose "(Client Only)" log shows on the server. 6.01's is OnRep_CurrentPlaylistInfo (0xC1DFF0), the same steps.
        UFunction* OnRep = AFortGameStateAthena::FindStaticFunction_OnRep_CurrentPlaylistData();
        if (!OnRep)
        {
            OnRep = AFortGameStateAthena::FindStaticFunction_OnRep_CurrentPlaylistInfo();
        }
        if (OnRep)
        {
            Reflection::FParams Params(OnRep);
            Params.Call(GameState);
        }
        else
        {
            Log::Error(L"PLAYLIST: AFortGameStateAthena has neither OnRep_CurrentPlaylistData nor OnRep_CurrentPlaylistInfo on this build, so {} doesn't load",
                static_cast<FName>(Playlist->PlaylistName).ToStdString());
        }
        GameState->StreamAdditionalPlaylistLevels();
    }
#endif
}

UFortPlaylistAthena* AFortGameStateAthena::GetCurrentPlaylist() const
{
    if (Member_CurrentPlaylistData.IsAvailable())
    {
        return CurrentPlaylistData;
    }
    if (HasCurrentPlaylistInfo())
    {
        return static_cast<FPlaylistPropertyArray&>(CurrentPlaylistInfo).GetCurrentPlaylist();
    }
    return nullptr;
}

bool AFortGameStateAthena::SetCurrentPlaylist(UFortPlaylistAthena* Playlist)
{
#if WITH_SERVER_CODE
    if (Member_CurrentPlaylistData.IsAvailable())
    {
        CurrentPlaylistData = Playlist;
        return true;
    }
    if (!HasCurrentPlaylistInfo())
    {
        return false;
    }
    FPlaylistPropertyArray& Info = CurrentPlaylistInfo;
    Info.BasePlaylist = Playlist;
    Info.OverridePlaylist = nullptr;
    // The match's overrides: what its party chose for the playlist's user options, which the game mode's
    // OnGivenMatchAssignment left in MatchOptions before it gave the game state the playlist. They have to be in the
    // array before the playlist's modifiers register (UpdatePlaylistDependentData, once its assets have loaded), as each
    // mutator takes the ones of its scope when it spawns.
    Info.SetPropertyOverrides(Playlist, GetMatchOptions());
    const ptrdiff_t KeyOffset = FPlaylistPropertyArray::PlaylistReplicationKeyOffset();
    if (KeyOffset >= 0)
    {
        ++*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&Info) + KeyOffset);
    }
    else
    {
        static bool bLogged = false;
        if (!bLogged)
        {
            bLogged = true;
            Log::Warning(L"PLAYLIST: FPlaylistPropertyArray's PlaylistReplicationKey isn't where 6.01 has it, so a playlist set after a player joined reaches only the players who join later");
        }
    }
    return true;
#else
    return false;
#endif
}

void AFortGameStateAthena::SetCurrentPlaylistId(int32_t PlaylistId)
{
#if WITH_SERVER_CODE
    if (!HasCurrentPlaylist())
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
    UFortPlaylistAthena* Current = GetCurrentPlaylist();
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
    if (!HasCurrentPlaylist() || PlaylistName.IsNone())
    {
        return;
    }
    // 5.21's copy of the name's (0xBD5B68): unless the game state has the playlist of that name already, loaded or
    // loading, the game mode's playlist manager's of that name becomes its playlist, and its id the game state's and the
    // game mode's CurrentPlaylistId.
    UFortPlaylistAthena* Current = GetCurrentPlaylist();
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

void AFortGameStateAthena::ReportPropertyOverrides() const
{
#if WITH_SERVER_CODE
    if (!HasCurrentPlaylistInfo() || GameplayMutatorsOffset() < 0 || !FPropertyOverride::IsAvailable())
    {
        return;
    }
    const FPlaylistPropertyArray& Info = CurrentPlaylistInfo;
    const TStructArray<FPropertyOverride>& Overrides = Info.PropertyOverrides;
    const TArray<UObject*>& Mutators = GameplayMutators;
    for (int32_t Index = 0; Index < Overrides.Num(); ++Index)
    {
        const FPropertyOverride& Override = Overrides[Index];
        const FString& Scope = Override.PropertyScope;
        const FString& PropertyName = Override.PropertyName;
        const FString& PropertyData = Override.PropertyData;
        // An object's scope as FPlaylistPropertyArray has it (6.01 0xEF1FF0): the short name of its class's package.
        UObject* Taker = nullptr;
        for (int32_t MutatorIndex = 0; MutatorIndex < Mutators.Num() && !Taker; ++MutatorIndex)
        {
            UObject* Mutator = Mutators[MutatorIndex];
            UObject* Package = Mutator ? Mutator->GetClass()->GetOutermost() : nullptr;
            const std::wstring PackageName = Package ? Package->GetName().ToStdString() : std::wstring();
            const size_t Slash = PackageName.rfind(L'/');
            if (Package && _wcsicmp(PackageName.c_str() + (Slash == std::wstring::npos ? 0 : Slash + 1), *Scope) == 0)
            {
                Taker = Mutator;
            }
        }
        if (!Taker)
        {
            Log::Warning(L"PLAYLIST: property override {}:{} has no mutator of its scope among the game state's {}, so nothing took it", Scope.ToStdString(), PropertyName.ToStdString(),
                Mutators.Num());
            continue;
        }
        const UProperty* Property = Taker->GetClass()->FindPropertyByName(FName(*PropertyName, FNAME_Find));
        const uint8_t* Value = Property ? reinterpret_cast<const uint8_t*>(Taker) + Property->GetOffset_ForInternal() : nullptr;
        std::wstring ValueText = L"not a property of its class";
        if (Value && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UFloatProperty))
        {
            ValueText = std::format(L"{:.2f}", *reinterpret_cast<const float*>(Value));
        }
        else if (Value && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UIntProperty))
        {
            ValueText = std::to_wstring(*reinterpret_cast<const int32_t*>(Value));
        }
        else if (Value && Property->ElementSize == 1)
        {
            ValueText = std::to_wstring(static_cast<int32_t>(*Value));
        }
        else if (Value)
        {
            ValueText = L"of a type Core's log doesn't read";
        }
        Log::Info(L"PLAYLIST: {}'s {} is {} (the match's property override: {})", Taker->GetName().ToStdString(), PropertyName.ToStdString(), ValueText, PropertyData.ToStdString());
    }
#endif
}

void AFortGameStateAthena::StreamAdditionalPlaylistLevels()
{
#if WITH_SERVER_CODE
    UFortPlaylistAthena* Playlist = HasCurrentPlaylist() && UFortPlaylist::HasAdditionalLevels() && Role == ROLE_Authority ? GetCurrentPlaylist() : nullptr;
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
