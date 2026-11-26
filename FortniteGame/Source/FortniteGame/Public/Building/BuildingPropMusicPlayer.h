#pragma once

#include "Building/BuildingSMActor.h"
#include "UObject/AssetPtr.h"
#include "UObject/Stack.h"

enum class EMusicTrackPlayback : uint8_t
{
    Disabled,
    Enabled,
};

struct FMusicPlayerData
{
    int32_t SongIndex = -1;
    float ServerTimeSongStarted = 0.0f;
};

static_assert(sizeof(FMusicPlayerData) == 0x8);

// A prop that plays a track from MusicTrackTable (7.40 on): Creative's radio, BP_CreativeRadio (an
// ABuildingPropPlaygroundMusicPlayer). BeginPlay (intact) fills MusicArray from the table's enabled rows; the clients play
// MusicArray[PlayingSongData.SongIndex] from OnRep_PlayingSongData (intact), and stop at INDEX_NONE. The radio's options
// write SavedTrackName (the track's asset name) and PlaybackType on the server, and the Blueprint's OnPropertyChanged
// (bound to its options component's OnAnyPropertyChanged and OnPropertiesLoaded) calls StartSongForOptionsChange.
class ABuildingPropMusicPlayer : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingPropMusicPlayer, FortniteGame)
    DefineUFunction(StartSongForOptionsChange)

public:
    DefineUProperty(TArray<TAssetPtr<UObject>>, MusicArray)
    DefineUProperty(FMusicPlayerData, PlayingSongData)
    DefineUProperty(FString, SavedTrackName)
    DefineUProperty(EMusicTrackPlayback, PlaybackType)

    void StartSongForOptionsChange();

    DECLARE_FUNCTION(execStartSongForOptionsChange);

    static void HookNatives();
};
