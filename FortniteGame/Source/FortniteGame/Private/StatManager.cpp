#include "pch.h"
#include "StatManager.h"
#include "Finder.h"
#include "Natives.h"

#include <cwchar>
#include <string>

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
    // ModifyStatForAllSources (1.10 0x8F1AF0) names each source's stat "<source>.<Stat>", a source being one of the
    // weapon's AnalyticTags (Weapon.ranged.assault, ...). A weapon has one, so the sum is the player's total.
    const std::wstring Suffix = std::wstring(L".") + Stat;
    int32_t Total = 0;
    const TArray<UStat*>& AllStats = Stats;
    for (UStat* Each : AllStats)
    {
        const FName Name = Each ? static_cast<FName>(Each->StatName) : FName();
        const std::wstring Text = Each ? Name.ToStdString() : std::wstring();
        // Names compare without case, as FNames do.
        if (Text.size() > Suffix.size() && _wcsicmp(Text.c_str() + (Text.size() - Suffix.size()), Suffix.c_str()) == 0)
        {
            Total += GetStatValue(Name, Period);
        }
    }
    return Total;
}
