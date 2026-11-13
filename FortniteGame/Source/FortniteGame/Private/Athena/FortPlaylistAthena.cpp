#include "pch.h"
#include "Athena/FortPlaylistAthena.h"
#include "FortPlaylistUserOptions.h"
#include "Log.h"

#include <cwchar>

bool UFortPlaylist::IsShowdownPlaylist() const
{
    // CL-4480234 0x11A7750: the playlist name against the three, without case (towlower), each its EFortShowdownMatchType.
    if (!Member_PlaylistName.IsAvailable())
    {
        return false;
    }
    const std::wstring Name = static_cast<FName>(PlaylistName).ToStdString();
    for (const wchar_t* Showdown : { L"Playlist_Showdown_Solo", L"Playlist_Showdown_Duos", L"Playlist_Showdown_Squads" })
    {
        if (_wcsicmp(Name.c_str(), Showdown) == 0)
        {
            return true;
        }
    }
    return false;
}

int32_t FPlaylistPropertyArray::SetPropertyOverrides(const UFortPlaylistAthena* Playlist, const TMap<FString, FString>* MatchOptions)
{
    int32_t Set = 0;
#if WITH_SERVER_CODE
    if (!Member_PropertyOverrides.IsAvailable() || !FPropertyOverride::IsAvailable())
    {
        return 0;
    }
    // The overrides are the match's: a playlist that takes another's place starts with none of the last one's.
    TStructArray<FPropertyOverride>& Overrides = PropertyOverrides;
    const bool bHadOverrides = Overrides.Num() > 0;
    Overrides.Empty();

    UPlaylistUserOptions* UserOptions = Playlist && MatchOptions && MatchOptions->Num() > 0 && UFortPlaylist::HasUserOptions() ? static_cast<UPlaylistUserOptions*>(Playlist->UserOptions) : nullptr;
    const TArray<UPlaylistUserOptionBase*>* Options = UserOptions ? &static_cast<TArray<UPlaylistUserOptionBase*>&>(UserOptions->Options) : nullptr;
    const std::wstring PlaylistText = Playlist ? static_cast<FName>(Playlist->PlaylistName).ToStdString() : std::wstring(L"none");
    for (int32_t Index = 0; Options && Index < Options->Num(); ++Index)
    {
        // The key the lobby sent the option's value under, and so the one AFortGameMode::OnGivenMatchAssignment gave the
        // game state's MatchOptions (CL-4424678 0xE4C2A0). The game compares its strings without their case.
        UPlaylistUserOptionBase* Option = (*Options)[Index];
        const FString ScopedKey = Option ? Option->GetOptionKey(true) : FString();
        const FString* Value = nullptr;
        for (const TPair<FString, FString>& MatchOption : *MatchOptions)
        {
            if (!ScopedKey.IsEmpty() && _wcsicmp(*MatchOption.Key, *ScopedKey) == 0)
            {
                Value = &MatchOption.Value;
                break;
            }
        }
        if (!Value || Value->IsEmpty())
        {
            continue;
        }
        // "<scope>:<OptionKey>" (0xEF0A60). An option without a scope is one of the playlist's own properties, which the
        // build changes on a copy of the playlist (OverridePlaylist); 6.01's only options are its Creative mutators'.
        const FString PropertyName = Option->GetOptionKey(false);
        const int32_t ScopeLength = ScopedKey.Len() - PropertyName.Len() - 1;
        const wchar_t* const ScopedText = *ScopedKey;
        if (PropertyName.IsEmpty() || ScopeLength <= 0 || ScopedText[ScopeLength] != L':' || _wcsicmp(ScopedText + ScopeLength + 1, *PropertyName) != 0)
        {
            Log::Warning(L"PLAYLIST: Unknown property override {} ({}'s {} has no scope: a property of the playlist itself, which Core doesn't override)", ScopedKey.ToStdString(), PlaylistText,
                Option->GetName().ToStdString());
            continue;
        }

        FPropertyOverride& Override = Overrides.AddDefaulted();
        Override.PropertyScope = ScopedKey.Left(ScopeLength);
        Override.PropertyName = PropertyName;
        Override.PropertyData = *Value;
        MarkItemDirty(Override);
        ++Set;
        Log::Info(L"PLAYLIST: Applied property override {} - {}", ScopedKey.ToStdString(), Value->ToStdString());
    }
    // Emptied and not filled again: the clients' copies of the old items go with the array's next replication.
    if (bHadOverrides && Set == 0)
    {
        MarkArrayDirty();
    }
    if (Set > 0)
    {
        Log::Info(L"PLAYLIST: {} takes {} of its {} user options from the match's options", PlaylistText, Set, Options->Num());
    }
#endif
    return Set;
}
