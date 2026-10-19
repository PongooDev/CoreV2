#pragma once

#include <string>

struct FCoreConfiguration
{
    std::wstring Backend = L"http://127.0.0.1:5595";
};

namespace Configuration
{
    void Load();
    const FCoreConfiguration& Get();
}
