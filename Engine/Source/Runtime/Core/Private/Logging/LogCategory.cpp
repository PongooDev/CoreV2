#include "pch.h"
#include "Logging/LogCategory.h"
#include "Finder.h"
#include "Natives.h"

const FLogCategoryBase* FLogCategoryBinding::Get() const
{
    if (Category)
    {
        return Category;
    }

    if (!bSearchedGame)
    {
        bSearchedGame = true;
        Category = reinterpret_cast<FLogCategoryBase*>(Finder::LogCategory(CategoryName));
        if (Category)
        {
            return Category;
        }
    }

    if (!Natives::FLogCategoryBase_FLogCategoryBase || !Natives::GIsGameThreadIdInitialized || !*Natives::GIsGameThreadIdInitialized)
    {
        return nullptr;
    }

    FLogCategoryBase* Constructed = reinterpret_cast<FLogCategoryBase*>(Storage);
    Natives::FLogCategoryBase_FLogCategoryBase(Constructed, CategoryName, DefaultVerbosity, CompileTimeVerbosity);
    Category = Constructed;
    return Category;
}
