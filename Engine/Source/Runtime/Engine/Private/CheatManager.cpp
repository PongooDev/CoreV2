#include "pch.h"
#include "GameFramework/CheatManager.h"
#include "Finder.h"
#include "GameFramework/PlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"

APlayerController* UCheatManager::GetOuterAPlayerController() const
{
    return Cast<APlayerController>(GetOuter());
}

void UCheatManager::CheatScript(const FString& ScriptName)
{
    Reflection::FParams Params(StaticFunction_CheatScript());
    Params.Set(L"ScriptName", ScriptName);
    Params.Call(this);
}

void UCheatManager::Teleport()
{
    // The exec function's virtual call gives the slot; UCheatManager's class default object has the engine's body in it.
    const int32_t Index = Finder::ImplementationIndex(FindStaticFunction_Teleport());
    UObject* const Default = StaticClass()->GetDefaultObject(false);
    void* const* const VTable = Default ? *reinterpret_cast<void* const* const*>(Default) : nullptr;
    if (Index < 0 || !VTable)
    {
        return;
    }
    reinterpret_cast<void (*)(UCheatManager*)>(VTable[Index])(this);
}
