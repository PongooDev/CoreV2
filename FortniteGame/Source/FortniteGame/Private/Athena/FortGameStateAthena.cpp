#include "pch.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Athena/Mutators/FortAthenaMutator.h"
#include "Athena/Mutators/FortAthenaMutator_AI.h"
#include "Athena/Mutators/FortMutatorListComponent.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Hooks.h"
#include "Log.h"
#include "Misc/StringAssetReference.h"
#include "Natives.h"
#include "Pawns/FortMovementComp_Character.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/AssetPtr.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

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
            Log::Info(L"PLAYLIST: {} has AI settings ({}), the game mode takes them", Playlist->PlaylistNameText(),
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
                Playlist->PlaylistNameText());
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
            Current ? Current->PlaylistNameText() : std::wstring(L"none"));
        return;
    }
    Log::Info(L"PLAYLIST: Playlist Object found in AFortGameStateAthena::SetCurrentPlaylistId() PlaylistName is {} (Server Only)", Playlist->PlaylistNameText());
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
    if (Current && Current->IsPlaylistNamed(PlaylistName) && IsPlaylistLoadedOrLoading(this))
    {
        return;
    }
    AFortGameModeAthena* GameMode = AthenaGameModeOf(this);
    UFortPlaylistManager* Playlists = GameMode ? static_cast<UFortPlaylistManager*>(GameMode->PlaylistManager) : nullptr;
    UFortPlaylistAthena* Playlist = Playlists ? Playlists->GetPlaylist(PlaylistName) : nullptr;
    if (!Playlist)
    {
        Log::Warning(L"PLAYLIST: {} isn't one of the game mode's playlist manager's, so the game state's stays {}", PlaylistName.ToStdString(),
            Current ? Current->PlaylistNameText() : std::wstring(L"none"));
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

TArray<UObject*>* AFortGameStateAthena::GetGameplayMutatorList() const
{
    if (GameplayMutatorsOffset() >= 0)
    {
        return &GetMember_GameplayMutators();
    }
    UFortMutatorListComponent* List = Member_MutatorListComponent.IsAvailable() && UFortMutatorListComponent::HasMutators() ? MutatorListComponent : nullptr;
    return List ? &List->GetMember_Mutators() : nullptr;
}

bool AFortGameStateAthena::ApplyMutatorHealthAndShield(AController* Controller)
{
#if WITH_SERVER_CODE
    return Controller && Finder::ResolveRestartHealthAndShieldLive() && Natives::AFortGameStateAthena_ApplyMutatorHealthAndShield(this, Controller);
#else
    return false;
#endif
}

bool AFortGameStateAthena::GetMutatorGravityData(FGravityMovementData& OutData) const
{
    // As 0xD4D100: each active mutator of the list in turn, the first answer kept. Only the gravity mutator answers in
    // the builds checked (AFortAthenaMutator's own slot is the shared empty stub, 0x2B1CB0).
    const TArray<UObject*>* MutatorList = AFortAthenaMutator_GravityMultiplier::IsAvailable() ? GetGameplayMutatorList() : nullptr;
    if (!MutatorList)
    {
        return false;
    }
    UClass* const GravityClass = AFortAthenaMutator_GravityMultiplier::StaticClass();
    const TArray<UObject*>& Mutators = *MutatorList;
    for (int32_t Index = 0; Index < Mutators.Num(); ++Index)
    {
        UObject* const Object = Mutators[Index];
        if (!Object || !Object->IsA(GravityClass))
        {
            continue;
        }
        const AFortAthenaMutator_GravityMultiplier* Mutator = reinterpret_cast<const AFortAthenaMutator_GravityMultiplier*>(Object);
        if (AFortAthenaMutator::HasMutatorActive() && !Mutator->bMutatorActive)
        {
            continue;
        }
        if (Mutator->GetGravityData(OutData))
        {
            return true;
        }
    }
    return false;
}

void AFortGameStateAthena::ReportPropertyOverrides() const
{
#if WITH_SERVER_CODE
    const TArray<UObject*>* MutatorList = GetGameplayMutatorList();
    if (!HasCurrentPlaylistInfo() || !MutatorList || !FPropertyOverride::IsAvailable())
    {
        return;
    }
    const FPlaylistPropertyArray& Info = CurrentPlaylistInfo;
    const TStructArray<FPropertyOverride>& Overrides = Info.PropertyOverrides;
    const TArray<UObject*>& Mutators = *MutatorList;
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

void AFortGameStateAthena::AddPlayerStateToGameMemberInfo(AFortPlayerStateAthena* PlayerState)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !PlayerState || !HasGameMemberInfoArray() || !AFortPlayerStateAthena::HasSquadId())
    {
        return;
    }
    const FUniqueNetIdRepl& UniqueId = PlayerState->UniqueId;
    const uint8_t SquadId = PlayerState->SquadId;
    const uint8_t TeamIndex = static_cast<uint8_t>(PlayerState->TeamIndex);
    // A player not placed yet: squad 0 is none, and the clients' maps would file them under it.
    if (!UniqueId.IsValid() || SquadId == 0)
    {
        return;
    }

    FGameMemberInfoArray& MemberInfo = GameMemberInfoArray;
    TStructArray<FGameMemberInfo>& Members = MemberInfo.Members;
    const std::wstring Id = UniqueId.ToString().ToStdString();
    bool bRemoved = false;
    for (int32_t Index = Members.Num() - 1; Index >= 0; --Index)
    {
        FGameMemberInfo& Member = Members[Index];
        const FUniqueNetIdRepl& MemberUniqueId = Member.MemberUniqueId;
        if (MemberUniqueId.ToString().ToStdString() != Id)
        {
            continue;
        }
        if (Member.SquadId == SquadId && Member.TeamIndex == TeamIndex)
        {
            return;
        }
        Members.RemoveAt(Index);
        bRemoved = true;
    }
    if (bRemoved)
    {
        MemberInfo.MarkArrayDirty();
    }

    FGameMemberInfo& Member = Members.AddDefaulted();
    Member.SquadId = SquadId;
    Member.TeamIndex = TeamIndex;
    FUniqueNetIdRepl::StaticStruct()->CopyScriptStruct(&Member.MemberUniqueId, &UniqueId);
    MemberInfo.MarkItemDirty(Member);
    ForceNetUpdate();
    Log::Info(L"Battle Royale: {} is a game member in squad {} of team {} ({} members)", PlayerState->GetPlayerName().ToStdString(), SquadId, TeamIndex, Members.Num());
#endif
}

