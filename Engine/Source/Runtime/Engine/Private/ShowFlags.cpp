#include "pch.h"
#include "ShowFlags.h"
#include "Internationalization/TextLocalizationManager.h"

#include <string_view>
#include <utility>

namespace
{
    constexpr std::pair<std::wstring_view, std::wstring_view> DisplayNameKeys[] = {
        { L"ContactShadows", L"ContactShadows" },
        { L"ForceFeedbackRadius", L"ForceFeedbackSF" },
        { L"Collision", L"CollisionWireFrame" },
        { L"CollisionVisibility", L"CollisionVisibility" },
        { L"CollisionPawn", L"CollisionPawn" },
        { L"AtmosphericFog", L"AtmosphereSF" },
        { L"PreviewShadowsIndicator", L"PreviewShadowIndicatorSF" },
        { L"VisualizeMeshDistanceFields", L"MeshDistanceFieldsSF" },
        { L"VisualizeGlobalDistanceField", L"GlobalDistanceFieldSF" },
        { L"VisualizeSSR", L"VisualizeSSR" },
        { L"VisualizeShadingModels", L"VisualizeShadingModels" },
        { L"VisualizeSenses", L"VisualizeSenses" },
        { L"VisualizeBloom", L"VisualizeBloom" },
        { L"LODColoration", L"VisualizeLODColoration" },
        { L"HLODColoration", L"VisualizeHLODColoration" },
        { L"ShaderComplexityWithQuadOverdraw", L"ShaderComplexityWithQuadOverdraw" },
        { L"PrimitiveDistanceAccuracy", L"PrimitiveDistanceAccuracy" },
        { L"MeshUVDensityAccuracy", L"MeshUVDensityAccuracy" },
        { L"MaterialTextureScaleAccuracy", L"MaterialTextureScaleAccuracy" },
        { L"OutputMaterialTextureScales", L"OutputMaterialTextureScales" },
        { L"RequiredTextureResolution", L"RequiredTextureResolution" },
    };
}

bool FEngineShowFlags::FindShowFlagDisplayName(const FString& InName, FString& OutText)
{
    FString Key = InName + TEXT("SF");
    for (const auto& [Name, NameKey] : DisplayNameKeys)
    {
        if (Name == std::wstring_view(*InName, InName.Len()))
        {
            Key = FString(NameKey);
            break;
        }
    }
    return FTextLocalizationManager::Get().FindDisplayString(FString(L""), Key, OutText);
}
