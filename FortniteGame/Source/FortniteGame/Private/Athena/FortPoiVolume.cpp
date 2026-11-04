#include "pch.h"
#include "Athena/FortPoiVolume.h"
#include "Athena/FortArenaCamera.h"
#include "Athena/FortPoiManager.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingFoundation.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
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
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

// What Epic's server has and the client paks don't (3.0 on)
// ----------------------------------------------------------
// Every AFortPoiVolume is a brush volume: its shape is its BrushComponent's BrushBodySetup (convex shapes built from the
// brush when the level was saved), and the BrushComponent is also its root, so the volume's place on the map is that
// component's transform. The client cook strips the BrushComponent of every POI volume (BrushComponent and RootComponent
// are None in 3.0's Athena_LocationVolumes and Athena_GameplayActors), so on a server running from client paks each volume
// has no shape and no place: AFortPoiVolume::PostInitializeComponents (3.0 0x9FDF20) logs "failed to initalized,
// PoiCollisionComp: TestPrimComp0 GetBrushComponent(): None", PoiCollisionComp's PoiBodyInstance is never made, and its
// OverlapComponent (0x9FA6C0, PoiBodyInstance's OverlapTest) never holds. Nothing then gets a location tag: not the Visit
// events of AFortPoiVolume::CheckPawnOverlap, nor the tags Land, Interact and the eliminations ask the game mode for
// (AFortPoiManager::GetTagsForPawnLocation, 0x9EB7C0). What survives the cook is each volume's LocationTags, its
// PoiCollisionComp, and its brush geometry (ABrush::Brush, a UModel in the volume's own space, so it has a size but no
// place).
//
// Epic's code is left as it is. When a volume has its BrushComponent (server assets put back into the build), Epic's
// PostInitializeComponents made its body and Core leaves it alone. Otherwise Core does what PostInitializeComponents
// does with a shape of its own: a UBodySetup with one box, FBodyInstance::InitBody on PoiBodyInstance with no physics
// scene, as Epic's call does.
//
// Where the box goes is worked out from the running world's own data, with no per-build table (Core-Docs
// ServerAssets/Athena-POI-3.0.md). A volume's subject is the last word of its location tag that isn't a number
// (Athena.Location.POI.TiltedTowers -> TiltedTowers, Athena.Location.Meta.GasStation.03 -> GasStation, number 3). Its
// place is the first of these that names the subject:
// 1. Tags: building actors whose StaticGameplayTags end in the subject (Building.Type.Prop.Vehicle.IceCreamTruck for
//    Athena.Location.Quest.Visit.IceCreamTruck.NN).
// 2. Arena cameras: the AFortArenaCamera spectator cameras whose CameraZone is one of the volume's tags, or whose name
//    holds the subject's words (arenaCam_TiltedTowers3). On 3.0 they are in an on-demand sublevel of the persistent map,
//    which Core loads (not into the world) to read them.
// 3. Levels: the plots (ABuildingFoundation streaming boxes) whose streamed level's name holds the subject's first word
//    (Athena_SUB_3x3_GasStation_a for GasStation, Athena_URB_3x3_TacoStand_a for TacoShop, Athena_REM_3x3_Crab_a for
//    Crab). A match that lies inside another match is the same place (a gas station plot inside a gas-station POI plot),
//    and only the innermost one counts.
// Numbered volumes take the n-th match in order of position (X, then Y): nothing in the data says which number is which,
// and the quests only count different ones. A volume with nothing in the data naming it isn't placed, and is logged.
// The size is the volume's brush (its UModel bounds, at scale 1: the scale is lost with the component), or what it
// covers when that is larger.
namespace
{
    // A place in the world the data ties to a subject.
    struct FPlace
    {
        std::wstring Name;
        FBox Box;
    };

    // How a volume's place was found.
    struct FResolved
    {
        std::wstring Source;
        std::vector<FPlace> Places;
        // Arena cameras: their spread is the place, standing on the lowest of them.
        bool bFromCameras = false;
    };

