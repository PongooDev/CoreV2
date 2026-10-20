#include "pch.h"
#include "UObject/Stack.h"
#include "Finder.h"
#include "Natives.h"

void FFrame::Step(UObject* Context, RESULT_DECL)
{
    Natives::FFrame_Step(this, Context, RESULT_PARAM);
}

void FFrame::StepExplicitProperty(void* const Result, UProperty* Property)
{
    Natives::FFrame_StepExplicitProperty(this, Result, Property);
}
