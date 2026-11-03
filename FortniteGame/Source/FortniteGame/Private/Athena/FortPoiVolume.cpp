#include "pch.h"
#include "Athena/FortPoiVolume.h"
#include "Athena/FortPoiManager.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingFoundation.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameThread.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "Math/Quat.h"
#include "Math/Transform.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cwctype>
#include <format>
#include <map>
#include <string>
#include <vector>

// What Epic's server has and the client paks don't (3.0, CL-3901517)
// ----------------------------------------------------------------------
// Every AFortPoiVolume is a brush volume: its shape is its BrushComponent's BrushBodySetup (convex shapes built from the
// brush when the level was saved), and the BrushComponent is also its root, so the volume's place on the map is that
// component's transform. The client cook strips the BrushComponent of every POI volume (BrushComponent and RootComponent
// are None in Athena_LocationVolumes and Athena_GameplayActors), so on a server running from client paks each volume has
// no shape and no place: AFortPoiVolume::PostInitializeComponents (0x9FDF20) logs "failed to initalized,
// PoiCollisionComp: TestPrimComp0 GetBrushComponent(): None" for all 63, PoiCollisionComp's PoiBodyInstance is never
// made, and its OverlapComponent (0x9FA6C0, PoiBodyInstance's OverlapTest) never holds. Nothing then gets a location tag:
// not the Visit events of AFortPoiVolume::CheckPawnOverlap, nor the tags Land, Interact and the eliminations ask the game
// mode for (AFortPoiManager::GetTagsForPawnLocation, 0x9EB7C0). What survives the cook is each volume's LocationTags, its
// PoiCollisionComp, and its brush geometry (ABrush::Brush, the UModel, in the volume's own space).
//
// Epic's code is left as it is. When a volume has its BrushComponent (server assets put back into the build), Epic's
// PostInitializeComponents made its body and Core leaves it alone. Otherwise Core does what PostInitializeComponents
// does with a shape of its own: a UBodySetup with one box, FBodyInstance::InitBody on PoiBodyInstance with no physics
// scene, as Epic's call does.
//
// Core's fallback shape (Core's own, not Epic's data; Core-Docs/ServerAssets/Athena-POI-3.0.md):
// - Size: the volume's brush, which survives: its bounds at scale 1 (the brush component's scale is lost with it).
// - Place: what the volume covers, found in the client paks. Named locations: the plots of their POI levels
//   (ABuildingFoundation actors stream the POI and building levels, and they keep their transforms; Snobby Shores is its
//   five mansion plots, Lucky Landing the shrine at the map's south end). Gas stations, taco shops and the animal-shaped
//   buildings: the plots of those building levels. Ice cream trucks: the trucks. The box is centred on what it covers and
//   stands on its base (half the brush's height below it, one and a half above); over several plots it is grown to reach
//   them all.
// - Numbering: where several volumes cover one kind of thing (GasStation.01-08, TacoShop.01-04, IceCreamTruck.01-14), the
//   n-th volume takes the n-th one by position (X, then Y). Epic's numbering isn't in the client paks; the quests count
//   different ones, which this keeps apart.
// The 15 "forbidden locations" dance volumes (Athena.Location.Quest.Dance.Forbidden.*) mark spots with nothing in the
// client paks to find them by, so they stay shapeless until the server assets are there.
namespace
{
    enum class EAnchor
    {
        // The plots of the POI levels named (their asset names, exactly).
        PoiLevels,
        // The plots of the building levels whose asset name contains one of the names, the Index-th of them.
        Buildings,
        // The actors of the class named, the Index-th of them.
        Actors,
        // Nothing in the client paks.
        None,
    };

    struct FPoiFallback
    {
        // The volume's actor name (Athena_LocationVolumes, Athena_GameplayActors).
        std::wstring Volume;
        EAnchor Anchor;
        std::vector<std::wstring> Names;
        // Which match (1-based, by position: X, then Y); 0: all of them.
        int32_t Index;
        // Half the brush's size (its UModel bounds, FModel), in the volume's space.
        FVector BrushExtent;
    };

