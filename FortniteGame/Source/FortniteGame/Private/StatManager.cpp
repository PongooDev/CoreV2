#include "pch.h"
#include "StatManager.h"
#include "Finder.h"
#include "Natives.h"

#include <cwchar>
#include <string>

UStat* UStatManager::FindStat(FName StatName)
{
    const TArray<UStat*>& AllStats = Stats;
    for (UStat* Each : AllStats)
    {
        if (Each && static_cast<FName>(Each->StatName) == StatName)
        {
            return Each;
        }
    }
    return nullptr;
}

int32_t UStatManager::GetStatValue(FName StatName, EStatRecordingPeriod Period)
{
    if (!Finder::ResolveMatchStatsLive())
    {
        return 0;
    }
    return Natives::UStatManager_GetStatValue(this, StatName, Period);
}

int32_t UStatManager::GetStatValueForAllSources(const wchar_t* Stat, EStatRecordingPeriod Period)
{
    // ModifyStatForAllSources (1.10 0x8F1AF0; its one-source half 1.11 0x904A80, 2.1 0x909CC0, 2.2 0x8EDB90) names each
    // source's stat Stat + TEXT(":") + source, a source being one of the weapon's AnalyticTags
    // ("Damage.Dealt.ToHostile:Weapon.ranged.assault"). A weapon has one, so the sum is the player's total.
    const std::wstring Prefix = std::wstring(Stat) + L":";
    int32_t Total = 0;
    const TArray<UStat*>& AllStats = Stats;
    for (UStat* Each : AllStats)
    {
        const FName Name = Each ? static_cast<FName>(Each->StatName) : FName();
        const std::wstring Text = Each ? Name.ToStdString() : std::wstring();
        // Names compare without case, as FNames do.
        if (Text.size() > Prefix.size() && _wcsnicmp(Text.c_str(), Prefix.c_str(), Prefix.size()) == 0)
        {
            Total += GetStatValue(Name, Period);
        }
    }
    return Total;
}
