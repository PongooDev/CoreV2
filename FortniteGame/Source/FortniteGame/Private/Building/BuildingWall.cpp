#include "pch.h"
#include "Building/BuildingWall.h"
#include "Finder.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

#include <cstring>

namespace
{
    // ETInteractionType::IT_Simple: a player's press of interact.
    constexpr uint8_t InteractSimple = 1;

    // FInteractionType's members, by their reflected offsets (2.5 to 3.1: InteractionType +0, RequestingPawn +4, an
    // unreflected pawn +0xC that the wall's CanInteract reads, InteractComponent +0x14, InteractPoint +0x1C; 0x28 bytes).
    struct FInteractionTypeLayout
    {
        int32_t Size = -1;
        int32_t InteractionType = -1;
        int32_t RequestingPawn = -1;
        int32_t InteractComponent = -1;
        int32_t InteractPoint = -1;
    };

    const FInteractionTypeLayout& InteractionTypeLayout()
    {
        static const FInteractionTypeLayout Layout = []
        {
            FInteractionTypeLayout Result;
            UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortInteractInterface:ServerOnAttemptInteract");
            UProperty* Param = Function ? Function->FindPropertyByName(FName(L"InteractType", FNAME_Find)) : nullptr;
            if (!Param || !Param->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty))
            {
                return Result;
            }
            UScriptStruct* Struct = static_cast<UStructProperty*>(Param)->Struct;
            const auto Offset = [Struct](const wchar_t* Name)
            {
                UProperty* Member = Struct ? Struct->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
                return Member ? Member->GetOffset_ForInternal() : -1;
            };
            Result.Size = Struct ? Struct->GetStructureSize() : -1;
            Result.InteractionType = Offset(L"InteractionType");
            Result.RequestingPawn = Offset(L"RequestingPawn");
            Result.InteractComponent = Offset(L"InteractComponent");
            Result.InteractPoint = Offset(L"InteractPoint");
            return Result;
        }();
        return Layout;
    }
}

bool ABuildingWall::IsDoor() const
{
    return HasDoor() && DoorMesh != nullptr && DoorComponent != nullptr;
}

bool ABuildingWall::ServerOnAttemptInteract(AFortPawn* RequestingPawn)
{
#if WITH_SERVER_CODE
    const FInteractionTypeLayout& Layout = InteractionTypeLayout();
    if (!RequestingPawn || !IsDoor() || Layout.Size <= 0 || Layout.Size > 0x80 || Layout.InteractionType < 0 || Layout.RequestingPawn < 0 || Layout.InteractComponent < 0
        || !Finder::ResolveServerOnAttemptInteractLive())
    {
        return false;
    }
    static UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    void* Interface = InteractInterface ? Natives::UObject_GetInterfaceAddress(reinterpret_cast<UObject*>(this), InteractInterface) : nullptr;
    if (!Interface)
    {
        return false;
    }

    alignas(8) uint8_t InteractType[0x80] = {};
    InteractType[Layout.InteractionType] = InteractSimple;
    const FWeakObjectPtr Pawn(reinterpret_cast<UObject*>(RequestingPawn));
    std::memcpy(InteractType + Layout.RequestingPawn, &Pawn, sizeof(Pawn));
    // The pawn again in the unreflected slot after it (21.40's RequestingPlayerController sits there; 3.1's wall
    // CanInteract casts it to AFortPlayerPawn), when the layout leaves room for it before InteractComponent.
    if (Layout.InteractComponent - Layout.RequestingPawn >= 2 * static_cast<int32_t>(sizeof(Pawn)))
    {
        std::memcpy(InteractType + Layout.RequestingPawn + sizeof(Pawn), &Pawn, sizeof(Pawn));
    }
    const FWeakObjectPtr Component(static_cast<UObject*>(DoorComponent));
    std::memcpy(InteractType + Layout.InteractComponent, &Component, sizeof(Component));
    if (Layout.InteractPoint >= 0)
    {
        const FVector Point = GetTransform().GetLocation();
        std::memcpy(InteractType + Layout.InteractPoint, &Point, sizeof(Point));
    }

    // The interface's own vtable (UHT's exec for it calls the same slot on the interface pointer).
    using FServerOnAttemptInteract = bool (*)(void* This, const void* InteractType);
    void** VTable = *static_cast<void***>(Interface);
    return reinterpret_cast<FServerOnAttemptInteract>(VTable[Natives::IFortInteractInterface_ServerOnAttemptInteractIndex])(Interface, InteractType);
#else
    return false;
#endif
}
