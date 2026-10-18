#pragma once

#include <string>

struct FCoreConfiguration
{
    std::wstring MapToLoad = L"/Game/Maps/FortniteEmptyDedicated";
    int32_t Port = 7777;
    bool bListenServer = false;
    bool bConsole = true;
};

namespace Configuration
{
    void Load();
    const FCoreConfiguration& Get();
}
