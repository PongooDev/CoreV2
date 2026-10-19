#include "pch.h"
#include "Misc/CommandLine.h"
#include "Natives.h"

const wchar_t* FCommandLine::Get()
{
    return Natives::FCommandLine_Get();
}
