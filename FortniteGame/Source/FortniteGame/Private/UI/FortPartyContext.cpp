#include "pch.h"
#include "UI/FortPartyContext.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Containers/ScriptArray.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameInstance.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameThread.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "McpProfile.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "UI/FortUITeamInfo.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace PartyBarTypes
{
    class UBlueprintContextLibrary : public UObject
    {
        DefineUnrealClass(UBlueprintContextLibrary, BlueprintContext)
        DefineUFunction(GetContext)
    };

    class UUserWidget : public UObject
    {
        DefineUnrealClass(UUserWidget, UMG)
        DefineUFunction(GetOwningPlayer)
    };

    class UWidget : public UObject
    {
        DefineUnrealClass(UWidget, UMG)
        DefineUFunction(SetVisibility)
    };

    class UTextBlock : public UObject
    {
        DefineUnrealClass(UTextBlock, UMG)
        DefineUFunction(SetText)
    };

    class UImage : public UObject
    {
        DefineUnrealClass(UImage, UMG)
        DefineUFunction(SetBrushFromMaterial)
        DefineUFunction(GetDynamicMaterial)
        DefineUFunction(SetBrushFromTexture)
    };

    class UMaterialInstanceDynamic : public UObject
    {
        DefineUnrealClass(UMaterialInstanceDynamic, Engine)
        DefineUFunction(SetTextureParameterValue)
        DefineUFunction(SetVectorParameterValue)
    };

    class UFortMcpContext : public UObject
    {
        DefineUnrealClass(UFortMcpContext, FortniteGame)
        DefineUFunction(GetLoadoutForPlayer)
    };

    class UFortBannerUtilities : public UObject
    {
        DefineUnrealClass(UFortBannerUtilities, FortniteGame)
        DefineUFunction(LoadAndGetBannerLargeIconFromRowName)
        DefineUFunction(GetBannerColorPrimaryColorFromRowName)
        DefineUFunction(GetBannerColorSecondaryColorFromRowName)
    };
}

namespace
{
    // ESlateVisibility.
    constexpr uint8_t Collapsed = 1;
    constexpr uint8_t SelfHitTestInvisible = 4;

    // How often the banners follow the party: nothing in the front end tells them when the party changes.
    constexpr ULONGLONG RefreshMs = 1000;
    // How often the object array is searched for banners while some are known (the main menu makes its bar once).
    constexpr ULONGLONG RescanMs = 5000;

    constexpr const wchar_t* AthenaPartyMemberClassPath = L"/Game/Athena/UI/Frontend/Parties/FullPartyMemberAthena.FullPartyMemberAthena_C";
    // PlayerBanner's brush: the banner material its SetBannerIconId and SetBannerColorId fill.
    constexpr const wchar_t* BannerMaterialPath = L"/Game/UI/Banner/BannerIcon.BannerIcon";
    // What FullPartyMemberAthena's connected banner (Image_3) shows of itself.
    constexpr const wchar_t* DefaultBannerPath = L"/Game/Athena/HUD/Art/T-Slot-FNBR-Banner-Empty-L.T-Slot-FNBR-Banner-Empty-L";

    // A party member's profile0 the game left unqueried: how long the game gets to query it itself (where it still
    // does, well within this), how long a query gets before another, and how many are sent.
    constexpr ULONGLONG ProfileGraceMs = 5000;
    constexpr ULONGLONG ProfileRetryMs = 15000;
    constexpr int32_t MaxProfileQueries = 3;

    struct FTrackedMember
    {
        TWeakObjectPtr<UObject> Widget;
        std::wstring Shown;
        // The Battle Royale banner drawn on the connected banner, "member|icon|color", empty for the bar's own.
        std::wstring Banner;
        bool bLogged = false;
    };

    struct FMemberProfile
    {
        TWeakObjectPtr<UObject> Profile;
        ULONGLONG FirstSeen = 0;
        ULONGLONG LastQuery = 0;
        int32_t Queries = 0;
        bool bLoaded = false;
    };

    constexpr const wchar_t* PlayerTrackerClassPath = L"/Script/FortniteUI.FortPlayerTrackerBase";

    std::vector<FTrackedMember> Tracked;
    std::vector<FMemberProfile> MemberProfiles;
    std::vector<TWeakObjectPtr<UObject>> LocalTrackers;
    TWeakObjectPtr<UObject> AthenaPartyMemberClass;
    TWeakObjectPtr<UObject> PlayerTrackerClass;
    ULONGLONG NextRefresh = 0;
    ULONGLONG NextRescan = 0;