    // Athena_LocationVolumes' 34 volumes and Athena_GameplayActors' 29 (FModel, 3.0).
    const std::vector<FPoiFallback>& PoiFallbacks()
    {
        static const std::vector<FPoiFallback> Fallbacks = []()
        {
            std::vector<FPoiFallback> Table = {
                { L"FortPoiVolume_AnarchyAcres", EAnchor::PoiLevels, { L"Athena_POI_FarmStead_002" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_DustyDepot", EAnchor::PoiLevels, { L"Athena_POI_Warehouses_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_FatalFields", EAnchor::PoiLevels, { L"Athena_POI_FarmStead" }, 0, FVector(8584, 8584, 5000) },
                { L"FortPoiVolume_FlushFactory", EAnchor::PoiLevels, { L"Athena_POI_ToiletFactory_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_GreasyGrove", EAnchor::PoiLevels, { L"Athena_POI_DurrrBurger_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_HauntedHills", EAnchor::PoiLevels, { L"Athena_POI_Cemetery_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_JunkJunction", EAnchor::PoiLevels, { L"Athena_POI_Junkyard_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_LonelyLodge", EAnchor::PoiLevels, { L"Athena_POI_RangerOutpost" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_LootLake", EAnchor::PoiLevels, { L"Athena_POI_Lake_001" }, 0, FVector(9992, 9480, 5000) },
                // Lucky Landing has no POI level of its own in 3.0. The Chinatown-style shrine at the map's south end,
                // below Fatal Fields (Athena_SS_1x1_Shrine_004, one of seven shrine buildings), is where it stands.
                { L"FortPoiVolume_LuckyLanding", EAnchor::PoiLevels, { L"Athena_SS_1x1_Shrine_004" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_MoistyMire", EAnchor::PoiLevels, { L"Athena_POI_Swamp_001" }, 0, FVector(18184, 11528, 5000) },
                { L"FortPoiVolume_PleasantPark", EAnchor::PoiLevels, { L"Athena_POI_CommunityPark_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_RetailRow", EAnchor::PoiLevels, { L"Athena_POI_StripMall", L"Athena_POI_Suburban_015" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_SaltySprings", EAnchor::PoiLevels, { L"Athena_POI_Suburban_006" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_ShiftyShafts", EAnchor::PoiLevels, { L"Athena_POI_Mine_001" }, 0, FVector(5000, 5000, 5000) },
                // Snobby Shores' mansions are the five mid-century modern plots on the west coast.
                { L"FortPoiVolume_SnobbyShores", EAnchor::PoiLevels, { L"Athena_POI_GenericPlot_MCM_001", L"Athena_POI_GenericPlot_MCM_002", L"Athena_POI_GenericPlot_MCM_003",
                    L"Athena_POI_GenericPlot_MCM_004", L"Athena_POI_GenericPlot_MCM_005" }, 0, FVector(6792, 5640, 5000) },
                { L"FortPoiVolume_TiltedTowers", EAnchor::PoiLevels, { L"Athena_POI_City_001" }, 0, FVector(6408, 5640, 5000) },
                { L"FortPoiVolume_TomatoTown", EAnchor::PoiLevels, { L"Athena_POI_Roadside_001" }, 0, FVector(5000, 5000, 5000) },
                { L"FortPoiVolume_WailingWoods", EAnchor::PoiLevels, { L"Athena_POI_HedgeMaze" }, 0, FVector(10504, 11144, 5000) },
                { L"FortPoiVolume_Crab", EAnchor::Buildings, { L"_Crab_" }, 0, FVector(1548, 1548, 1548) },
                { L"FortPoiVolume_Fox", EAnchor::Buildings, { L"_Fox_" }, 0, FVector(1548, 1548, 1548) },
                { L"FortPoiVolume_llama", EAnchor::Buildings, { L"_Llama_" }, 0, FVector(1548, 1548, 1548) },
            };
            for (int32_t Number = 1; Number <= 8; ++Number)
            {
                Table.push_back({ std::format(L"FortPoiVolume_GasStation_{:02}", Number), EAnchor::Buildings, { L"GasStation", L"GenericPlot_GAS" }, Number, FVector(1548, 1548, 1548) });
            }
            for (int32_t Number = 1; Number <= 4; ++Number)
            {
                Table.push_back({ std::format(L"FortPoiVolume_TacoShop_{:02}", Number), EAnchor::Buildings, { L"TacoStand" }, Number, FVector(1548, 1548, 1548) });
            }
            for (int32_t Number = 1; Number <= 14; ++Number)
            {
                Table.push_back({ std::format(L"Quest_Visit_IceCreamTruck_{:02}", Number), EAnchor::Actors, { L"Car_IceCreamTruck_C" }, Number, FVector(625, 625, 625) });
            }
            for (int32_t Number = 1; Number <= 15; ++Number)
            {
                Table.push_back({ std::format(L"Quest_Dance_Forbidden_{:02}", Number), EAnchor::None, {}, 0, FVector(500, 500, 500) });
            }
            return Table;
        }();
        return Fallbacks;
    }

    struct FAnchor
    {
        std::wstring Name;
        FVector Location;
        // Where the thing stands: its plot (a foundation's streaming box moved onto the map) or the actor's bounds.
        FBox Box;
    };

    // The asset a streamed level instance was made from: "/Temp/Game/Athena/Maps/POI/Athena_POI_City_001_43" ->
    // "Athena_POI_City_001".
    std::wstring LevelAssetName(const std::wstring& LevelPackage)
    {
        std::wstring Name = LevelPackage.substr(LevelPackage.rfind(L'/') + 1);
        const size_t Suffix = Name.rfind(L'_');
        if (Suffix != std::wstring::npos && Suffix + 1 < Name.size() && std::all_of(Name.begin() + Suffix + 1, Name.end(), [](wchar_t C) { return std::iswdigit(C) != 0; }))
        {
            Name.resize(Suffix);
        }
        return Name;
    }

    FBox ActorBox(AActor* Actor)
    {
        FVector Origin;
        FVector Extent;
        Actor->GetActorBounds(false, Origin, Extent);
        return FBox(Origin - Extent, Origin + Extent);
    }

    // A foundation's plot: its streaming box (the plot's tile, in the foundation's space) moved onto the map.
    FBox PlotBox(ABuildingFoundation* Foundation, const FTransform& Transform)
    {
        const FBox Local = Foundation->StreamingBoundingBox;
        if (!Local.IsValid || Local.Max.X <= Local.Min.X || Local.Max.Y <= Local.Min.Y)
        {
            return ActorBox(Foundation);
        }
        FBox World;
        for (int32_t Corner = 0; Corner < 8; ++Corner)
        {
            const FVector Point((Corner & 1) ? Local.Max.X : Local.Min.X, (Corner & 2) ? Local.Max.Y : Local.Min.Y, (Corner & 4) ? Local.Max.Z : Local.Min.Z);
            World += Transform.TransformPosition(Point);
        }
        return World;
    }

    void SortByPosition(std::vector<FAnchor>& Anchors)
    {
        std::sort(Anchors.begin(), Anchors.end(), [](const FAnchor& A, const FAnchor& B) { return A.Location.X != B.Location.X ? A.Location.X < B.Location.X : A.Location.Y < B.Location.Y; });
    }

    // Every streamed level's plot, keyed by the level instance's package, with its asset name.
    struct FPlots
    {
        std::vector<FAnchor> All;
        std::map<std::wstring, FAnchor> ByLevelPackage;
    };

    FPlots StreamedPlots(UWorld* World)
    {
        FPlots Plots;
        for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, ABuildingFoundation::StaticClass()))
        {
            ABuildingFoundation* Foundation = static_cast<ABuildingFoundation*>(Actor);
            const FName LevelToStream = Foundation ? Foundation->LevelToStream : FName();
            if (LevelToStream.IsNone())
            {
                continue;
            }
            const FTransform Transform = Foundation->GetTransform();
            const std::wstring Package = LevelToStream.ToStdString();
            FAnchor Plot{ LevelAssetName(Package), Transform.Translation, PlotBox(Foundation, Transform) };
            Plots.ByLevelPackage[Package] = Plot;
            Plots.All.push_back(Plot);
        }
        SortByPosition(Plots.All);
        return Plots;
    }

    std::vector<FAnchor> ActorsOfClassNamed(UWorld* World, const std::wstring& ClassName)
    {
        std::vector<FAnchor> Anchors;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (Class->GetName().ToStdString() != ClassName)
            {
                continue;
            }
            for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, Class))
            {
                Anchors.push_back({ Actor->GetName().ToStdString(), Actor->GetTransform().Translation, ActorBox(Actor) });
            }
            break;
        }
        SortByPosition(Anchors);
        return Anchors;
    }

    std::vector<FAnchor> FindAnchors(UWorld* World, const FPlots& Plots, const FPoiFallback& Fallback)
    {
        std::vector<FAnchor> Found;
        if (Fallback.Anchor == EAnchor::PoiLevels)
        {
            for (const FAnchor& Plot : Plots.All)
            {
                if (std::find(Fallback.Names.begin(), Fallback.Names.end(), Plot.Name) != Fallback.Names.end())
                {
                    Found.push_back(Plot);
                }
            }
            return Found;
        }

        std::vector<FAnchor> Candidates;
        if (Fallback.Anchor == EAnchor::Buildings)
        {
            for (const FAnchor& Plot : Plots.All)
            {
                if (std::any_of(Fallback.Names.begin(), Fallback.Names.end(), [&Plot](const std::wstring& Name) { return Plot.Name.find(Name) != std::wstring::npos; }))
                {
                    Candidates.push_back(Plot);
                }
            }
        }
        else if (Fallback.Anchor == EAnchor::Actors && !Fallback.Names.empty())
        {
            static std::map<std::wstring, std::vector<FAnchor>> ActorCache;
            if (!ActorCache.contains(Fallback.Names[0]))
            {
                ActorCache[Fallback.Names[0]] = ActorsOfClassNamed(World, Fallback.Names[0]);
            }
            Candidates = ActorCache[Fallback.Names[0]];
        }
        if (Fallback.Index == 0)
        {
            return Candidates;
        }
        if (Fallback.Index <= static_cast<int32_t>(Candidates.size()))
        {
            Found.push_back(Candidates[Fallback.Index - 1]);
        }
        return Found;
    }

    std::wstring Describe(const FVector& V)
    {
        return std::format(L"({:.0f}, {:.0f}, {:.0f})", V.X, V.Y, V.Z);
    }

    // The boxes Core gave its volumes, for the manager's tick list and the log.
    std::map<const AFortPoiVolume*, FBox>& FallbackBoxes()
    {
        static std::map<const AFortPoiVolume*, FBox> Boxes;
        return Boxes;
    }

    // Does what AFortPoiVolume::PostInitializeComponents does with a brush component, with Box for its shape.
    bool InitPoiBody(AFortPoiVolume* Volume, const FBox& Box)
    {
        UFortPoiCollisionComponent* PoiCollisionComp = Volume->PoiCollisionComp;
        UBodySetup* BodySetup = PoiCollisionComp ? NewObject<UBodySetup>(PoiCollisionComp, UBodySetup::StaticClass()) : nullptr;
        if (!BodySetup)
        {
            return false;
        }
        // The body keeps only a weak pointer to its setup, as it does to the brush component's.
        BodySetup->AddToRoot();
        FKBoxElem& Elem = BodySetup->AggGeom.BoxElems.AddDefaulted();
        const FVector Size = Box.Max - Box.Min;
        Elem.X = Size.X;
        Elem.Y = Size.Y;
        Elem.Z = Size.Z;

        FTransform Transform;
        Transform.Translation = Box.GetCenter();
        PoiCollisionComp->PoiBodyInstance.InitBody(BodySetup, Transform, PoiCollisionComp, nullptr);
        return true;
    }

    // ECollisionShape::Sphere's FCollisionShape (4.20: the shape type, then the box's three floats or the radius).
    struct FCollisionShapeSphere
    {
        int32_t ShapeType = 2;
        float Radius = 0.0f;
        float Unused[2] = {};
    };

    // UFortPoiCollisionComponent's OverlapComponent override (3.0 0x9FA6C0, slot 0xFC): sub rsp; add rcx,
    // PoiBodyInstance; ...; call FBodyInstance::OverlapTest. The slot CheckPawnOverlap and GetTagsForPawnLocation call.
    int32_t OverlapComponentIndex(UFortPoiCollisionComponent* Component)
    {
        static int32_t Index = -2;
        if (Index != -2)
        {
            return Index;
        }
        Index = -1;
        UObject* Default = UFortPoiCollisionComponent::StaticClass()->GetDefaultObject(false);
        void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
        const int32_t BodyOffset = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Component->PoiBodyInstance) - reinterpret_cast<uintptr_t>(Component));
        for (int32_t Slot = 0; VTable && Slot < 0x200 && Index < 0; ++Slot)
        {
            const uint8_t* Code = static_cast<const uint8_t*>(VTable[Slot]);
            if (Finder::IsReadable(Code, 16) && Code[0] == 0x48 && Code[1] == 0x83 && Code[2] == 0xEC && Code[4] == 0x48 && Code[5] == 0x81 && Code[6] == 0xC1
                && *reinterpret_cast<const int32_t*>(Code + 7) == BodyOffset)
            {
                Index = Slot;
            }
        }
        return Index;
    }

    // Asks the volume's PoiCollisionComp, as CheckPawnOverlap does, whether a 50 cm sphere at Location overlaps it.
    bool OverlapsPoint(AFortPoiVolume* Volume, const FVector& Location)
    {
        UFortPoiCollisionComponent* Component = Volume->PoiCollisionComp;
        const int32_t Index = Component ? OverlapComponentIndex(Component) : -1;
        if (Index < 0)
        {
            return false;
        }
        using FOverlapComponent = bool (*)(UFortPoiCollisionComponent* This, const FVector& Pos, const FQuat& Rot, const FCollisionShapeSphere& Shape);
        const FOverlapComponent OverlapComponent = reinterpret_cast<FOverlapComponent>((*reinterpret_cast<void***>(Component))[Index]);
        FCollisionShapeSphere Sphere;
        Sphere.Radius = 50.0f;
        return OverlapComponent(Component, Location, FQuat(), Sphere);
    }
}

std::wstring AFortPoiVolume::DescribeLocationTags() const
{
    std::wstring Tags;
    for (const FGameplayTag& Tag : LocationTags.GameplayTags)
    {
        Tags += (Tags.empty() ? L"" : L", ") + Tag.TagName.ToStdString();
    }
    return Tags.empty() ? std::wstring(L"no tags") : Tags;
}

FBox AFortPoiVolume::GetFallbackBox() const
{
    const auto Found = FallbackBoxes().find(this);
    return Found != FallbackBoxes().end() ? Found->second : FBox();
}

int32_t AFortPoiVolume::RestorePoiVolumes(UWorld* World)
{
#if WITH_SERVER_CODE
    UClass* VolumeClass = World ? StaticClass() : nullptr;
    if (!VolumeClass || !Finder::ResolvePoiVolumeLive())
    {
        return 0;
    }

    const FPlots Plots = StreamedPlots(World);
    std::vector<FVector> Containers;
    for (AActor* Container : UGameplayStatics::GetAllActorsOfClass(World, ABuildingContainer::StaticClass()))
    {
        Containers.push_back(Container->GetTransform().Translation);
    }

    int32_t Epics = 0;
    int32_t Shaped = 0;
    int32_t Shapeless = 0;
    int32_t Tested = 0;
    int32_t Overlapping = 0;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, VolumeClass))
    {
        AFortPoiVolume* Volume = static_cast<AFortPoiVolume*>(Actor);
        if (!Volume || FallbackBoxes().contains(Volume))
        {
            continue;
        }
        const std::wstring Name = Volume->GetName().ToStdString();
        const std::wstring Level = Volume->GetOutermost() ? Volume->GetOutermost()->GetName().ToStdString() : std::wstring();
        if (Volume->GetBrushComponent())
        {
            ++Epics;
            Log::Info(L"POI: {} ({}) has its brush: Epic's own shape", Name, Volume->DescribeLocationTags());
            continue;
        }

        // A volume of a streamed level stands on the plot that streamed it; the always-loaded ones are in the table.
        std::vector<FAnchor> Anchors;
        FVector BrushExtent(5000, 5000, 5000);
        const std::vector<FPoiFallback>& Fallbacks = PoiFallbacks();
        const auto Fallback = std::find_if(Fallbacks.begin(), Fallbacks.end(), [&Name](const FPoiFallback& Entry) { return Name == Entry.Volume; });
        const auto StreamedBy = Plots.ByLevelPackage.find(Level);
        if (StreamedBy != Plots.ByLevelPackage.end())
        {
            Anchors.push_back(StreamedBy->second);
        }
        else if (Fallback != Fallbacks.end())
        {
            BrushExtent = Fallback->BrushExtent;
            Anchors = FindAnchors(World, Plots, *Fallback);
        }
        if (Anchors.empty())
        {
            ++Shapeless;
            Log::Warning(L"POI: {} ({}, {}) has no brush and nothing in the client paks places it: shapeless until the server assets are there", Name, Volume->DescribeLocationTags(),
                Level.substr(Level.rfind(L'/') + 1));
            continue;
        }

        FBox Covered;
        FBox Spread;
        std::wstring AnchorNames;
        for (const FAnchor& Anchor : Anchors)
        {
            Covered += Anchor.Box;
            Spread += Anchor.Location;
            AnchorNames += (AnchorNames.empty() ? L"" : L" + ") + Anchor.Name;
        }
        // The brush's size, centred on what it covers, standing on its base. A volume over several plots (Retail Row,
        // Snobby Shores) is grown to reach them all, with a margin of six building cells.
        const FVector Reach = Spread.GetExtent() + FVector(3072, 3072, 0);
        const FVector Extent(std::max(BrushExtent.X, Anchors.size() > 1 ? Reach.X : 0.0f), std::max(BrushExtent.Y, Anchors.size() > 1 ? Reach.Y : 0.0f), BrushExtent.Z);
        const FVector Base(Covered.GetCenter().X, Covered.GetCenter().Y, Covered.Min.Z);
        const FBox Box(Base - FVector(Extent.X, Extent.Y, Extent.Z * 0.5f), Base + FVector(Extent.X, Extent.Y, Extent.Z * 1.5f));
        if (!InitPoiBody(Volume, Box))
        {
            ++Shapeless;
            Log::Warning(L"POI: {} ({}) couldn't be given a body", Name, Volume->DescribeLocationTags());
            continue;
        }
        FallbackBoxes()[Volume] = Box;
        ++Shaped;

        // What the manager's overlap test now says: inside at the box's centre, outside well above it.
        const bool bInside = OverlapsPoint(Volume, Box.GetCenter());
        const bool bAbove = OverlapsPoint(Volume, Box.GetCenter() + FVector(0, 0, (Box.Max.Z - Box.Min.Z) + 10000.0f));
        ++Tested;
        Overlapping += bInside && !bAbove ? 1 : 0;

        const int32_t ContainersInside = static_cast<int32_t>(std::count_if(Containers.begin(), Containers.end(), [&Box](const FVector& Location) { return Box.IsInside(Location); }));
        const float LargeGameVolume = Volume->LargeGameVolume;
        const bool bShouldTickUpdate = LargeGameVolume * LargeGameVolume > (Box.Max - Box.Min).SizeSquared();
        Log::Info(L"POI: {} at {} - {} on {} ({} containers inside{}{})", Volume->DescribeLocationTags(), Describe(Box.Min), Describe(Box.Max), AnchorNames, ContainersInside,
            bShouldTickUpdate ? L", Visit" : L"", bInside && !bAbove ? L"" : L", OVERLAP TEST FAILED");
    }
    Log::Info(L"POI: {} volumes given Core's fallback shape ({} of {} pass the overlap test), {} with Epic's brush, {} shapeless; {} streamed plots", Shaped, Overlapping, Tested, Epics,
        Shapeless, Plots.All.size());
    return Shaped;
#else
    return 0;
#endif
}

AFortPoiManager* AFortPoiManager::Get(UWorld* World)
{
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, StaticClass()))
    {
        return static_cast<AFortPoiManager*>(Actor);
    }
    return nullptr;
}

namespace
{
    void WatchPoiManagerOf(FWeakObjectPtr Manager)
    {
        GameThread::Enqueue([Manager]()
        {
            AFortPoiManager* PoiManager = static_cast<AFortPoiManager*>(Manager.Get());
            if (!PoiManager)
            {
                return;
            }
            TArray<AFortPoiVolume*>& AllPoiVolumes = PoiManager->AllPoiVolumes;
            if (AllPoiVolumes.Num() == 0)
            {
                WatchPoiManagerOf(Manager);
                return;
            }

            // InitalizePoiManger has run (HandleMatchHasStarted). It put a volume in SmallTickUpdatedPoi when
            // ShouldTickUpdate held: LargeGameVolume squared above the squared diagonal of the volume's actor bounds. The
            // volumes Core shaped have no brush component for those bounds, so their test is done again on their box.
            TArray<AFortPoiVolume*>& SmallTickUpdatedPoi = PoiManager->SmallTickUpdatedPoi;
            for (AFortPoiVolume* Volume : AllPoiVolumes)
            {
                const FBox Box = Volume ? Volume->GetFallbackBox() : FBox();
                if (!Box.IsValid)
                {
                    continue;
                }
                const float LargeGameVolume = Volume->LargeGameVolume;
                const bool bShouldTickUpdate = LargeGameVolume * LargeGameVolume > (Box.Max - Box.Min).SizeSquared();
                for (int32_t Index = SmallTickUpdatedPoi.Num() - 1; Index >= 0; --Index)
                {
                    if (SmallTickUpdatedPoi[Index] == Volume)
                    {
                        SmallTickUpdatedPoi.RemoveAt(Index);
                    }
                }
                if (bShouldTickUpdate)
                {
                    SmallTickUpdatedPoi.Add(Volume);
                }
            }
            Log::Info(L"POI: the match's POI manager has {} volumes, {} tested for Visit each tick ({} players a frame)", AllPoiVolumes.Num(), SmallTickUpdatedPoi.Num(),
                PoiManager->MaxNumTestsPerFrame);
        });
    }
}

void AFortPoiManager::WatchInitialization(UWorld* World)
{
#if WITH_SERVER_CODE
    AFortPoiManager* Manager = World && !FallbackBoxes().empty() ? Get(World) : nullptr;
    if (!Manager)
    {
        return;
    }
    Log::Info(L"POI: {} lists the volumes when the match starts", Manager->GetName().ToStdString());
    WatchPoiManagerOf(FWeakObjectPtr(Manager));
#endif
}