bool AFortGameStateAthena::HasStreamedAdditionalPlaylistLevels() const
{
    UFortPlaylistAthena* Playlist = HasCurrentPlaylist() && UFortPlaylist::HasAdditionalLevels() && HasAdditionalPlaylistLevelsStreamed() ? GetCurrentPlaylist() : nullptr;
    if (!Playlist)
    {
        return true;
    }
    // Counted, not matched by name: the name each streamed level is listed under is the game's to choose.
    const TArray<TAssetPtr<UWorld>>& Levels = Playlist->AdditionalLevels;
    int32_t Wanted = 0;
    for (int32_t Index = 0; Index < Levels.Num(); ++Index)
    {
        Wanted += SoftElementAt(Levels, Index).ToStringReference().IsValid() ? 1 : 0;
    }
    const TArray<FName>& Streamed = AdditionalPlaylistLevelsStreamed;
    return Streamed.Num() >= Wanted;
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
            Playlist->PlaylistNameText(), Levels.Num());
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

void AFortGameStateAthena::OnPostEarnedKill(AFortPlayerStateAthena* KillerPlayerState)
{
#if WITH_SERVER_CODE
    const TArray<UObject*>* MutatorList = GetGameplayMutatorList();
    if (!KillerPlayerState || Role != ROLE_Authority || !MutatorList || !AFortAthenaMutator::HasOnPostEarnedKill())
    {
        return;
    }
    // 6.31 hands the mutators the killer's controller. A player bot's is a stock AAIController, which the TDM mutator's
    // override can't read a team from, so Team Rumble's mutator scores a bot's kill by Core's copy of it.
    AFortPlayerController* const KillerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(KillerPlayerState->GetOwner()));
    UClass* const TdmClass = AFortAthenaMutator_TDM::StaticClass();
    // Each active mutator of the list, as the game state's other loops over it ask them (OnPlaylistTimerTicked, 6.31
    // 0xCAF640: skip a null one or one whose IsMutatorActive is false). A copy, as a mutator's answer may change the list.
    const TArray<UObject*> Mutators = *MutatorList;
    UClass* const MutatorClass = AFortAthenaMutator::StaticClass();
    bool bTeamScores = false;
    for (int32_t Index = 0; Index < Mutators.Num(); ++Index)
    {
        AFortAthenaMutator* Mutator = Mutators[Index] && Mutators[Index]->IsA(MutatorClass) ? reinterpret_cast<AFortAthenaMutator*>(Mutators[Index]) : nullptr;
        if (!Mutator || (AFortAthenaMutator::HasMutatorActive() && !Mutator->bMutatorActive))
        {
            continue;
        }
        bTeamScores = bTeamScores || (TdmClass && Mutators[Index]->IsA(TdmClass));
        if (KillerController)
        {
            Mutator->FortGameStateAthena_OnPostEarnedKill(KillerController);
        }
        else if (TdmClass && Mutators[Index]->IsA(TdmClass))
        {
            reinterpret_cast<AFortAthenaMutator_TDM*>(Mutator)->ScoreBotKill(this, KillerPlayerState);
        }
    }
    // Team Rumble's two scores, as its HUD shows them (team 2's, the other team's, of the eliminations that win). Nothing to
    // say in a match without its mutator.
    if (!bTeamScores)
    {
        return;
    }
    Log::Info(L"OnPostEarnedKill: {} (team {}) earned a kill; the mutators' team scores are {} and {} of {}", KillerPlayerState->GetPlayerName().ToStdString(),
        static_cast<int32_t>(static_cast<uint8_t>(KillerPlayerState->TeamIndex)), static_cast<int32_t>(MutatorGenericInt_0), static_cast<int32_t>(MutatorGenericInt_1),
        static_cast<int32_t>(MutatorGenericInt_2));
    // And the game mode's own ranking of that score, once its Tick has run (the best team's score and team).
    UWorld* World = HasCurrentHighScore() ? GetWorld() : nullptr;
    if (World)
    {
        const TWeakObjectPtr<AFortGameStateAthena> WeakThis(this);
        World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]
        {
            if (AFortGameStateAthena* GameState = WeakThis.Get())
            {
                Log::Info(L"OnPostEarnedKill: the game mode's Tick has team {} best, with {}", static_cast<int32_t>(GameState->CurrentHighScoreTeam), static_cast<int32_t>(GameState->CurrentHighScore));
            }
        }));
    }
