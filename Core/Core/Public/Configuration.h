#pragma once

struct FCoreConfiguration
{
    bool bConsole = true;
};

namespace Configuration
{
    void Load();
    const FCoreConfiguration& Get();
}
