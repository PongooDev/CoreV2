#pragma once

#include <string>

struct FCoreConfiguration
{
    std::wstring Backend = L"http://127.0.0.1:5595";
    // -CoreMap=<map URL>: the map a server starts on, with its options (Battle Royale:
    // /Game/Athena/Maps/Athena_Terrain?playlistId=2). The game turns a first argument without a dash into a switch
    // ("-Athena_Terrain"), so StartGameInstance never sees a map URL there.
    std::wstring Map;
};

namespace Configuration
{
    void Load();
    const FCoreConfiguration& Get();
}