    template <typename T>
    T* PropertyOf(const UStruct* Owner, const wchar_t* Name)
    {
        return Owner ? static_cast<T*>(Owner->FindPropertyByName(FName(Name, FNAME_Find))) : nullptr;
    }

    template <typename T>
    T* ValueOf(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? PropertyOf<UProperty>(Object->GetClass(), Name) : nullptr;
        return Property ? Property->ContainerPtrToValuePtr<T>(Object) : nullptr;
    }

    UObject* WidgetOf(UObject* Object, const wchar_t* Name)
    {
        UObject** Value = ValueOf<UObject*>(Object, Name);
        return Value ? *Value : nullptr;
    }

    // A function's parameters, initialized and destroyed with it. The buffer holds the whole frame: InitializeStruct
    // writes the function's local variables too.
    class FCall
    {
    public:
        explicit FCall(UFunction* InFunction)
            : Function(InFunction)
        {
            if (Function)
            {
                Parms.assign(std::max<size_t>(Function->ParmsSize, static_cast<size_t>(Function->GetStructureSize())), 0);
                Function->InitializeStruct(Parms.data());
            }
        }

        ~FCall()
        {
            if (Function)
            {
                Function->DestroyStruct(Parms.data());
            }
        }

        FCall(const FCall&) = delete;
        FCall& operator=(const FCall&) = delete;

        template <typename T = UProperty>
        T* Property(const wchar_t* Name) const
        {
            return PropertyOf<T>(Function, Name);
        }

        void* Param(const wchar_t* Name)
        {
            UProperty* Found = Property(Name);
            return Found ? Found->ContainerPtrToValuePtr<void>(Parms.data()) : nullptr;
        }

        bool Call(UObject* Object)
        {
            if (!Function || !Object)
            {
                return false;
            }
            Object->ProcessEvent(Function, Parms.data());
            return true;
        }

    private:
        UFunction* Function;
        std::vector<uint8_t> Parms;
    };

    // The UI context of a class the widget's local player has (UBlueprintContextLibrary::GetContext).
    UObject* ContextOf(UObject* ContextObject, UClass* Class)
    {
        UClass* Library = PartyBarTypes::UBlueprintContextLibrary::StaticClass();
        if (!ContextObject || !Library || !Class)
        {
            return nullptr;
        }
        Reflection::FParams Params(PartyBarTypes::UBlueprintContextLibrary::StaticFunction_GetContext());
        if (!Params.Set(L"ContextObject", ContextObject) || !Params.Set(L"Class", Class))
        {
            return nullptr;
        }
        Params.Call(Library->GetDefaultObject());
        UObject** Context = Params.Get<UObject*>(L"ReturnValue");
        return Context && *Context && (*Context)->IsA(Class) ? *Context : nullptr;
    }

    UObject* OwningPlayerOf(UObject* Widget)
    {
        Reflection::FParams Params(PartyBarTypes::UUserWidget::StaticFunction_GetOwningPlayer());
        Params.Call(Widget);
        UObject** PlayerController = Params.Get<UObject*>(L"ReturnValue");
        return PlayerController ? *PlayerController : nullptr;
    }

