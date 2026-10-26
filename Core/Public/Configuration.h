#pragma once

#include <string>

struct FCoreConfiguration
{
    std::wstring Backend = L"http://127.0.0.1:5595";
    // -CoreMap=<map URL>: the map a server starts on, with its options (Battle Royale:
    // /Game/Athena/Maps/Athena_Terrain?playlistId=2). The game turns a first argument without a dash into a switch
    // ("-Athena_Terrain"), so StartGameInstance never sees a map URL there.
    std::wstring Map;
    // -CoreMode=BattleRoyale: the server hosts Battle Royale, on the map and playlist the build's own data names, and
    // registers with the matchmaking service (Core-Docs Athena-Plan.md). Without it the server is Save the World's.
    std::wstring Mode;

    bool IsBattleRoyale() const;
};

namespace Configuration
{
    void Load();
    const FCoreConfiguration& Get();
}