#endif
}

void AFortGameStateAthena::HookCallSites()
{
#if WITH_SERVER_CODE
    // The Cube Monsters' movement (6.20 to 6.22): AFortGameStateAthena::AddManagedCharacterMovementComponent (6.22
    // 0xC6F230) asks the active AI mutator to take the component and adds it to AFortGameState's
    // ManagedCharMovementComponents when the mutator says no; the mutator's FortGameStateAthena_AddManagedCharacter-
    // MovementComponent always said no (the shared `xor al, al; ret`), so AFortAthenaMutator_AI's DeimosMoveComponents
    // stayed empty and its batched movement (FortGameStateAthena_TickActor) never moved one. Both calls go to Core's, or
    // neither: a component the mutator took must also come out of its list.
    UClass* Class = StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    if (!Default || !AFortAthenaMutator_AI::HasDeimosMoveComponents() || !UFortMovementComp_AIChar::HasDeimos())
    {
        return;
    }
    Finder::FManagedCharacterMovementSites Sites;
    if (!Finder::ResolveManagedCharacterMovementLive(*reinterpret_cast<void***>(Default), Sites))
    {
        return;
    }
    if (!Sites.AddMutatorCall || !Sites.RemoveMutatorCall)
    {
        Log::Info(L"Finder: AFortAthenaMutator_AI's character movement hand-over is {} on this build, left as it is", !Sites.AddMutatorCall && !Sites.RemoveMutatorCall ? L"the build's own" : L"only half stripped");
        return;
    }
    Natives::UFortMovementComp_Character_ManagedTickIndex = Sites.ManagedTickIndexOffset;
    Hooks::RedirectCall(L"AFortGameStateAthena::AddManagedCharacterMovementComponent -> AFortAthenaMutator_AI::FortGameStateAthena_AddManagedCharacterMovementComponent", Sites.AddMutatorCall,
        reinterpret_cast<void*>(&AFortAthenaMutator_AI::AddManagedCharacterMovementComponentHK));
    Hooks::RedirectCall(L"AFortGameStateAthena::RemoveManagedCharacterMovementComponent -> AFortAthenaMutator_AI::FortGameStateAthena_RemoveManagedCharacterMovementComponent", Sites.RemoveMutatorCall,
        reinterpret_cast<void*>(&AFortAthenaMutator_AI::RemoveManagedCharacterMovementComponentHK));
#endif
}
