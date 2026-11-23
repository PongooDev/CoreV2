#pragma once

class IPlatformFile;

class FPlatformFileManager
{
public:
    static FPlatformFileManager& Get();

    // The platform file in the chain named Name (FPakPlatformFile::GetTypeName for the pak one), or null.
    IPlatformFile* FindPlatformFile(const wchar_t* Name);
};
