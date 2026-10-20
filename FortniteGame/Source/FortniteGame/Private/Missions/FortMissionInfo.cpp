#include "pch.h"
#include "Missions/FortMissionInfo.h"
#include "Finder.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

#include <new>
#include <vector>

namespace
{
    constexpr size_t Align(size_t Value, size_t Alignment)
    {
        return (Value + Alignment - 1) & ~(Alignment - 1);
    }
}

void UFortMissionInfo::GenerateMissionDifficultyOptions(FFortGeneratedDifficultyOptions& OutOptions, void* RandomStream, float GameDifficulty, AGameModeBase* GameMode, UObject* GenerationData)
{
    if (!Natives::UFortMissionInfo_GenerateMissionDifficultyOptions && !Finder::ResolveMissionGenerationLive())
    {
        return;
    }

    UScriptStruct* OptionsStruct = FFortGeneratedDifficultyOptions::StaticStruct();
    const size_t OptionsSize = static_cast<size_t>(OptionsStruct->GetStructureSize());
    const size_t DifficultyOffset = Align(OptionsSize, alignof(float));
    const size_t ForceOffset = DifficultyOffset + sizeof(float);
    const size_t GameModeOffset = Align(ForceOffset + sizeof(bool), alignof(FWeakObjectPtr));
    const size_t DataOffset = Align(GameModeOffset + sizeof(FWeakObjectPtr), alignof(UObject*));

    std::vector<uint8_t> Params(DataOffset + sizeof(UObject*), 0);
    *reinterpret_cast<float*>(Params.data() + DifficultyOffset) = GameDifficulty;
    *reinterpret_cast<bool*>(Params.data() + ForceOffset) = true;
    new (Params.data() + GameModeOffset) FWeakObjectPtr(reinterpret_cast<UObject*>(GameMode));
    *reinterpret_cast<UObject**>(Params.data() + DataOffset) = GenerationData;

    std::vector<uint8_t> Result(OptionsSize, 0);
    Natives::UFortMissionInfo_GenerateMissionDifficultyOptions(this, Result.data(), RandomStream, Params.data());
    OptionsStruct->CopyScriptStruct(&OutOptions, Result.data());
    OptionsStruct->DestroyStruct(Result.data());
    OptionsStruct->DestroyStruct(Params.data());
}