    // The front end's controller is an AFortPlayerControllerFrontEnd; a match's is an AFortPlayerControllerAthena.
    bool IsInFrontEnd(UObject* Widget)
    {
        UObject* PlayerController = OwningPlayerOf(Widget);
        return PlayerController && !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass());
    }

    // The party as the Save the World bar's UFortPlayerTrackerBase::SetTeamMember reads it: slot 0 is the local player's
    // own UFortPartyContext::GetLocalPlayerTeamMemberInfo, slot N the local team's GetTeamMembers()[N].
    class FPartySlots
    {
    public:
        explicit FPartySlots(UObject* Widget)
            : LocalCall(UFortPartyContext::StaticFunction_GetLocalPlayerTeamMemberInfo())
            , TeamCall(UFortUITeamInfo::StaticFunction_GetTeamMembers())
        {
            UFortPartyContext* Party = UFortPartyContext::Get(Widget);
            UStructProperty* LocalInfo = LocalCall.Property<UStructProperty>(L"LocalPlayerInfo");
            if (!Party || !LocalInfo)
            {
                return;
            }
            Struct = LocalInfo->Struct;
            if (LocalCall.Call(Party))
            {
                Local = static_cast<const uint8_t*>(LocalCall.Param(L"LocalPlayerInfo"));
            }

            UFortUITeamInfo* Team = Party->GetLocalPlayerTeam();
            UArrayProperty* TeamMembers = TeamCall.Property<UArrayProperty>(L"TeamMembers");
            UProperty* Inner = TeamMembers ? static_cast<UProperty*>(TeamMembers->Inner) : nullptr;
            if (Team && Inner && TeamCall.Call(Team))
            {
                const FScriptArray* Array = static_cast<const FScriptArray*>(TeamCall.Param(L"TeamMembers"));
                Members = static_cast<const uint8_t*>(Array->GetData());
                Num = Array->Num();
                Stride = Inner->ElementSize;
            }

            MemberUniqueId = PropertyOf<UProperty>(Struct, L"MemberUniqueId");
            PlayerName = PropertyOf<UProperty>(Struct, L"PlayerName");
            bPartyLeader = PropertyOf<UBoolProperty>(Struct, L"bPartyLeader");
            NumPlayersInParty = PropertyOf<UProperty>(Struct, L"NumPlayersInParty");
        }

        // The member in a slot, or null for an open slot.
        const uint8_t* Slot(int32_t Index) const
        {
            const uint8_t* Info = Index == 0 ? Local : (Index > 0 && Index < Num ? Members + static_cast<size_t>(Index) * Stride : nullptr);
            return Info && MemberUniqueId && MemberIdOf(Info).IsValid() ? Info : nullptr;
        }

        const FUniqueNetIdRepl& MemberIdOf(const uint8_t* Info) const
        {
            return *MemberUniqueId->ContainerPtrToValuePtr<FUniqueNetIdRepl>(Info);
        }

        const void* PlayerNameOf(const uint8_t* Info) const
        {
            return PlayerName ? PlayerName->ContainerPtrToValuePtr<void>(Info) : nullptr;
        }

        // The crown the Save the World bar shows: the leader of a party of more than one.
        bool ShowsLeader(const uint8_t* Info) const
        {
            const int32_t Players = NumPlayersInParty ? *NumPlayersInParty->ContainerPtrToValuePtr<int32_t>(Info) : 0;
            return bPartyLeader && bPartyLeader->GetPropertyValue_InContainer(Info) && Players > 1;
        }

        UScriptStruct* Struct = nullptr;

    private:
        FCall LocalCall;
        FCall TeamCall;
        const uint8_t* Local = nullptr;
        const uint8_t* Members = nullptr;
        int32_t Num = 0;
        int32_t Stride = 0;
        UProperty* MemberUniqueId = nullptr;
        UProperty* PlayerName = nullptr;
        UBoolProperty* bPartyLeader = nullptr;
        UProperty* NumPlayersInParty = nullptr;
    };

    void CallEvent(UObject* Object, const wchar_t* Name)
    {
        FCall Event(Object ? Object->FindFunction(FName(Name, FNAME_Find)) : nullptr);
        Event.Call(Object);
    }

    void SetVisibility(UObject* Widget, uint8_t Visibility)
    {
        if (!Widget || !Widget->IsA(PartyBarTypes::UWidget::StaticClass()))
        {
            return;
        }
        Reflection::FParams Params(PartyBarTypes::UWidget::StaticFunction_SetVisibility());
        if (Params.Set(L"InVisibility", Visibility))
        {
            Params.Call(Widget);
        }
    }

    void SetText(UObject* TextBlock, const void* Text)
    {
        UFunction* Function = PartyBarTypes::UTextBlock::StaticFunction_SetText();
        UProperty* InText = PropertyOf<UProperty>(Function, L"InText");
        if (!TextBlock || !Text || !InText || !TextBlock->IsA(PartyBarTypes::UTextBlock::StaticClass()))
        {
            return;
        }
        // The parameter borrows the member's FText: ProcessEvent copies parameters in and out bitwise and destroys
        // none of them, SetText keeps its own copy, and the member outlives the call.
        std::vector<uint8_t> Parms(Function->ParmsSize, 0);
        std::memcpy(InText->ContainerPtrToValuePtr<void>(Parms.data()), Text, static_cast<size_t>(InText->ElementSize));
        TextBlock->ProcessEvent(Function, Parms.data());
    }

    bool UpdateMemberInfo(UObject* Member, UScriptStruct* Struct, const uint8_t* Info)
    {
        FCall Update(Member->FindFunction(FName(L"UpdateMemberInfo", FNAME_Find)));
        UStructProperty* NewMemberInfo = Update.Property<UStructProperty>(L"NewMemberInfo");
        if (!NewMemberInfo || NewMemberInfo->Struct != Struct)
        {
            return false;
        }
        Struct->CopyScriptStruct(Update.Param(L"NewMemberInfo"), Info);
        return Update.Call(Member);
    }

    // A function of one object parameter (Name, or none), and its object return value if it has one.
    UObject* CallForObject(UFunction* Function, UObject* Target, const wchar_t* Name, UObject* Value)
    {
        Reflection::FParams Params(Function);
        if (!Target || (Name && !Params.Set(Name, Value)) || !Params.Call(Target))
        {
            return nullptr;
        }
        UObject** Result = PropertyOf<UProperty>(Function, L"ReturnValue") ? Params.Get<UObject*>(L"ReturnValue") : nullptr;
        return Result ? *Result : nullptr;
    }

    // A UFortBannerUtilities colour row's FLinearColor (four floats).
    bool BannerColor(UFunction* Function, FName Row, float (&Color)[4])
    {
        Reflection::FParams Params(Function);
        return Params.Set(L"BannerColorRowName", Row) && Params.Call(PartyBarTypes::UFortBannerUtilities::StaticClass()->GetDefaultObject())
            && Params.Relocate(L"ReturnValue", Color, sizeof(Color));
    }

    void SetMaterialColor(UObject* Material, FName Parameter, const float (&Color)[4])
    {
        Reflection::FParams Params(PartyBarTypes::UMaterialInstanceDynamic::StaticFunction_SetVectorParameterValue());
        void* Value = Params.Set(L"ParameterName", Parameter) ? Params.GetAddress(L"Value") : nullptr;
        if (Value)
        {
            std::memcpy(Value, Color, sizeof(Color));
            Params.Call(Material);
        }
    }

    // The member's Battle Royale banner, their athena loadout's (UFortMcpContext::GetLoadoutForPlayer), on the connected
    // banner. FullPartyMemberAthena has no PlayerBanner: its connected state's Image_3 shows a fixed texture,
    // T-Slot-FNBR-Banner-Empty-L (the dog tags), whoever the member is. The image takes PlayerBanner's material and the
    // loadout's icon and colours, as PlayerBanner's SetBannerIconAndColorFromProfile draws them (BannerLibrary's
    // UpdateBannerIconOnMaterial and UpdateBannerColorOnMaterial: BannerIcon, RGBA0 and RGBA1). A loadout without a
    // banner (the member's athena not loaded yet) shows the bar's own. Returns the banner drawn, "member|icon|color".
    std::wstring DrawBanner(UObject* Member, const FUniqueNetIdRepl& MemberId, const std::wstring& Drawn)
    {
        UFunction* GetLoadout = PartyBarTypes::UFortMcpContext::StaticFunction_GetLoadoutForPlayer();
        UObject* Image = WidgetOf(Member, L"Image_3");
        UObject* McpContext = ContextOf(Member, PartyBarTypes::UFortMcpContext::StaticClass());
        if (!GetLoadout || !Image || !McpContext || !Image->IsA(PartyBarTypes::UImage::StaticClass()))
        {
            return Drawn;
        }

        Reflection::FParams Loadout(GetLoadout, true);
        UStructProperty* LoadoutValue = PropertyOf<UStructProperty>(GetLoadout, L"ReturnValue");
        UProperty* IconId = LoadoutValue ? PropertyOf<UProperty>(LoadoutValue->Struct, L"BannerIconId") : nullptr;
        UProperty* ColorId = LoadoutValue ? PropertyOf<UProperty>(LoadoutValue->Struct, L"BannerColorId") : nullptr;
        const void* Value = Loadout.GetAddress(L"ReturnValue");
        if (!IconId || !ColorId || !Value || !Loadout.CopyStruct(L"MemberUniqueId", &MemberId) || !Loadout.Call(McpContext))
        {
            return Drawn;
        }
        const FString& Icon = *IconId->ContainerPtrToValuePtr<FString>(Value);
        const FString& Color = *ColorId->ContainerPtrToValuePtr<FString>(Value);
        std::wstring Banner = Icon.IsEmpty() ? std::wstring() : MemberId.ToString().ToStdString() + L"|" + Icon.ToStdString() + L"|" + Color.ToStdString();
        if (Banner == Drawn)
        {
            return Drawn;
        }
        if (Banner.empty())
        {
            // No banner to draw (the member's athena not loaded yet): the bar's own, not the banner drawn for whoever
            // had the slot before.
            UObject* Default = Drawn.empty() ? nullptr : StaticFindObject(nullptr, nullptr, DefaultBannerPath, false);
            Reflection::FParams SetTexture(PartyBarTypes::UImage::StaticFunction_SetBrushFromTexture());
            if (Default && SetTexture.Set(L"Texture", Default))
            {
                SetTexture.Call(Image);
                return Banner;
            }
            return Drawn;
        }

        UObject* Material = StaticFindObject(nullptr, nullptr, BannerMaterialPath, false);
        if (!Material)
        {
            Material = StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, BannerMaterialPath);
        }
        UObject* Utilities = PartyBarTypes::UFortBannerUtilities::StaticClass()->GetDefaultObject();
        Reflection::FParams IconLoad(PartyBarTypes::UFortBannerUtilities::StaticFunction_LoadAndGetBannerLargeIconFromRowName());
        if (!Material || !Utilities || !IconLoad.Set(L"BannerIconRowName", FName(*Icon, FNAME_Find)) || !IconLoad.Call(Utilities))
        {
            return Drawn;
        }
        UObject** Texture = IconLoad.Get<UObject*>(L"ReturnValue");
        float Primary[4] = {};
        float Secondary[4] = {};
        const FName ColorRow(*Color, FNAME_Find);
        if (!Texture || !*Texture
            || !BannerColor(PartyBarTypes::UFortBannerUtilities::StaticFunction_GetBannerColorPrimaryColorFromRowName(), ColorRow, Primary)
            || !BannerColor(PartyBarTypes::UFortBannerUtilities::StaticFunction_GetBannerColorSecondaryColorFromRowName(), ColorRow, Secondary))
        {
            return Drawn;
        }

        CallForObject(PartyBarTypes::UImage::StaticFunction_SetBrushFromMaterial(), Image, L"Material", Material);
        UObject* Dynamic = CallForObject(PartyBarTypes::UImage::StaticFunction_GetDynamicMaterial(), Image, nullptr, nullptr);
        if (!Dynamic || !Dynamic->IsA(PartyBarTypes::UMaterialInstanceDynamic::StaticClass()))
        {
            return Drawn;
        }
        Reflection::FParams SetIcon(PartyBarTypes::UMaterialInstanceDynamic::StaticFunction_SetTextureParameterValue());
        if (SetIcon.Set(L"ParameterName", FName(L"BannerIcon")) && SetIcon.Set(L"Value", *Texture))
        {
            SetIcon.Call(Dynamic);
        }
        SetMaterialColor(Dynamic, FName(L"RGBA0"), Primary);
        SetMaterialColor(Dynamic, FName(L"RGBA1"), Secondary);
        return Banner;
    }

    // One banner of the bar. UpdateMemberInfo picks open, connecting or connected from the party context's
    // GetPartyMemberDisplayState, as the Save the World bar does; its ShowConnected gives up on the name (it reads the
    // match's TeamMembers), so the name is set here, and the banner. The kill count is the match's and stays hidden.
    std::wstring Show(FTrackedMember& Entry, UObject* Member, const FPartySlots& Party)
    {
        const int32_t* Index = ValueOf<int32_t>(Member, L"TeamMemberIndex");
        const uint8_t* Info = Index ? Party.Slot(*Index) : nullptr;
        std::wstring Shown;

        if (Info && UpdateMemberInfo(Member, Party.Struct, Info))
        {
            SetText(WidgetOf(Member, L"PlayerName"), Party.PlayerNameOf(Info));
            SetVisibility(WidgetOf(Member, L"LeaderImageOverlay"), Party.ShowsLeader(Info) ? SelfHitTestInvisible : Collapsed);
            Entry.Banner = DrawBanner(Member, Party.MemberIdOf(Info), Entry.Banner);
            Shown = Party.MemberIdOf(Info).ToString().ToStdString();
        }
        else
        {
            CallEvent(Member, L"ShowOpen");
            SetVisibility(WidgetOf(Member, L"LeaderImageOverlay"), Collapsed);
        }
        SetVisibility(WidgetOf(Member, L"KillsText"), Collapsed);
        SetVisibility(WidgetOf(Member, L"Image_1"), Collapsed);
        return Shown;
    }

    // Only the front end's world: a match's banners keep the game's own path, and the search stays out of matches.
    bool IsFrontEndWorld()
    {
        UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
        return World && World->GetName().ToStdString() == L"Frontend";
    }

    // The banners: every live FullPartyMemberAthena widget. The class exists once the main menu has loaded it.
    void FindMembers()
    {
        UObject* Class = AthenaPartyMemberClass.Get();
        if (!Class)
        {
            Class = StaticFindObject(nullptr, nullptr, AthenaPartyMemberClassPath, false);
            AthenaPartyMemberClass = Class;
        }
        const FUObjectArray* Array = Natives::GUObjectArray;
        if (!Class || !Array)
        {
            return;
        }

        for (int32_t Index = 0; Index < Array->GetObjectArrayNum(); ++Index)
        {
            const FUObjectItem* Item = Array->IndexToObject(Index);
            UObject* Object = Item ? reinterpret_cast<UObject*>(Item->Object) : nullptr;
            if (!Object || Item->IsUnreachable() || Item->IsPendingKill() || reinterpret_cast<UObject*>(Object->GetClass()) != Class || Object->IsTemplate())
            {
                continue;
            }
            const bool bKnown = std::any_of(Tracked.begin(), Tracked.end(), [Object](const FTrackedMember& Known) { return Known.Widget.Get() == Object; });
            if (!bKnown)
            {
                Tracked.push_back(FTrackedMember{ TWeakObjectPtr<UObject>(Object) });
            }
        }
    }

    // 1.8.1's UFortRegisteredPlayerInfo (its profile query, 0x638D00) queries no profile of another player while the sub
    // game is Battle Royale (UFortAssetManager's, +0x508, is 1): only its own player's, or on a server. 1.8 queried a
    // party member's profile0 (Core-Backend a4ecf92 sends their athena with it). The party member's profile0 so stays at
    // revision -1, and everything the lobby shows of them comes out empty: UHomeBaseContext::GetTotalHomebaseRating's
    // rating of 0 points (power 1 in Manage Party Member), their banner ids (PlayerBanner keeps the banner it drew last,
    // the viewer's own), and their HomeBaseInfo, whose ValidData UFortPartyContext::GetPartyMemberDisplayState needs
    // for connected (the Battle Royale bar's endless "connecting"). A world whose own sub game is unset keeps the one it
    // was reached from (0x5C7700 sets it only from a valid one), so a Save the World zone's teammates' profile0 may stay
    // unqueried too (CoreV2 #127's escape menu, unconfirmed). Core queries such a profile0 as the game did: after a grace
    // period in which the game's own query (where it still makes one) lands, and again if no answer came. Not in a
    // Battle Royale match, whose hundred registered players need no profile0.
    void LoadPartyMemberProfiles(UWorld* World)
    {
        std::erase_if(MemberProfiles, [](const FMemberProfile& Member) { return !Member.Profile.Get(); });
        UFortGameInstance* GameInstance = World ? Cast<UFortGameInstance>(reinterpret_cast<UObject*>(World->OwningGameInstance)) : nullptr;
        if (!GameInstance || !Natives::UMcpProfile_QueryProfile)
        {
            return;
        }
        const TArray<ULocalPlayer*>& LocalPlayers = GameInstance->LocalPlayers;
        UObject* Controller = LocalPlayers.Num() > 0 && LocalPlayers[0] ? reinterpret_cast<UObject*>(LocalPlayers[0]->PlayerController) : nullptr;
        if (!Controller || Controller->IsA(AFortPlayerControllerAthena::StaticClass()))
        {
            return;
        }

        const ULONGLONG Now = GetTickCount64();
        const TArray<UFortRegisteredPlayerInfo*>& Players = GameInstance->GetMember_RegisteredPlayers();
        for (int32_t Index = 0; Index < Players.Num(); ++Index)
        {
            UFortRegisteredPlayerInfo* Player = Players[Index];
            UMcpProfile* Profile = Player ? reinterpret_cast<UMcpProfile*>(Player->AccountProfile) : nullptr;
            if (!Profile)
            {
                continue;
            }
            auto Known = std::find_if(MemberProfiles.begin(), MemberProfiles.end(), [Profile](const FMemberProfile& Member) { return Member.Profile.Get() == Profile; });
            if (Known == MemberProfiles.end())
            {
                MemberProfiles.push_back(FMemberProfile{ TWeakObjectPtr<UObject>(Profile), Now });
                Known = std::prev(MemberProfiles.end());
            }

            const std::wstring PlayerId = Player->PlayerID.ToString().ToStdString();
            if (Profile->ProfileRevision >= 0)
            {
                if (Known->Queries > 0 && !Known->bLoaded)
                {
                    UE_LOG(LogFort, Log, TEXT("FortPartyContext: %s's profile0 is loaded (revision %lld)"), PlayerId.c_str(), static_cast<long long>(Profile->ProfileRevision));
                }
                Known->bLoaded = true;
                continue;
            }
            if (Now - Known->FirstSeen < ProfileGraceMs || Known->Queries >= MaxProfileQueries || (Known->Queries > 0 && Now - Known->LastQuery < ProfileRetryMs))
            {
                continue;
            }
            if (Profile->QueryProfile())
            {
                ++Known->Queries;
                Known->LastQuery = Now;
                UE_LOG(LogFort, Log, TEXT("FortPartyContext: queries %s's profile0, which the game left unqueried (attempt %d)"), PlayerId.c_str(), Known->Queries);
            }
        }
    }

    // The local player's controller, unless it is a Battle Royale match's.
    UObject* SaveTheWorldController(UWorld* World)
    {
        UFortGameInstance* GameInstance = World ? Cast<UFortGameInstance>(reinterpret_cast<UObject*>(World->OwningGameInstance)) : nullptr;
        const TArray<ULocalPlayer*>* LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
        UObject* Controller = LocalPlayers && LocalPlayers->Num() > 0 && (*LocalPlayers)[0] ? reinterpret_cast<UObject*>((*LocalPlayers)[0]->PlayerController) : nullptr;
        return Controller && !Controller->IsA(AFortPlayerControllerAthena::StaticClass()) ? Controller : nullptr;
    }

    // Every live player tracker of the local player's own slot (UFortPlayerTrackerBase, IsLocalPlayer).
    void FindLocalTrackers()
    {
        UObject* Class = PlayerTrackerClass.Get();
        if (!Class)
        {
            Class = StaticFindObject(nullptr, nullptr, PlayerTrackerClassPath, false);
            PlayerTrackerClass = Class;
        }
        const FUObjectArray* Array = Natives::GUObjectArray;
        if (!Class || !Array)
        {
            return;
        }

        // Whether each class seen is a tracker's, so each class is walked once.
        std::unordered_map<UClass*, bool> Trackers;
        for (int32_t Index = 0; Index < Array->GetObjectArrayNum(); ++Index)
        {
            const FUObjectItem* Item = Array->IndexToObject(Index);
            UObject* Object = Item ? reinterpret_cast<UObject*>(Item->Object) : nullptr;
            if (!Object || Item->IsUnreachable() || Item->IsPendingKill())
            {
                continue;
            }
            UClass* ObjectClass = Object->GetClass();
            auto Known = Trackers.find(ObjectClass);
            if (Known == Trackers.end())
            {
                Known = Trackers.emplace(ObjectClass, ObjectClass && ObjectClass->IsChildOf(reinterpret_cast<UStruct*>(Class))).first;
            }
            const bool* bLocal = Known->second && !Object->IsTemplate() ? ValueOf<bool>(Object, L"IsLocalPlayer") : nullptr;
            if (!bLocal || !*bLocal)
            {
                continue;
            }
            const bool bTracked = std::any_of(LocalTrackers.begin(), LocalTrackers.end(), [Object](const TWeakObjectPtr<UObject>& Tracker) { return Tracker.Get() == Object; });
            if (!bTracked)
            {
                LocalTrackers.push_back(TWeakObjectPtr<UObject>(Object));
            }
        }
    }

    // Whether a tracker shows a member: its MemberInfo (FullPartyMember's) has a MemberUniqueId. True for a tracker
    // without one, which Core leaves alone.
    bool ShowsMember(UObject* Tracker)
    {
        UStructProperty* MemberInfo = PropertyOf<UStructProperty>(Tracker->GetClass(), L"MemberInfo");
        UProperty* MemberUniqueId = MemberInfo ? PropertyOf<UProperty>(MemberInfo->Struct, L"MemberUniqueId") : nullptr;
        return !MemberUniqueId || MemberUniqueId->ContainerPtrToValuePtr<FUniqueNetIdRepl>(MemberInfo->ContainerPtrToValuePtr<void>(Tracker))->IsValid();
    }

    // A mission's escape menu showed the other players' banners, power and stats but the local player's own slot as
    // open ("+") (CoreV2 #111). The party bar (FullPartyBar) gives slot 0 to the local player: its tracker's
    // UFortPlayerTrackerBase::SetTeamMember(0) (1.9 0x2F7AA50) keeps the local player's id and shows an empty
    // FFortTeamMemberInfo, which only a later event replaces (the local team's member added, removed or changed, or a
    // party member joining or leaving the front end's party). The other slots read the team's GetTeamMembers at once.
    // The front end's party sends such events; a mission's team, settled by the time the menu is built, sends none, so
    // the slot stays open although the team lists the local player (GetTeamMembers's first member). Once a second,
    // outside the front end and Battle Royale matches, every local tracker still showing no member reads its member again
    // with its own UpdateBasedOnId (the local team's GetTeamMemberInfo for its id), as such an event would.
    void RefreshLocalTrackers(UWorld* World)
    {
        std::erase_if(LocalTrackers, [](const TWeakObjectPtr<UObject>& Tracker) { return !Tracker.Get(); });
        if (IsFrontEndWorld() || !SaveTheWorldController(World))
        {
            return;
        }
        FindLocalTrackers();
        for (const TWeakObjectPtr<UObject>& Entry : LocalTrackers)
        {
            UObject* Tracker = Entry.Get();
            if (!Tracker || ShowsMember(Tracker))
            {
                continue;
            }
            CallEvent(Tracker, L"UpdateBasedOnId");
            if (ShowsMember(Tracker))
            {
                UE_LOG(LogFort, Log, TEXT("FortPlayerTracker: %s shows the local player"), *GetNameSafe(Tracker));
            }
        }
    }

    void Refresh()
    {
        std::erase_if(Tracked, [](const FTrackedMember& Member) { return !Member.Widget.Get(); });
        UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
        LoadPartyMemberProfiles(World);
        RefreshLocalTrackers(World);
        if (!IsFrontEndWorld())
        {
            return;
        }
        const ULONGLONG Now = GetTickCount64();
        if (Tracked.empty() || Now >= NextRescan)
        {
            NextRescan = Now + RescanMs;
            FindMembers();
        }

        std::unique_ptr<FPartySlots> Party;
        for (FTrackedMember& Member : Tracked)
        {
            UObject* Widget = Member.Widget.Get();
            if (!IsInFrontEnd(Widget))
            {
                continue;
            }
            if (!Party)
            {
                Party = std::make_unique<FPartySlots>(Widget);
            }

            std::wstring Shown = Show(Member, Widget, *Party);
            if (!Member.bLogged || Shown != Member.Shown)
            {
                const int32_t* Index = ValueOf<int32_t>(Widget, L"TeamMemberIndex");
                UE_LOG(LogFort, Log, TEXT("FullPartyBarAthena: slot %d %s%s"), Index ? *Index : -1, Shown.empty() ? L"is open" : L"shows ", Shown.c_str());
                Member.Shown = std::move(Shown);
                Member.bLogged = true;
            }
        }
    }

    // Every frame on the game thread, a refresh once a second.
    void Tick()
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now >= NextRefresh)
        {
            NextRefresh = Now + RefreshMs;
            Refresh();
        }
        GameThread::Enqueue(&Tick);
    }
}

