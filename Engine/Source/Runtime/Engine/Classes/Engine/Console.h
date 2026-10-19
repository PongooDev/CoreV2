#pragma once

#include "ConsoleSettings.h"
#include "Engine/EngineBaseTypes.h"
#include "InputCoreTypes.h"
#include "HAL/UnrealMemory.h"
#include "Misc/CoreMiscDefines.h"
#include "Misc/OutputDevice.h"
#include "ObjectLayout.h"
#include "UObject/Object.h"

class ULocalPlayer;
class UTexture2D;

struct FAutoCompleteNode
{
    int32_t IndexChar;
    TArray<int32_t> AutoCompleteListIndices;
    TArray<FAutoCompleteNode*> ChildNodes;

    FAutoCompleteNode()
        : IndexChar(INDEX_NONE)
    {
    }

    FAutoCompleteNode(int32_t NewChar)
        : IndexChar(NewChar)
    {
    }

    ~FAutoCompleteNode()
    {
        for (int32_t Idx = 0; Idx < ChildNodes.Num(); Idx++)
        {
            delete ChildNodes[Idx];
        }
        ChildNodes.Empty();
    }

    static void* operator new(size_t Size)
    {
        return FMemory::Malloc(Size);
    }

    static void operator delete(void* Pointer)
    {
        FMemory::Free(Pointer);
    }
};

static_assert(sizeof(FAutoCompleteNode) == 0x28);

class UConsole : public UObject, public FOutputDevice
{
    DefineUnrealClass(UConsole, Engine)

public:
    DefineUProperty(ULocalPlayer*, ConsoleTargetPlayer)
    DefineUProperty(UTexture2D*, DefaultTexture_Black)
    DefineUProperty(UTexture2D*, DefaultTexture_White)
    DefineUProperty(TArray<FString>, HistoryBuffer)
    DefineNativeMember(TArray<FAutoCompleteCommand>, AutoCompleteList, UConsole_AutoCompleteList)
    DefineNativeBitfield(bIsRuntimeAutoCompleteUpToDate, UConsole_bIsRuntimeAutoCompleteUpToDate, 1)
    DefineNativeMember(FAutoCompleteNode, AutoCompleteTree, UConsole_AutoCompleteTree)
    DefineNativeMember(const UConsoleSettings*, ConsoleSettings, UConsole_ConsoleSettings)

    void ConsoleCommand(const FString& Command);
    void BuildRuntimeAutoCompleteList(bool bForce = false);
    bool InputKey(int32_t ControllerId, FKey Key, EInputEvent Event, float AmountDepressed = 1.f, bool bGamepad = false);

    struct FRegisterConsoleAutoCompleteEntriesGlobal
    {
        void Broadcast(TArray<FAutoCompleteCommand>& AutoCompleteList) const;
    };

    static constexpr FRegisterConsoleAutoCompleteEntriesGlobal RegisterConsoleAutoCompleteEntries{};

    static void Hook();

private:
    static void BuildRuntimeAutoCompleteListHK(UConsole* This, bool bForce);
};
