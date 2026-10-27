#include "pch.h"
#include "Engine/Console.h"
#include "Delegates/Delegate.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Math/NumericLimits.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Natives.h"
#include "ShowFlags.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include <utility>

namespace
{
    // UConsole::ConsoleCommand runs a line on ConsoleTargetPlayer's controller or, with none (only splitscreen sets it, so
    // always in Fortnite), on the world's first player controller (Cert 0x22D1B80, 1.7.2 0x24731A0). Core read only the
    // former, so the client never got its cheat manager and every local cheat was "not recognized" (#75).
    APlayerController* ConsolePlayerController(UConsole* Console)
    {
        ULocalPlayer* Player = Console->ConsoleTargetPlayer;
        if (!Player)
        {
            UGameViewportClient* const Viewport = Cast<UGameViewportClient>(Console->GetOuter());
            UGameInstance* const GameInstance = Viewport ? static_cast<UGameInstance*>(Viewport->GameInstance) : nullptr;
            const TArray<ULocalPlayer*>* const LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
            Player = LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
        }
        return Player ? static_cast<APlayerController*>(Player->PlayerController) : nullptr;
    }
}

void UConsole::FRegisterConsoleAutoCompleteEntriesGlobal::Broadcast(TArray<FAutoCompleteCommand>& AutoCompleteList) const
{
    static_cast<const TMulticastDelegate<TArray<FAutoCompleteCommand>&>*>(Natives::UConsole_RegisterConsoleAutoCompleteEntries)->Broadcast(AutoCompleteList);
}

void UConsole::ConsoleCommand(const FString& Command)
{
    using FConsoleCommand = void (*)(UConsole* This, const FString& Command);
    reinterpret_cast<FConsoleCommand>((*reinterpret_cast<void* const* const*>(this))[Natives::UConsole_ConsoleCommandIndex])(this, Command);
}

bool UConsole::InputKey(int32_t ControllerId, FKey Key, EInputEvent Event, float AmountDepressed, bool bGamepad)
{
    using FInputKey = bool (*)(UConsole* This, int32_t ControllerId, FKey Key, EInputEvent Event, float AmountDepressed, bool bGamepad);
    return reinterpret_cast<FInputKey>((*reinterpret_cast<void* const* const*>(this))[Natives::UConsole_InputKeyIndex])(this, ControllerId, Key, Event, AmountDepressed, bGamepad);
}

class FConsoleVariableAutoCompleteVisitor
{
public:
    static void OnConsoleVariable(const wchar_t* Name, IConsoleObject* CVar, TArray<FAutoCompleteCommand>& Sink)
    {
#if (UE_BUILD_SHIPPING || UE_BUILD_TEST)
        if (CVar->TestFlags(ECVF_Cheat))
        {
            return;
        }
#endif
        if (CVar->TestFlags(ECVF_Unregistered))
        {
            return;
        }

        const UConsoleSettings* ConsoleSettings = GetDefault<UConsoleSettings>();

        const int32_t NewIdx = Sink.AddZeroed(1);
        FAutoCompleteCommand& Cmd = Sink[NewIdx];
        Cmd.Command = Name;

        IConsoleVariable* CVariable = CVar->AsVariable();
        if (CVariable)
        {
            if (CVar->TestFlags(ECVF_ReadOnly))
            {
                Cmd.Color = ConsoleSettings->AutoCompleteFadedColor;
            }
            else
            {
                Cmd.Color = ConsoleSettings->AutoCompleteCVarColor;
            }
        }
        else
        {
            Cmd.Color = ConsoleSettings->AutoCompleteCommandColor;
        }
    }
};