UFortPartyContext* UFortPartyContext::Get(UObject* ContextObject)
{
    return reinterpret_cast<UFortPartyContext*>(ContextOf(ContextObject, StaticClass()));
}

UFortUITeamInfo* UFortPartyContext::GetLocalPlayerTeam()
{
    Reflection::FParams Params(StaticFunction_GetLocalPlayerTeam());
    Params.Call(this);
    UObject** Team = Params.Get<UObject*>(L"ReturnValue");
    return Team ? reinterpret_cast<UFortUITeamInfo*>(*Team) : nullptr;
}

// 1.8.1 gave the Battle Royale main menu (MainMenu's ConfigureSubGameWidgets) a party bar of its own,
// FullPartyBarAthena; 1.8's showed none. Its four banners (FullPartyMemberAthena) are the match's squad: each reads the
// owning AFortPlayerControllerAthena's TeamMembers and follows AthenaHUDContext's team events. The lobby's controller is
// an AFortPlayerControllerFrontEnd, so every banner's cast fails and it shows open ("+"), the local player's too
// (Core-Backend #104). In the front end each banner shows the party instead, slot by slot as the Save the World bar
// (FullPartyBar) does. No engine function is hooked: once a second, in the front end's world, Core finds the banners in
// the object array and fills them. The same refresh, outside Battle Royale matches, queries the other players' profile0
// that 1.8.1 leaves unqueried while the sub game is Battle Royale (LoadPartyMemberProfiles), and outside the front end
// fills a mission's escape menu slot of the local player (RefreshLocalTrackers).
void UFortPartyContext::StartAthenaPartyBar()
{
    static bool bStarted = false;
    if (!bStarted)
    {
        Finder::ResolveMcpProfileQuery();
        bStarted = GameThread::Enqueue(&Tick);
    }
}