    std::wstring Lower(std::wstring Text)
    {
        std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t C) { return static_cast<wchar_t>(std::towlower(C)); });
        return Text;
    }

    bool IsNumber(const std::wstring& Text)
    {
        return !Text.empty() && std::all_of(Text.begin(), Text.end(), [](wchar_t C) { return std::iswdigit(C) != 0; });
    }

    // An asset, actor or tag name's words, lower case: "arenaCam_TiltedTowers3" -> arena, cam, tilted, towers, 3;
    // "Athena_POI_GenericPlot_GAS_001" -> athena, poi, generic, plot, gas, 001.
    std::vector<std::wstring> Words(const std::wstring& Name)
    {
        std::vector<std::wstring> Result;
        std::wstring Word;
        const auto Flush = [&]()
        {
            if (!Word.empty())
            {
                Result.push_back(Lower(Word));
                Word.clear();
            }
        };
        for (size_t Index = 0; Index < Name.size(); ++Index)
        {
            const wchar_t C = Name[Index];
            if (!std::iswalnum(C))
            {
                Flush();
                continue;
            }
            if (!Word.empty())
            {
                const wchar_t Previous = Word.back();
                const bool bLowerToUpper = std::iswlower(Previous) && std::iswupper(C);
                const bool bDigitChange = (std::iswdigit(Previous) != 0) != (std::iswdigit(C) != 0);
                if (bLowerToUpper || bDigitChange)
                {
                    Flush();
                }
            }
            Word += C;
        }
        Flush();
        return Result;
    }

    bool ContainsSequence(const std::vector<std::wstring>& Haystack, const std::vector<std::wstring>& Needle)
    {
        return !Needle.empty() && std::search(Haystack.begin(), Haystack.end(), Needle.begin(), Needle.end()) != Haystack.end();
    }

    // What a volume's location tags say it is: the last word of a tag that isn't a number, and that number, if any
    // (Athena.Location.Meta.GasStation.03 -> GasStation, 3).
    struct FSubject
    {
        std::wstring Name;
        int32_t Number = 0;
    };

    FSubject SubjectOf(const AFortPoiVolume* Volume)
    {
        for (const FGameplayTag& Tag : Volume->LocationTags.GameplayTags)
        {
            std::vector<std::wstring> Segments;
            const std::wstring TagName = Tag.TagName.ToStdString();
            for (size_t Start = 0; Start <= TagName.size();)
            {
                const size_t Dot = std::min(TagName.find(L'.', Start), TagName.size());
                Segments.push_back(TagName.substr(Start, Dot - Start));
                Start = Dot + 1;
            }
            FSubject Subject;
            if (!Segments.empty() && IsNumber(Segments.back()))
            {
                Subject.Number = std::stoi(Segments.back());
                Segments.pop_back();
            }
            if (!Segments.empty() && !Segments.back().empty())
            {
                Subject.Name = Segments.back();
                return Subject;
            }
        }
        return {};
    }

    FBox ActorBox(AActor* Actor)
    {
        FVector Origin;
        FVector Extent;
        Actor->GetActorBounds(false, Origin, Extent);
        return FBox(Origin - Extent, Origin + Extent);
    }

    // The asset a streamed level instance was made from: "/Temp/Game/Athena/Maps/POI/Athena_POI_City_001_43" ->
    // "Athena_POI_City_001".
    std::wstring LevelAssetName(const std::wstring& LevelPackage)
    {
        std::wstring Name = LevelPackage.substr(LevelPackage.rfind(L'/') + 1);
        const size_t Suffix = Name.rfind(L'_');
        if (Suffix != std::wstring::npos && IsNumber(Name.substr(Suffix + 1)))
        {
            Name.resize(Suffix);
        }
        return Name;
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

    struct FCamera
    {
        std::wstring Name;
        FVector Location;
        FName Zone;
    };

    // What the world holds that can name a volume's place.
    struct FWorldData
    {
        // Every streamed plot, with its level's asset name and that name's words.
        std::vector<std::pair<FPlace, std::vector<std::wstring>>> Plots;
        std::vector<FCamera> Cameras;
        // Building actors by the last segment of each of their StaticGameplayTags (lower case), for the subjects asked.
        std::map<std::wstring, std::vector<FPlace>> TaggedActors;
        std::wstring CameraSource;
    };

    void GatherPlots(UWorld* World, FWorldData& Data)
    {
        for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, ABuildingFoundation::StaticClass()))
        {
            ABuildingFoundation* Foundation = static_cast<ABuildingFoundation*>(Actor);
            const FName LevelToStream = Foundation ? Foundation->LevelToStream : FName();
            if (LevelToStream.IsNone())
            {
                continue;
            }
            const std::wstring Name = LevelAssetName(LevelToStream.ToStdString());
            Data.Plots.push_back({ FPlace{ Name, PlotBox(Foundation, Foundation->GetTransform()) }, Words(Name) });
        }
    }

    // The arena cameras: those in the world, or, when the world has none, those of the persistent map's sublevels that
    // aren't loaded (3.0's Athena_CameraLocations streams only on demand). Such a level is loaded as a package and read;
    // it isn't added to the world.
    void GatherCameras(UWorld* World, FWorldData& Data)
    {
        UClass* CameraClass = AFortArenaCamera::StaticClass();
        if (!CameraClass)
        {
            return;
        }
        const auto Add = [&Data](AActor* Actor, const FVector& Location)
        {
            AFortArenaCamera* Camera = static_cast<AFortArenaCamera*>(Actor);
            Data.Cameras.push_back({ Actor->GetName().ToStdString(), Location, Camera->HasCameraZone() ? FGameplayTag(Camera->CameraZone).TagName : FName() });
        };
        for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, CameraClass))
        {
            Add(Actor, Actor->GetTransform().Translation);
        }
        if (!Data.Cameras.empty())
        {
            Data.CameraSource = L"the world";
            return;
        }
        for (ULevelStreaming* Streaming : static_cast<TArray<ULevelStreaming*>&>(World->StreamingLevels))
        {
            if (!Streaming || Streaming->LoadedLevel)
            {
                continue;
            }
            const FString Path = static_cast<const TAssetPtr<UWorld>&>(Streaming->WorldAsset).ToStringReference().ToString();
            // Levels a foundation streams are /Temp/ instances, picked and loaded by the game itself.
            if (Path.IsEmpty() || std::wstring(*Path).starts_with(L"/Temp/"))
            {
                continue;
            }
            UWorld* LevelWorld = LoadObject<UWorld>(nullptr, *Path, nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet));
            ULevel* Level = LevelWorld ? static_cast<ULevel*>(LevelWorld->PersistentLevel) : nullptr;
            if (!Level)
            {
                continue;
            }
            const FTransform LevelTransform = Streaming->LevelTransform;
            const size_t Before = Data.Cameras.size();
            for (AActor* Actor : Level->GetActors())
            {
                USceneComponent* Root = Actor && Actor->IsA(CameraClass) ? static_cast<USceneComponent*>(Actor->RootComponent) : nullptr;
                if (Root)
                {
                    Add(Actor, LevelTransform.TransformPosition(Root->RelativeLocation));
                }
            }
            if (Data.Cameras.size() > Before)
            {
                Data.CameraSource += (Data.CameraSource.empty() ? L"" : L", ") + std::wstring(*Path).substr(std::wstring(*Path).rfind(L'/') + 1);
            }
        }
    }

    void GatherTaggedActors(UWorld* World, const std::set<std::wstring>& Subjects, FWorldData& Data)
    {
        for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, ABuildingActor::StaticClass()))
        {
            ABuildingActor* Building = static_cast<ABuildingActor*>(Actor);
            if (!Building)
            {
                continue;
            }
            for (const FGameplayTag& Tag : static_cast<FGameplayTagContainer&>(Building->StaticGameplayTags).GameplayTags)
            {
                const std::wstring TagName = Tag.TagName.ToStdString();
                const std::wstring Leaf = Lower(TagName.substr(TagName.rfind(L'.') + 1));
                if (Subjects.contains(Leaf))
                {
                    Data.TaggedActors[Leaf].push_back({ Actor->GetName().ToStdString(), ActorBox(Actor) });
                    break;
                }
            }
        }
    }

    // Of matches that are the same place (one's centre inside the other), keeps the innermost.
    std::vector<FPlace> Innermost(std::vector<FPlace> Places)
    {
        const auto Area = [](const FBox& Box) { return (Box.Max.X - Box.Min.X) * (Box.Max.Y - Box.Min.Y); };
        const auto ContainsXY = [](const FBox& Box, const FVector& Point) { return Point.X > Box.Min.X && Point.X < Box.Max.X && Point.Y > Box.Min.Y && Point.Y < Box.Max.Y; };
        std::vector<FPlace> Result;
        for (size_t Index = 0; Index < Places.size(); ++Index)
        {
            const bool bHoldsAnother = std::any_of(Places.begin(), Places.end(), [&](const FPlace& Other)
            {
                return &Other != &Places[Index] && Area(Other.Box) < Area(Places[Index].Box) && ContainsXY(Places[Index].Box, Other.Box.GetCenter());
            });
            if (!bHoldsAnother)
            {
                Result.push_back(Places[Index]);
            }
        }
        return Result;
    }

    void SortByPosition(std::vector<FPlace>& Places)
    {
        std::sort(Places.begin(), Places.end(), [](const FPlace& A, const FPlace& B)
        {
            const FVector CA = A.Box.GetCenter();
            const FVector CB = B.Box.GetCenter();
            return CA.X != CB.X ? CA.X < CB.X : CA.Y < CB.Y;
        });
    }

    // The arena cameras of one place, without the odd one far from the rest (a camera of Loot Lake's set looks at it from
    // Tilted Towers): those within twice the median distance from the median point.
    std::vector<FCamera> CameraCluster(std::vector<FCamera> Cameras)
    {
        if (Cameras.size() < 3)
        {
            return Cameras;
        }
        const auto Median = [](std::vector<float> Values)
        {
            std::nth_element(Values.begin(), Values.begin() + Values.size() / 2, Values.end());
            return Values[Values.size() / 2];
        };
        std::vector<float> Xs;
        std::vector<float> Ys;
        for (const FCamera& Camera : Cameras)
        {
            Xs.push_back(Camera.Location.X);
            Ys.push_back(Camera.Location.Y);
        }
        const float MX = Median(Xs);
        const float MY = Median(Ys);
        std::vector<float> Distances;
        for (const FCamera& Camera : Cameras)
        {
            Distances.push_back(std::hypot(Camera.Location.X - MX, Camera.Location.Y - MY));
        }
        const float Limit = Median(Distances) * 2.0f + 1.0f;
        std::vector<FCamera> Kept;
        for (const FCamera& Camera : Cameras)
        {
            if (std::hypot(Camera.Location.X - MX, Camera.Location.Y - MY) <= Limit)
            {
                Kept.push_back(Camera);
            }
        }
        return Kept;
    }

    // Finds where the data puts a volume, or says why it can't (Reason).
    FResolved Resolve(const AFortPoiVolume* Volume, const FSubject& Subject, const FWorldData& Data, std::wstring& Reason)
    {
        FResolved Resolved;
        const std::wstring Key = Lower(Subject.Name);
        const std::vector<std::wstring> SubjectWords = Words(Subject.Name);

        if (const auto Tagged = Data.TaggedActors.find(Key); Tagged != Data.TaggedActors.end() && !Tagged->second.empty())
        {
            Resolved.Source = std::format(L"actors tagged *.{}", Subject.Name);
            Resolved.Places = Tagged->second;
        }

        if (Resolved.Places.empty())
        {
            std::vector<FCamera> Named;
            for (const FCamera& Camera : Data.Cameras)
            {
                const bool bZone = !Camera.Zone.IsNone()
                    && std::any_of(Volume->LocationTags.GameplayTags.begin(), Volume->LocationTags.GameplayTags.end(), [&Camera](const FGameplayTag& Tag) { return Tag.TagName == Camera.Zone; });
                if (bZone || ContainsSequence(Words(Camera.Name), SubjectWords))
                {
                    Named.push_back(Camera);
                }
            }
            if (!Named.empty())
            {
                const std::vector<FCamera> Cluster = CameraCluster(Named);
                FBox Spread;
                for (const FCamera& Camera : Cluster)
                {
                    Spread += Camera.Location;
                }
                Resolved.Source = std::format(L"{} of {} arena cameras ({})", Cluster.size(), Named.size(), Data.CameraSource);
                Resolved.Places.push_back({ Cluster.front().Name, Spread });
                Resolved.bFromCameras = true;
            }
        }

        if (Resolved.Places.empty() && !SubjectWords.empty())
        {
            std::vector<FPlace> Plots;
            for (const auto& [Plot, PlotWords] : Data.Plots)
            {
                if (std::find(PlotWords.begin(), PlotWords.end(), SubjectWords.front()) != PlotWords.end())
                {
                    Plots.push_back(Plot);
                }
            }
            Resolved.Places = Innermost(Plots);
            if (!Resolved.Places.empty())
            {
                Resolved.Source = std::format(L"plots of levels named *{}*", SubjectWords.front());
            }
        }

        if (Resolved.Places.empty())
        {
            Reason = std::format(L"no actor tagged *.{}, no arena camera named {} and no streamed level named *{}*", Subject.Name, Subject.Name, SubjectWords.empty() ? Subject.Name : SubjectWords.front());
            return Resolved;
        }
        if (Subject.Number > 0)
        {
            SortByPosition(Resolved.Places);
            if (Subject.Number > static_cast<int32_t>(Resolved.Places.size()))
            {
                Reason = std::format(L"number {} but the {} give only {} places", Subject.Number, Resolved.Source, Resolved.Places.size());
                Resolved.Places.clear();
                return Resolved;
            }
            const size_t Count = Resolved.Places.size();
            Resolved.Places = { Resolved.Places[Subject.Number - 1] };
            Resolved.Source += std::format(L", no. {} of {} by position", Subject.Number, Count);
        }
        return Resolved;
    }

    // UModel::Bounds, the brush's bounds in the volume's space (an FBoxSphereBounds: Origin, BoxExtent, SphereRadius).
    // UModel has no reflected members, so its offset is found once from the brushes themselves: the one offset where
    // every brush holds a box with positive extents and a sphere radius between its largest extent and the box's
    // diagonal, as the bounds built from the brush's points are.
    ptrdiff_t ModelBoundsOffset(const std::vector<const UObject*>& Models)
    {
        static ptrdiff_t Offset = -1;
        if (Offset >= 0 || Models.empty())
        {
            return Offset;
        }
        const auto Fits = [](const UObject* Model, ptrdiff_t At)
        {
            const float* Values = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(Model) + At);
            if (!Finder::IsReadable(Values, sizeof(float) * 7))
            {
                return false;
            }
            for (int32_t Index = 0; Index < 7; ++Index)
            {
                if (!std::isfinite(Values[Index]) || std::fabs(Values[Index]) > 1.0e7f)
                {
                    return false;
                }
            }
            const float EX = Values[3];
            const float EY = Values[4];
            const float EZ = Values[5];
            const float Radius = Values[6];
            if (EX < 1.0f || EY < 1.0f || EZ < 1.0f)
            {
                return false;
            }
            return Radius >= std::max({ EX, EY, EZ }) - 1.0f && Radius <= std::sqrt(EX * EX + EY * EY + EZ * EZ) + 1.0f;
        };
        for (ptrdiff_t At = sizeof(void*) * 5; At < 0x400; At += 4)
        {
            if (std::all_of(Models.begin(), Models.end(), [&](const UObject* Model) { return Fits(Model, At); }))
            {
                Offset = At;
                Log::Info(L"POI: UModel::Bounds at +0x{:X} (checked on {} brushes)", At, Models.size());
                break;
            }
        }
        return Offset;
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

    int32_t Epics = 0;
    std::vector<AFortPoiVolume*> Shapeless;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, VolumeClass))
    {
        AFortPoiVolume* Volume = static_cast<AFortPoiVolume*>(Actor);
        if (!Volume || FallbackBoxes().contains(Volume))
        {
            continue;
        }
        if (Volume->GetBrushComponent())
        {
            ++Epics;
            Log::Info(L"POI: {} ({}) has its brush (server assets): Epic's own shape, no fallback", Volume->GetName().ToStdString(), Volume->DescribeLocationTags());
            continue;
        }
        Shapeless.push_back(Volume);
    }
    if (Shapeless.empty())
    {
        Log::Info(L"POI: no volume lacks its brush ({} with Epic's)", Epics);
        return 0;
    }

    std::vector<const UObject*> Models;
    std::set<std::wstring> Subjects;
    for (AFortPoiVolume* Volume : Shapeless)
    {
        if (const UObject* Model = reinterpret_cast<const UObject*>(static_cast<UModel*>(Volume->Brush)))
        {
            Models.push_back(Model);
        }
        if (const FSubject Subject = SubjectOf(Volume); !Subject.Name.empty())
        {
            Subjects.insert(Lower(Subject.Name));
        }
    }
    const ptrdiff_t BoundsOffset = ModelBoundsOffset(Models);

    FWorldData Data;
    GatherPlots(World, Data);
    GatherCameras(World, Data);
    GatherTaggedActors(World, Subjects, Data);
    Log::Info(L"POI: {} volumes without a brush; the world has {} streamed plots, {} arena cameras ({}), and {} actors tagged with a volume's subject", Shapeless.size(),
        Data.Plots.size(), Data.Cameras.size(), Data.CameraSource.empty() ? L"none found" : Data.CameraSource.c_str(),
        [&Data]() { size_t Count = 0; for (const auto& [Leaf, Places] : Data.TaggedActors) Count += Places.size(); return Count; }());

    std::vector<FVector> Containers;
    for (AActor* Container : UGameplayStatics::GetAllActorsOfClass(World, ABuildingContainer::StaticClass()))
    {
        Containers.push_back(Container->GetTransform().Translation);
    }

    int32_t Shaped = 0;
    int32_t Unplaced = 0;
    int32_t Overlapping = 0;
    for (AFortPoiVolume* Volume : Shapeless)
    {
        const std::wstring Name = Volume->GetName().ToStdString();
        const FSubject Subject = SubjectOf(Volume);
        std::wstring Reason = L"its location tags name nothing";
        const FResolved Resolved = Subject.Name.empty() ? FResolved() : Resolve(Volume, Subject, Data, Reason);
        if (Resolved.Places.empty())
        {
            ++Unplaced;
            Log::Warning(L"POI: {} ({}) not placed: {}", Name, Volume->DescribeLocationTags(), Reason);
            continue;
        }

        // The brush's size (UModel bounds, at scale 1), or what the volume covers where that is larger.
        FVector BrushExtent;
        const UObject* Model = reinterpret_cast<const UObject*>(static_cast<UModel*>(Volume->Brush));
        if (BoundsOffset >= 0 && Model)
        {
            const float* Bounds = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(Model) + BoundsOffset);
            BrushExtent = FVector(Bounds[3], Bounds[4], Bounds[5]);
        }
        FBox Covered;
        std::wstring PlaceNames;
        for (const FPlace& Place : Resolved.Places)
        {
            Covered += Place.Box;
            PlaceNames += (PlaceNames.empty() ? L"" : L" + ") + Place.Name;
        }
        const FVector Reach = Covered.GetExtent();
        const FVector Extent(std::max(BrushExtent.X, Reach.X), std::max(BrushExtent.Y, Reach.Y), BrushExtent.Z > 0.0f ? BrushExtent.Z : std::max(Reach.Z, 1000.0f));
        const FVector Centre = Covered.GetCenter();
        // It stands on what it covers: from half its height below the base to one and a half above. Arena cameras stand
        // above the ground, so a box from cameras reaches a full height below the lowest.
        const float Base = Covered.Min.Z;
        const float Below = Resolved.bFromCameras ? Extent.Z : Extent.Z * 0.5f;
        const FBox Box(FVector(Centre.X - Extent.X, Centre.Y - Extent.Y, Base - Below), FVector(Centre.X + Extent.X, Centre.Y + Extent.Y, Base + Extent.Z * 1.5f));
        if (!InitPoiBody(Volume, Box))
        {
            ++Unplaced;
            Log::Warning(L"POI: {} ({}) couldn't be given a body", Name, Volume->DescribeLocationTags());
            continue;
        }
        FallbackBoxes()[Volume] = Box;
        ++Shaped;

        // What the manager's overlap test now says: inside at the box's centre, outside well above it.
        const bool bInside = OverlapsPoint(Volume, Box.GetCenter());
        const bool bAbove = OverlapsPoint(Volume, Box.GetCenter() + FVector(0, 0, (Box.Max.Z - Box.Min.Z) + 10000.0f));
        Overlapping += bInside && !bAbove ? 1 : 0;

        const int32_t ContainersInside = static_cast<int32_t>(std::count_if(Containers.begin(), Containers.end(), [&Box](const FVector& Location) { return Box.IsInside(Location); }));
        const float LargeGameVolume = Volume->LargeGameVolume;
        const bool bShouldTickUpdate = LargeGameVolume * LargeGameVolume > (Box.Max - Box.Min).SizeSquared();
        Log::Info(L"POI: {} at {} - {} from {}: {} (brush {}; {} containers inside{}{})", Volume->DescribeLocationTags(), Describe(Box.Min), Describe(Box.Max), Resolved.Source,
            PlaceNames, BoundsOffset >= 0 ? Describe(BrushExtent) : std::wstring(L"unknown"), ContainersInside, bShouldTickUpdate ? L", Visit" : L"",
            bInside && !bAbove ? L"" : L", OVERLAP TEST FAILED");
    }
    Log::Info(L"POI: {} volumes placed from the world's data ({} of {} pass the overlap test), {} with Epic's brush, {} not placed", Shaped, Overlapping, Shaped, Epics, Unplaced);
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
