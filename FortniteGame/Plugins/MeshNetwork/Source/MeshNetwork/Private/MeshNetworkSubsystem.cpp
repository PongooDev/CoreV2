#include "pch.h"
#include "MeshNetworkSubsystem.h"
#include "Log.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    const wchar_t* NodeTypeName(EMeshNetworkNodeType Type)
    {
        switch (Type)
        {
        case EMeshNetworkNodeType::Root: return L"Root";
        case EMeshNetworkNodeType::Inner: return L"Inner";
        case EMeshNetworkNodeType::Edge: return L"Edge";
        case EMeshNetworkNodeType::Client: return L"Client";
        default: return L"Unknown";
        }
    }
}

UMeshNetworkSubsystem* UMeshNetworkSubsystem::Get(const UObject* WorldContextObject)
{
    UClass* Class = StaticClass();
    UClass* LibraryClass = Class ? FindObject<UClass>(nullptr, L"/Script/Engine.SubsystemBlueprintLibrary") : nullptr;
    UObject* Library = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
    UFunction* Function = Library ? Library->FindFunction(FName(L"GetGameInstanceSubsystem", FNAME_Find)) : nullptr;
    Reflection::FParams Params(Function);
    if (!WorldContextObject || !Params.Set(L"ContextObject", const_cast<UObject*>(WorldContextObject)) || !Params.Set(L"Class", reinterpret_cast<UObject*>(Class))
        || !Params.Call(Library))
    {
        return nullptr;
    }
    return static_cast<UMeshNetworkSubsystem*>(Params.Take<UObject*>(L"ReturnValue"));
}

void UMeshNetworkSubsystem::SetMeshNetworkNodeType(EMeshNetworkNodeType InNodeType)
{
    NodeType = static_cast<uint8_t>(InNodeType);
    Log::Info(L"UMeshNetworkSubsystem::SetMeshNetworkNodeType: {}", NodeTypeName(InNodeType));
    uint8_t Parameter = static_cast<uint8_t>(InNodeType);
    OnMeshNodeTypeChanged.ProcessMulticastDelegate(&Parameter);
}

void UMeshNetworkSubsystem::SetConnectedToRoot(bool bInConnectedToRoot)
{
    if (Member_bConnectedToRoot.IsAvailable() && bConnectedToRoot != bInConnectedToRoot)
    {
        bConnectedToRoot = bInConnectedToRoot;
        bool Parameter = bInConnectedToRoot;
        OnConnectedToRootChanged.ProcessMulticastDelegate(&Parameter);
    }
}