void UConsole::BuildRuntimeAutoCompleteList(bool bForce)
{
#if ALLOW_CONSOLE
    if (!bForce)
    {
        bIsRuntimeAutoCompleteUpToDate = false;
        return;
    }

    for (int32_t Idx = 0; Idx < AutoCompleteTree.ChildNodes.Num(); Idx++)
    {
        FAutoCompleteNode* Node = AutoCompleteTree.ChildNodes[Idx];
        delete Node;
    }

    AutoCompleteTree.ChildNodes.Reset();

    AutoCompleteList.Reset();
    AutoCompleteList.AddDefaulted(ConsoleSettings->ManualAutoCompleteList.Num());
    for (int32_t Idx = 0; Idx < ConsoleSettings->ManualAutoCompleteList.Num(); Idx++)
    {
        AutoCompleteList[Idx] = ConsoleSettings->ManualAutoCompleteList[Idx];
        AutoCompleteList[Idx].Color = ConsoleSettings->AutoCompleteCommandColor;
    }

    RegisterConsoleAutoCompleteEntries.Broadcast(AutoCompleteList);

    {
        IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
            FConsoleObjectVisitor::CreateStatic<TArray<FAutoCompleteCommand>&>(
                &FConsoleVariableAutoCompleteVisitor::OnConsoleVariable,
                AutoCompleteList));
    }

    for (TObjectIterator<UFunction> It; It; ++It)
    {
        UFunction* Func = *It;

        const UClass* FuncOuter = Cast<UClass>(Func->GetOuter());
        const bool bIsLevelScriptFunction = FuncOuter
            && (FuncOuter->IsChildOf(ALevelScriptActor::StaticClass()))
            && (FuncOuter != ALevelScriptActor::StaticClass())
            && (Func->ReturnValueOffset == MAX_uint16)
            && (Func->GetSuperFunction() == nullptr);

        if ((Func->HasAnyFunctionFlags(FUNC_Exec) && (Func->GetSuperFunction() == nullptr || FuncOuter))
            || bIsLevelScriptFunction)
        {
            FString FuncName = Func->GetName();
            if (FDefaultValueHelper::HasWhitespaces(FuncName))
            {
                FuncName = FString::Printf(TEXT("\"%s\""), *FuncName);
            }
            if (bIsLevelScriptFunction)
            {
                FuncName = FString(TEXT("ce ")) + FuncName;
            }

            const int32_t NewIdx = AutoCompleteList.AddDefaulted();
            AutoCompleteList[NewIdx].Command = FuncName;
            AutoCompleteList[NewIdx].Color = ConsoleSettings->AutoCompleteCommandColor;

            FString Desc;

            for (TFieldIterator<UProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
            {
                UProperty* Prop = *PropIt;
                Desc += FString::Printf(TEXT("%s[%s] "), *Prop->GetName(), *Prop->GetCPPType());
            }
            AutoCompleteList[NewIdx].Desc = Desc;
        }
    }

    {
        TArray<FString> Packages;
        for (int32_t PathIdx = 0; PathIdx < ConsoleSettings->AutoCompleteMapPaths.Num(); ++PathIdx)
        {
            FPackageName::FindPackagesInDirectory(Packages, FString::Printf(TEXT("%s%s"), *FPaths::GameDir(), *ConsoleSettings->AutoCompleteMapPaths[PathIdx]));
        }

        FPackageName::FindPackagesInDirectory(Packages, FPaths::GameUserDeveloperDir());

        for (int32_t PackageIndex = 0; PackageIndex < Packages.Num(); PackageIndex++)
        {
            FString Pkg = std::move(Packages[PackageIndex]);
            const int32_t ExtIdx = Pkg.Find(*FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase, ESearchDir::FromEnd);

            FString MapName;
            if (ExtIdx != INDEX_NONE && Pkg.Split(TEXT("/"), nullptr, &MapName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
            {
                FString TrimmedMapName;
                if (!MapName.Split(TEXT("."), &TrimmedMapName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
                {
                    TrimmedMapName = MapName;
                }
                int32_t NewIdx;
                if (TrimmedMapName.EndsWith(TEXT("_P")))
                {
                    NewIdx = 0;
                    AutoCompleteList.InsertDefaulted(0, 3);
                }
                else
                {
                    NewIdx = AutoCompleteList.AddDefaulted(3);
                }
                AutoCompleteList[NewIdx].Command = FString::Printf(TEXT("open %s"), *TrimmedMapName);
                AutoCompleteList[NewIdx].Color = ConsoleSettings->AutoCompleteCommandColor;
                AutoCompleteList[NewIdx + 1].Command = FString::Printf(TEXT("travel %s"), *TrimmedMapName);
                AutoCompleteList[NewIdx + 1].Color = ConsoleSettings->AutoCompleteCommandColor;
                AutoCompleteList[NewIdx + 2].Command = FString::Printf(TEXT("servertravel %s"), *TrimmedMapName);
                AutoCompleteList[NewIdx + 2].Color = ConsoleSettings->AutoCompleteCommandColor;
            }
        }
    }

    {
        const int32_t NewIdx = AutoCompleteList.AddDefaulted();
        AutoCompleteList[NewIdx].Command = FString(TEXT("open 127.0.0.1"));
        AutoCompleteList[NewIdx].Desc = FString(TEXT("(opens connection to localhost)"));
        AutoCompleteList[NewIdx].Color = ConsoleSettings->AutoCompleteCommandColor;
    }

    {
        struct FIterSink
        {
            FIterSink(TArray<FAutoCompleteCommand>& InAutoCompleteList)
                : AutoCompleteList(InAutoCompleteList)
            {
            }

            bool OnEngineShowFlag(uint32_t InIndex, const FString& InName)
            {
                FString LocName;
                FEngineShowFlags::FindShowFlagDisplayName(InName, LocName);

                const int32_t NewIdx = AutoCompleteList.AddZeroed(1);
                AutoCompleteList[NewIdx].Command = TEXT("show ") + InName;
                AutoCompleteList[NewIdx].Desc = FString::Printf(TEXT("(toggles the %s showflag)"), *LocName);
                AutoCompleteList[NewIdx].Color = GetDefault<UConsoleSettings>()->AutoCompleteCommandColor;

                return true;
            }

            TArray<FAutoCompleteCommand>& AutoCompleteList;
        };

        FIterSink Sink(AutoCompleteList);
        FEngineShowFlags::IterateAllFlags(Sink);
    }

    for (int32_t ListIdx = 0; ListIdx < AutoCompleteList.Num(); ListIdx++)
    {
        FString Command = AutoCompleteList[ListIdx].Command.ToLower();
        FAutoCompleteNode* Node = &AutoCompleteTree;
        for (int32_t Depth = 0; Depth < Command.Len(); Depth++)
        {
            const int32_t Char = Command[Depth];
            int32_t FoundNodeIdx = INDEX_NONE;
            TArray<FAutoCompleteNode*>& NodeList = Node->ChildNodes;
            for (int32_t NodeIdx = 0; NodeIdx < NodeList.Num(); NodeIdx++)
            {
                if (NodeList[NodeIdx]->IndexChar == Char)
                {
                    FoundNodeIdx = NodeIdx;
                    Node = NodeList[FoundNodeIdx];
                    NodeList[FoundNodeIdx]->AutoCompleteListIndices.Add(ListIdx);
                    break;
                }
            }
            if (FoundNodeIdx == INDEX_NONE)
            {
                FAutoCompleteNode* NewNode = new FAutoCompleteNode(Char);
                NewNode->AutoCompleteListIndices.Add(ListIdx);
                Node->ChildNodes.Add(NewNode);
                Node = NewNode;
            }
        }
    }
    bIsRuntimeAutoCompleteUpToDate = true;
#endif
}

void UConsole::BuildRuntimeAutoCompleteListHK(UConsole* This, bool bForce)
{
    This->BuildRuntimeAutoCompleteList(bForce);
}

void UConsole::ConsoleCommandHK(UConsole* This, const FString& Command)
{
    // Cheats are on for every player (#65): the controller the line runs on gets a cheat manager before the command runs,
    // so a cheat typed here runs on this player's game, and "cheat <command>" runs it on the server (through
    // AFortPlayerController::Cheat). A line runs as typed (#75).
    APlayerController* const PlayerController = ConsolePlayerController(This);
    if (PlayerController && !PlayerController->CheatManager)
    {
        PlayerController->AddCheats(true);
    }

    void* const* const VTable = *reinterpret_cast<void* const* const*>(This);
    for (const auto& [ConsoleVTable, Original] : ConsoleCommandOriginals)
    {
        if (ConsoleVTable == VTable)
        {
            reinterpret_cast<void (*)(UConsole*, const FString&)>(Original)(This, Command);
            return;
        }
    }
}

void UConsole::Hook()
{
    const int32_t Index = Natives::UConsole_BuildRuntimeAutoCompleteListIndex;
    if (Index < 0 || Sites::ConsoleVTables.empty())
    {
        return;
    }

    void* const StrippedBody = Sites::ConsoleVTables.front()[Index];
    for (void** VTable : Sites::ConsoleVTables)
    {
        if (VTable[Index] == StrippedBody)
        {
            Hooks::SwapVTable(L"UConsole::BuildRuntimeAutoCompleteList", VTable, Index, reinterpret_cast<void*>(&BuildRuntimeAutoCompleteListHK));
        }

        void* Original = nullptr;
        const int32_t CommandIndex = Natives::UConsole_ConsoleCommandIndex;
        if (CommandIndex >= 0 && Hooks::SwapVTable(L"UConsole::ConsoleCommand", VTable, CommandIndex, reinterpret_cast<void*>(&ConsoleCommandHK), &Original) && Original)
        {
            ConsoleCommandOriginals.emplace_back(VTable, Original);
        }
    }
}
