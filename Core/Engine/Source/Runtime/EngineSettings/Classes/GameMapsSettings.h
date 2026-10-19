#pragma once

#include "Containers/EnumAsByte.h"
#include "Misc/StringAssetReference.h"
#include "Misc/StringClassReference.h"
#include "UObject/Object.h"

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

    static void Hook();

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
