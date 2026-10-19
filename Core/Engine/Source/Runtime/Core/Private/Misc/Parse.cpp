#include "pch.h"
#include "Misc/Parse.h"
#include "Natives.h"

bool FParse::Param(const wchar_t* Stream, const wchar_t* Param)
{
    return Natives::FParse_Param(Stream, Param);
}
