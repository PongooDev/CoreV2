#pragma once

#include "Containers/UnrealString.h"
#include "Math/Color.h"
#include "UObject/Object.h"

struct FAutoCompleteCommand
{
    FString Command;
    FString Desc;
    FColor Color;

    FAutoCompleteCommand()
        : Color(180, 180, 180)
    {
    }
};

static_assert(sizeof(FAutoCompleteCommand) == 0x28);

class UConsoleSettings : public UObject
{
    DefineUnrealClass(UConsoleSettings, EngineSettings)

public:
    DefineUProperty(int32_t, MaxScrollbackSize)
    DefineUProperty(TArray<FAutoCompleteCommand>, ManualAutoCompleteList)
    DefineUProperty(TArray<FString>, AutoCompleteMapPaths)
    DefineUProperty(float, BackgroundOpacityPercentage)
    DefineUProperty(bool, bOrderTopToBottom)
    DefineUProperty(FColor, InputColor)
    DefineUProperty(FColor, HistoryColor)
    DefineUProperty(FColor, AutoCompleteCommandColor)
    DefineUProperty(FColor, AutoCompleteCVarColor)
    DefineUProperty(FColor, AutoCompleteFadedColor)
};
