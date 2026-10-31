#pragma once

#include "Containers/EnumAsByte.h"
#include "Misc/StringAssetReference.h"
#include "Misc/StringClassReference.h"
#include "UObject/Object.h"

#include <functional>

namespace ETwoPlayerSplitScreenType
{
    enum Type
    {
        Horizontal,
        Vertical,
    };
}

namespace EThreePlayerSplitScreenType
{
    enum Type
    {
        FavorTop,
        FavorBottom,
    };
}

struct FGameModeName
{
    FString Name;
    FStringClassReference GameMode;
};

class UGameMapsSettings : public UObject
{
    DefineUnrealClass(UGameMapsSettings, EngineSettings)

public:
    static const FString GetGameDefaultMap();
    static const FString& GetGlobalDefaultGameMode();
    // 1.10's (UE 4.19) returns the path by value, built from the FSoftClassPath.
    static FString* GetGlobalDefaultGameModeByValue(FString* Result);
    // The global default game mode's path, read in either layout.
    static FString GlobalDefaultGameModePath();

    static void Hook();

    // The map URL a server of Core's -CoreMode starts on (its game's own data names it), set at boot; -CoreMap wins.
    static inline std::function<FString()> ModeDefaultMap;

    DefineUProperty(FStringAssetReference, EditorStartupMap)
    DefineUProperty(FString, LocalMapOptions)
    DefineUProperty(FStringAssetReference, TransitionMap)
    DefineUProperty(bool, bUseSplitscreen)
    DefineUProperty(TEnumAsByte<ETwoPlayerSplitScreenType::Type>, TwoPlayerSplitscreenLayout)
    DefineUProperty(TEnumAsByte<EThreePlayerSplitScreenType::Type>, ThreePlayerSplitscreenLayout)
    DefineUProperty(bool, bOffsetPlayerGamepadIds)
    DefineUProperty(FStringClassReference, GameInstanceClass)
    DefineUProperty(FStringAssetReference, GameDefaultMap)
    DefineUProperty(FStringAssetReference, ServerDefaultMap)
    DefineUProperty(FStringClassReference, GlobalDefaultGameMode)
    DefineUProperty(FStringClassReference, GlobalDefaultServerGameMode)
    DefineUProperty(TArray<FGameModeName>, GameModeMapPrefixes)
    DefineUProperty(TArray<FGameModeName>, GameModeClassAliases)
};
