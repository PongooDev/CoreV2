#include "pch.h"
#include "Quests/FortQuestManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Configuration.h"
#include "Containers/ScriptArray.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "GameFramework/GameModeBase.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Natives.h"
#include "Online/FortMcpProfileAccount.h"
#include "Online/FortMcpProfileWorld.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/AssetPtr.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    void (*SendStatEventWithTagsOG)(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush) = nullptr;

    // Battle Royale quest managers holding progress from events that asked for no flush, and when it is saved.
    std::vector<std::pair<FWeakObjectPtr, std::chrono::steady_clock::time_point>> DueSaves;
    constexpr auto SaveDelay = std::chrono::seconds(5);

#if WITH_SERVER_CODE
    void SaveStatEvents(UFortQuestManager* QuestManager);
#endif

    // The quest manager's flush: its pending progress goes to its profile as UpdateQuests, nothing is sent when none is
    // pending. The completion delegate it takes by value is left unbound. Builds that strip it save it through Core's.
    void SaveProgress(UFortQuestManager* QuestManager)
    {
#if WITH_SERVER_CODE
        if (!Natives::UFortQuestManager_FlushStatEvents)
        {
            SaveStatEvents(QuestManager);
            return;
        }
#endif
        alignas(16) uint8_t OnComplete[0x40] = {};
        Natives::UFortQuestManager_FlushStatEvents(QuestManager, OnComplete);
    }

    void SaveWhenDue()
    {
        GameThread::Enqueue([]
        {
            const auto Now = std::chrono::steady_clock::now();
            std::erase_if(DueSaves, [Now](const auto& Due)
            {
                if (Due.second > Now)
                {
                    return false;
                }
                if (UFortQuestManager* QuestManager = static_cast<UFortQuestManager*>(Due.first.Get()))
                {
                    SaveProgress(QuestManager);
                }
                return true;
            });
            if (!DueSaves.empty())
            {
                SaveWhenDue();
            }
        });
    }
}

namespace
{
    // An objective's new count for the player's client (AFortPlayerController::UpdatedObjectiveStats, replicated: its
    // OnRep shows the quest update). Quest is the quest's definition.
    struct FFortUpdatedObjectiveStat
    {
        DefineUnrealStruct(FFortUpdatedObjectiveStat, FortniteGame)
        DefineUProperty(UFortItemDefinition*, Quest)
        DefineUProperty(FName, BackendName)
        DefineUProperty(int32_t, StatValue)
    };

    class AFortPlayerControllerQuests : public UObject
    {
        DefineUnrealClass(AFortPlayerController, FortniteGame)

    public:
        DefineUStructArrayProperty(FFortUpdatedObjectiveStat, UpdatedObjectiveStats)
        static bool HasUpdatedObjectiveStats() { return Member_UpdatedObjectiveStats.IsAvailable(); }
    };

    // The objective's new count shows on the player's client, as SendStatEventWithTags gives it (1.9 0x866A00, 1.9.1
    // 0x869010: the entry for the quest and objective, else a new one).
    void ShowObjectiveCount(AFortPlayerController* PlayerController, UFortItemDefinition* QuestDefinition, FName BackendName, int32_t Count)
    {
        if (!PlayerController || !AFortPlayerControllerQuests::HasUpdatedObjectiveStats())
        {
            return;
        }
        TStructArray<FFortUpdatedObjectiveStat>& Updated = reinterpret_cast<AFortPlayerControllerQuests*>(PlayerController)->UpdatedObjectiveStats;
        FFortUpdatedObjectiveStat* Stat = nullptr;
        for (int32_t Index = 0; Index < Updated.Num() && !Stat; ++Index)
        {
            if (Updated[Index].Quest == QuestDefinition && Updated[Index].BackendName == BackendName)
            {
                Stat = &Updated[Index];
            }
        }
        if (!Stat)
        {
            Stat = &Updated.AddDefaulted();
            Stat->Quest = QuestDefinition;
            Stat->BackendName = BackendName;
        }
        Stat->StatValue = Count;
        PlayerController->ForceNetUpdate();
    }
}

#if WITH_SERVER_CODE
// Core's SendStatEventWithTags and SaveStatEvents for builds that strip them (1.10), as 1.9.1's are (0x8CAC40, 0x8CBE80).
namespace
{
    // The item events an objective (ItemEvent) can listen to instead of a stat row; Max_None is none.
    class EFortQuestObjectiveItemEvent
    {
        DefineUnrealEnum(EFortQuestObjectiveItemEvent, FortniteGame)
        DefineUEnumValue(Craft)
        DefineUEnumValue(Collect)
        DefineUEnumValue(Max_None)
    };

    // A row of the objective stat table (Quests/ObjectiveStatTable): the event type and the tags the event must have, as
    // comma-separated names, and a condition on the event's target, the player and the zone.
    struct FFortQuestObjectiveStatTableRow
    {
        DefineUnrealStruct(FFortQuestObjectiveStatTableRow, FortniteGame)
        DefineUProperty(uint8_t, Type)
        DefineUProperty(FString, TargetTags)
        DefineUProperty(FString, SourceTags)
        DefineUProperty(FString, ContextTags)
        DefineUProperty(FString, Condition)
    };

    // One objective of a quest definition.
    struct FFortMcpQuestObjectiveInfo
    {
        DefineUnrealStruct(FFortMcpQuestObjectiveInfo, FortniteGame)
        DefineUProperty(FName, BackendName)
        DefineUStructProperty(FDataTableRowHandle, ObjectiveStatHandle)
        DefineUStructArrayProperty(FDataTableRowHandle, AlternativeStatHandles)
        DefineUProperty(uint8_t, ItemEvent)
        DefineUProperty(TAssetPtr<UFortItemDefinition>, ItemReference)
        // The count that completes the objective.
        DefineUProperty(int32_t, Count)
        DefineUProperty(int32_t, Stage)
    };

    class UFortQuestItemDefinitionObjectives : public UFortItemDefinition
    {
        DefineUnrealClass(UFortQuestItemDefinition, FortniteGame)

    public:
        DefineUStructArrayProperty(FFortMcpQuestObjectiveInfo, Objectives)
    };

    class UFortMcpProfileQuest : public UObject
    {
        DefineUnrealClass(UFortMcpProfileQuest, FortniteGame)
        DefineUFunction(UpdateQuests)

    public:
        // Sends UpdateQuests as the dedicated server; Advance (TArray<FFortQuestObjectiveCompletion>) is moved into the
        // command, and Completed is called with the quest manager when the profile answers.
        bool UpdateQuests(FScriptArray& Advance, UObject* Object, void (*Completed)(UObject* Object, const void* Result))
        {
            Reflection::FParams Params(StaticFunction_UpdateQuests(), true);
            FScriptArray* AdvanceParameter = Params.Get<FScriptArray>(L"Advance");
            FDedicatedServerUrlContext* Context = Params.Get<FDedicatedServerUrlContext>(L"Context");
            if (!AdvanceParameter || !Context)
            {
                return false;
            }
            std::swap(*AdvanceParameter, Advance);
            if (Finder::ResolveOutpostMcpRequestLive())
            {
                Context->Bind(Object, Completed);
            }
            return Params.Call(this);
        }
    };

    // What the conditions read from the zone, the player and the event's target (1.9.1 0x874590).
    struct FGameDifficultyInfo
    {
        DefineUnrealStruct(FGameDifficultyInfo, FortniteGame)
        DefineUProperty(int32_t, ContentAccountLevel)
        DefineUProperty(float, Difficulty)
    };

    class AFortGameStateZoneQuests : public UObject
    {
        DefineUnrealClass(AFortGameStateZone, FortniteGame)

    public:
        DefineUStructProperty(FDataTableRowHandle, ZoneDifficultyInfoRow)
    };

    class AFortPawnQuests : public UObject
    {
        DefineUnrealClass(AFortPawn, FortniteGame)

    public:
        DefineUProperty(FGameplayTagContainer, GameplayTags)
    };

    class AGameModeBaseQuests : public UObject
    {
        DefineUnrealClass(AGameModeBase, Engine)
        DefineUFunction(GetNumPlayers)

    public:
        int32_t GetNumPlayers()
        {
            Reflection::FParams Params(StaticFunction_GetNumPlayers());
            return Params.Call(this) ? Params.Take<int32_t>(L"ReturnValue") : 0;
        }
    };

    bool EqualsIgnoreCase(const std::wstring& A, const std::wstring& B)
    {
        return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin(), [](wchar_t X, wchar_t Y) { return std::towlower(X) == std::towlower(Y); });
    }

    std::wstring Lower(std::wstring Text)
    {
        std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Character) { return static_cast<wchar_t>(std::towlower(Character)); });
        return Text;
    }

    // The variables a condition can name, lower case (names are case-insensitive, as FNames are).
    struct FConditionContext
    {
        std::unordered_map<std::wstring, double> Numbers;
        std::unordered_map<std::wstring, const FGameplayTagContainer*> Tags;
    };

    // The quest conditions' expressions (FortQuestFilterExpressionEvaluator): numbers and variables compared with == !=
    // < <= > >=, a tag variable's HasTag, joined with && || ! (AND OR NOT) and parentheses. A variable the context
    // doesn't hold is an error, and the condition then doesn't hold (the game's evaluation fails the same way).
    class FConditionEvaluator
    {
    public:
        FConditionEvaluator(const std::wstring& InText, const FConditionContext& InContext)
            : Text(InText), Context(InContext)
        {
        }

        std::optional<bool> Evaluate()
        {
            const std::optional<bool> Result = ParseOr();
            SkipSpace();
            if (!Result || Position != Text.size())
            {
                return Error.empty() ? Fail(L"unexpected '" + Text.substr(Position) + L"'") : std::nullopt;
            }
            return Result;
        }

        const std::wstring& GetError() const { return Error; }

    private:
        struct FOperand
        {
            std::wstring Token;
            std::optional<double> Number;
            const FGameplayTagContainer* Tags = nullptr;
        };

        std::optional<bool> Fail(const std::wstring& Message)
        {
            if (Error.empty())
            {
                Error = Message;
            }
            return std::nullopt;
        }

        void SkipSpace()
        {
            while (Position < Text.size() && std::iswspace(Text[Position]))
            {
                ++Position;
            }
        }

        bool Accept(const wchar_t* Symbol)
        {
            SkipSpace();
            const size_t Length = std::wcslen(Symbol);
            if (Text.compare(Position, Length, Symbol) != 0)
            {
                return false;
            }
            // A word (AND, OR, NOT, HasTag) only as a whole word.
            if (std::iswalpha(Symbol[0]) && Position + Length < Text.size() && (std::iswalnum(Text[Position + Length]) || Text[Position + Length] == L'_' || Text[Position + Length] == L'.'))
            {
                return false;
            }
            Position += Length;
            return true;
        }

        bool AcceptWord(const wchar_t* Word)
        {
            SkipSpace();
            const size_t Length = std::wcslen(Word);
            if (Position + Length > Text.size() || !EqualsIgnoreCase(Text.substr(Position, Length), Word))
            {
                return false;
            }
            if (Position + Length < Text.size() && (std::iswalnum(Text[Position + Length]) || Text[Position + Length] == L'_' || Text[Position + Length] == L'.'))
            {
                return false;
            }
            Position += Length;
            return true;
        }

        std::optional<bool> ParseOr()
        {
            std::optional<bool> Left = ParseAnd();
            while (Left && (Accept(L"||") || AcceptWord(L"OR")))
            {
                const std::optional<bool> Right = ParseAnd();
                if (!Right)
                {
                    return std::nullopt;
                }
                Left = *Left || *Right;
            }
            return Left;
        }

        std::optional<bool> ParseAnd()
        {
            std::optional<bool> Left = ParseUnary();
            while (Left && (Accept(L"&&") || AcceptWord(L"AND")))
            {
                const std::optional<bool> Right = ParseUnary();
                if (!Right)
                {
                    return std::nullopt;
                }
                Left = *Left && *Right;
            }
            return Left;
        }

        std::optional<bool> ParseUnary()
        {
            SkipSpace();
            if (Position + 1 < Text.size() && Text[Position] == L'!' && Text[Position + 1] != L'=')
            {
                ++Position;
                const std::optional<bool> Operand = ParseUnary();
                return Operand ? std::optional<bool>(!*Operand) : std::nullopt;
            }
            if (AcceptWord(L"NOT"))
            {
                const std::optional<bool> Operand = ParseUnary();
                return Operand ? std::optional<bool>(!*Operand) : std::nullopt;
            }
            return ParseComparison();
        }

        std::optional<FOperand> ParseOperand()
        {
            SkipSpace();
            const size_t Start = Position;
            while (Position < Text.size() && (std::iswalnum(Text[Position]) || Text[Position] == L'_' || Text[Position] == L'.' || Text[Position] == L'-'))
            {
                ++Position;
            }
            if (Position == Start)
            {
                Fail(Position < Text.size() ? L"unexpected '" + Text.substr(Position) + L"'" : L"an operand is missing");
                return std::nullopt;
            }

            FOperand Operand;
            Operand.Token = Text.substr(Start, Position - Start);
            const std::wstring Key = Lower(Operand.Token);
            wchar_t* End = nullptr;
            const double Value = std::wcstod(Operand.Token.c_str(), &End);
            if (End && *End == L'\0')
            {
                Operand.Number = Value;
            }
            else if (const auto Number = Context.Numbers.find(Key); Number != Context.Numbers.end())
            {
                Operand.Number = Number->second;
            }
            else if (const auto Tags = Context.Tags.find(Key); Tags != Context.Tags.end())
            {
                Operand.Tags = Tags->second;
            }
            return Operand;
        }

        std::optional<bool> ParseComparison()
        {
            SkipSpace();
            if (Accept(L"("))
            {
                const std::optional<bool> Inner = ParseOr();
                if (!Inner)
                {
                    return std::nullopt;
                }
                return Accept(L")") ? Inner : Fail(L"a ')' is missing");
            }

            const std::optional<FOperand> Left = ParseOperand();
            if (!Left)
            {
                return std::nullopt;
            }
            if (AcceptWord(L"HasTag"))
            {
                const std::optional<FOperand> Tag = ParseOperand();
                if (!Tag)
                {
                    return std::nullopt;
                }
                if (!Left->Tags)
                {
                    return Fail(L"'" + Left->Token + L"' isn't a tag variable here");
                }
                FGameplayTag Wanted;
                Wanted.TagName = FName(Tag->Token.c_str(), FNAME_Find);
                return Wanted.IsValid() && Left->Tags->HasTag(Wanted);
            }

            static constexpr const wchar_t* Operators[] = { L"==", L"!=", L"<=", L">=", L"<", L">", L"=" };
            const wchar_t* Operator = nullptr;
            for (const wchar_t* Candidate : Operators)
            {
                if (!Operator && Accept(Candidate))
                {
                    Operator = Candidate;
                }
            }
            if (!Operator)
            {
                if (!Left->Number)
                {
                    return Fail(L"'" + Left->Token + L"' isn't a variable here");
                }
                return *Left->Number != 0.0;
            }

            const std::optional<FOperand> Right = ParseOperand();
            if (!Right)
            {
                return std::nullopt;
            }
            if (!Left->Number || !Right->Number)
            {
                return Fail(L"'" + (Left->Number ? Right->Token : Left->Token) + L"' isn't a variable here");
            }
            const double A = *Left->Number;
            const double B = *Right->Number;
            const std::wstring Op = Operator;
            return Op == L"==" || Op == L"=" ? A == B : Op == L"!=" ? A != B : Op == L"<=" ? A <= B : Op == L">=" ? A >= B : Op == L"<" ? A < B : A > B;
        }

        const std::wstring& Text;
        const FConditionContext& Context;
        size_t Position = 0;
        std::wstring Error;
    };

    // The context of an event's conditions, as 1.9.1 builds it (0x874590): the target's variables (a pawn's tags, an
    // item's tags, tier and rarity), the player's placement (Athena.Rank), the players in the match, and the zone's
    // difficulty. Variables that need the game's own getters Core doesn't have (Pawn.Level, Building.*, Mission.*,
    // Account.*, Team.Size) are left out, so conditions on them don't hold.
    FConditionContext MakeConditionContext(UObject* TargetObject, AFortPlayerController* PlayerController)
    {
        FConditionContext Context;
        if (AFortPawn* Pawn = Cast<AFortPawn>(TargetObject))
        {
            Context.Tags[L"pawn.tags"] = &static_cast<FGameplayTagContainer&>(reinterpret_cast<AFortPawnQuests*>(Pawn)->GameplayTags);
        }
        else if (UFortItemDefinition* Item = Cast<UFortItemDefinition>(TargetObject))
        {
            Context.Tags[L"item.tags"] = &static_cast<FGameplayTagContainer&>(Item->GameplayTags);
            Context.Numbers[L"item.tier"] = static_cast<uint8_t>(Item->Tier);
            Context.Numbers[L"item.rarity"] = Item->GetRarity();
        }

        if (AFortPlayerStateAthena* PlayerState = PlayerController ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))) : nullptr)
        {
            Context.Numbers[L"athena.rank"] = static_cast<int32_t>(PlayerState->Place);
        }
        UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
        if (UObject* GameMode = World ? reinterpret_cast<UObject*>(World->GetAuthGameMode()) : nullptr)
        {
            Context.Numbers[L"player.count"] = reinterpret_cast<AGameModeBaseQuests*>(GameMode)->GetNumPlayers();
        }
        UObject* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        if (GameState)
        {
            FDataTableRowHandle& Handle = reinterpret_cast<AFortGameStateZoneQuests*>(GameState)->ZoneDifficultyInfoRow;
            UDataTable* Table = Handle.DataTable;
            const FName RowName = Handle.RowName;
            if (const FGameDifficultyInfo* Info = Table && !RowName.IsNone() ? Table->FindRow<FGameDifficultyInfo>(RowName, L"AFortGameStateZone::GetZoneDifficultyInfo", false) : nullptr)
            {
                Context.Numbers[L"zone.difficulty"] = static_cast<float>(Info->Difficulty);
                Context.Numbers[L"zone.contentaccountlevel"] = static_cast<int32_t>(Info->ContentAccountLevel);
            }
        }
        return Context;
    }

    // A stat row's tags. Rows the game cached (bIsCached) were parsed the same way; Core parses each row once.
    struct FRowTags
    {
        std::vector<FName> Target;
        std::vector<FName> Source;
        std::vector<FName> Context;
    };

    std::vector<FName> ParseTagNames(const FString& Names)
    {
        std::vector<FName> Result;
        std::wstring Text = Names.ToStdString();
        size_t Start = 0;
        while (Start <= Text.size())
        {
            const size_t Comma = Text.find(L',', Start);
            std::wstring Name = Text.substr(Start, Comma == std::wstring::npos ? std::wstring::npos : Comma - Start);
            Name.erase(0, Name.find_first_not_of(L" \t"));
            Name.erase(Name.find_last_not_of(L" \t") + 1);
            if (!Name.empty())
            {
                // A name no tag has can't match any event's tags.
                Result.push_back(FName(Name.c_str(), FNAME_Find));
            }
            if (Comma == std::wstring::npos)
            {
                break;
            }
            Start = Comma + 1;
        }
        return Result;
    }

    const FRowTags& RowTagsOf(const FFortQuestObjectiveStatTableRow* Row)
    {
        static std::unordered_map<const void*, FRowTags> Cache;
        auto [Found, bAdded] = Cache.try_emplace(Row);
        if (bAdded)
        {
            Found->second.Target = ParseTagNames(Row->TargetTags);
            Found->second.Source = ParseTagNames(Row->SourceTags);
            Found->second.Context = ParseTagNames(Row->ContextTags);
        }
        return Found->second;
    }

    bool HasAllTags(const std::vector<FName>& Wanted, const FGameplayTagContainer& Tags)
    {
        return std::all_of(Wanted.begin(), Wanted.end(), [&Tags](FName Name)
        {
            FGameplayTag Tag;
            Tag.TagName = Name;
            return !Name.IsNone() && Tags.HasTag(Tag);
        });
    }

    const FFortQuestObjectiveStatTableRow* StatRowOf(const FDataTableRowHandle& Handle, const wchar_t* ContextString)
    {
        UDataTable* Table = Handle.DataTable;
        const FName RowName = Handle.RowName;
        return Table && !RowName.IsNone() ? Table->FindRow<FFortQuestObjectiveStatTableRow>(RowName, ContextString, false) : nullptr;
    }

    // A stat row takes the event when it is of the event's type and the event has every tag the row names (1.9.1
    // 0x87C830).
    bool RowMatches(const FFortQuestObjectiveStatTableRow* Row, uint8_t Type, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags)
    {
        if (!Row || static_cast<uint8_t>(Row->Type) != Type)
        {
            return false;
        }
        const FRowTags& Tags = RowTagsOf(Row);
        return HasAllTags(Tags.Target, TargetTags) && HasAllTags(Tags.Source, SourceTags) && HasAllTags(Tags.Context, ContextTags);
    }

    // The objective's row or one of its alternatives takes the event (1.9.1 0x87C750).
    bool ObjectiveMatches(FFortMcpQuestObjectiveInfo& Objective, uint8_t Type, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, const wchar_t* ContextString)
    {
        if (RowMatches(StatRowOf(Objective.ObjectiveStatHandle, ContextString), Type, TargetTags, SourceTags, ContextTags))
        {
            return true;
        }
        TStructArray<FDataTableRowHandle>& Alternatives = Objective.AlternativeStatHandles;
        for (int32_t Index = 0; Index < Alternatives.Num(); ++Index)
        {
            if (RowMatches(StatRowOf(Alternatives[Index], ContextString), Type, TargetTags, SourceTags, ContextTags))
            {
                return true;
            }
        }
        return false;
    }

    // The player controller a registered player's quest manager belongs to (1.9.1 reads it from the player info,
    // 0x630450).
    AFortPlayerController* PlayerControllerOf(UFortRegisteredPlayerInfo* PlayerInfo)
    {
        static std::unordered_map<UFortRegisteredPlayerInfo*, FWeakObjectPtr> Known;
        if (const auto Found = Known.find(PlayerInfo); Found != Known.end())
        {
            AFortPlayerController* Cached = static_cast<AFortPlayerController*>(Found->second.Get());
            if (Cached && static_cast<UFortRegisteredPlayerInfo*>(Cached->MyPlayerInfo) == PlayerInfo)
            {
                return Cached;
            }
            Known.erase(Found);
        }
        for (TObjectIterator<AFortPlayerController> It; It; ++It)
        {
            if (static_cast<UFortRegisteredPlayerInfo*>(It->MyPlayerInfo) == PlayerInfo)
            {
                Known[PlayerInfo] = FWeakObjectPtr(*It);
                return *It;
            }
        }
        return nullptr;
    }

    // UpdateQuests sent and not yet answered (1.9.1's SaveRequests): their counts are the profile's already, but not
    // yet in the quests' CompletionCounts.
    struct FSaveRequests
    {
        FWeakObjectPtr QuestManager;
        std::deque<std::vector<std::pair<std::wstring, int32_t>>> Requests;
    };
    std::vector<FSaveRequests> SavesInFlight;

    FSaveRequests* SaveRequestsOf(UFortQuestManager* QuestManager, bool bAdd)
    {
        std::erase_if(SavesInFlight, [](const FSaveRequests& Saves) { return !Saves.QuestManager.Get(); });
        const FWeakObjectPtr Weak(QuestManager);
        for (FSaveRequests& Saves : SavesInFlight)
        {
            if (Saves.QuestManager == Weak)
            {
                return &Saves;
            }
        }
        return bAdd ? &SavesInFlight.emplace_back(FSaveRequests{ Weak, {} }) : nullptr;
    }

    // 1.9.1 0x8BC420.
    int32_t CountInFlight(UFortQuestManager* QuestManager, const std::wstring& StatName)
    {
        int32_t Count = 0;
        if (FSaveRequests* Saves = SaveRequestsOf(QuestManager, false))
        {
            for (const auto& Request : Saves->Requests)
            {
                for (const auto& [Name, Advanced] : Request)
                {
                    Count += EqualsIgnoreCase(Name, StatName) ? Advanced : 0;
                }
            }
        }
        return Count;
    }

    // 1.9.1 0x8BDEA0: the pending progress of a stat, matched ignoring case.
    FFortQuestObjectiveCompletion* FindPending(UFortQuestManager* QuestManager, const std::wstring& StatName)
    {
        TStructArray<FFortQuestObjectiveCompletion>& Pending = QuestManager->PendingChanges;
        for (int32_t Index = 0; Index < Pending.Num(); ++Index)
        {
            if (EqualsIgnoreCase(static_cast<FString&>(Pending[Index].StatName).ToStdString(), StatName))
            {
                return &Pending[Index];
            }
        }
        return nullptr;
    }

    FFortQuestObjectiveCompletion& AddPending(UFortQuestManager* QuestManager, const std::wstring& StatName)
    {
        FFortQuestObjectiveCompletion* Entry = FindPending(QuestManager, StatName);
        if (!Entry)
        {
            TStructArray<FFortQuestObjectiveCompletion>& Pending = QuestManager->PendingChanges;
            Entry = &Pending.AddDefaulted();
            Entry->StatName = FString(StatName.c_str());
            Entry->Count = 0;
        }
        return *Entry;
    }

    // The profile answered an UpdateQuests (1.9.1 0x8BF800): the request is done, and when it failed its progress is
    // pending again.
    void OnStatEventsSaved(UObject* Object, const void* Result)
    {
        UFortQuestManager* QuestManager = static_cast<UFortQuestManager*>(Object);
        const bool bSuccess = Result && *static_cast<const uint8_t*>(Result) != 0;
        FSaveRequests* Saves = QuestManager ? SaveRequestsOf(QuestManager, false) : nullptr;
        if (!Saves || Saves->Requests.empty())
        {
            return;
        }
        const std::vector<std::pair<std::wstring, int32_t>> Request = std::move(Saves->Requests.front());
        Saves->Requests.pop_front();
        if (!bSuccess)
        {
            Log::Warning(L"UpdateQuests for {} failed, {} update(s) placed back into the queue", QuestManager->GetName().ToStdString(), Request.size());
            for (const auto& [StatName, Count] : Request)
            {
                FFortQuestObjectiveCompletion& Entry = AddPending(QuestManager, StatName);
                Entry.Count = Entry.Count + Count;
            }
        }
    }

    // 1.9.1 0x8CBE80: the progress pending goes to the bound profile as UpdateQuests, through the dedicated server's
    // command URL.
    void SaveStatEvents(UFortQuestManager* QuestManager)
    {
        TStructArray<FFortQuestObjectiveCompletion>& Pending = QuestManager->PendingChanges;
        if (Pending.Num() == 0)
        {
            return;
        }
        UFortMcpProfileQuest* Profile = Cast<UFortMcpProfileQuest>(static_cast<FWeakObjectPtr&>(QuestManager->BoundProfile).Get());
        if (!Profile)
        {
            Log::Warning(L"SaveStatEvents for {}: no profile is bound, the progress stays pending", QuestManager->GetName().ToStdString());
            return;
        }

        std::vector<std::pair<std::wstring, int32_t>> Request;
        std::wstring Text;
        for (int32_t Index = 0; Index < Pending.Num(); ++Index)
        {
            Request.emplace_back(static_cast<FString&>(Pending[Index].StatName).ToStdString(), static_cast<int32_t>(Pending[Index].Count));
            Text += std::format(L"{}{}:{}", Text.empty() ? L"" : L", ", Request.back().first, Request.back().second);
        }
        Log::Info(L"SaveStatEvents for {}: {}", QuestManager->GetName().ToStdString(), Text);

        FSaveRequests* Saves = SaveRequestsOf(QuestManager, true);
        Saves->Requests.push_back(Request);
        FScriptArray& Advance = Pending;
        if (!Profile->UpdateQuests(Advance, QuestManager, &OnStatEventsSaved))
        {
            Saves->Requests.pop_back();
            Log::Error(L"SaveStatEvents for {}: UpdateQuests couldn't be sent", QuestManager->GetName().ToStdString());
        }
    }

    // 1.9.1 0x8CAC40 (SendStatEventWithTags): every active quest's objectives of its current stage that listen to the
    // event (its stat row or an alternative, an item event's item, or the custom stat itself, and the row's condition)
    // add the event's count to the quest manager's pending progress for their BackendName, once per name; the player's
    // client is shown each objective's new count. An event that asked for a flush, or completed an objective, saves the
    // progress, except for Battle Royale's controllers (UFortQuestManager::SendStatEventWithTagsHK saves theirs).
    void SendStatEventWithTagsCore(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
    {
        UFortRegisteredPlayerInfo* PlayerInfo = This ? Cast<UFortRegisteredPlayerInfo>(This->GetOuter()) : nullptr;
        AFortPlayerController* PlayerController = PlayerInfo ? PlayerControllerOf(PlayerInfo) : nullptr;
        if (!PlayerController || !UFortQuestManager::HasCurrentQuests())
        {
            return;
        }

        const FDataTableRowHandle* CustomStat = static_cast<const FDataTableRowHandle*>(CustomStatName);
        const bool bCustom = CustomStat && (static_cast<UDataTable*>(CustomStat->DataTable) || !static_cast<FName>(CustomStat->RowName).IsNone());
        const uint8_t NumGameplayEvents = EFortQuestObjectiveStatEvent::GetNumGameplayEvents();
        if (!bCustom && Type >= NumGameplayEvents)
        {
            Log::Warning(L"SendStatEventWithTags: Cannot be called with stat type {}!", EFortQuestObjectiveStatEvent{ Type }.ToString());
            return;
        }

        UFortItemDefinition* TargetItem = Cast<UFortItemDefinition>(TargetObject);
        const uint8_t NoItemEvent = EFortQuestObjectiveItemEvent::GetMax_None();
        const uint8_t ItemEvent = Type == EFortQuestObjectiveStatEvent::GetCraft() ? static_cast<uint8_t>(EFortQuestObjectiveItemEvent::GetCraft())
            : Type == EFortQuestObjectiveStatEvent::GetCollect() ? static_cast<uint8_t>(EFortQuestObjectiveItemEvent::GetCollect()) : NoItemEvent;
        std::optional<FConditionContext> Conditions;
        static std::unordered_set<std::wstring> ReportedConditions;

        std::vector<FName> Processed;
        const TArray<UFortQuestItem*>& Quests = This->CurrentQuests;
        for (int32_t QuestIndex = 0; QuestIndex < Quests.Num(); ++QuestIndex)
        {
            UFortQuestItem* Quest = Quests[QuestIndex];
            const uint8_t State = Quest ? static_cast<uint8_t>(Quest->quest_state) : static_cast<uint8_t>(EFortQuestState::GetCompleted());
            UFortQuestItemDefinitionObjectives* Definition = State == static_cast<uint8_t>(EFortQuestState::GetCompleted()) || State == static_cast<uint8_t>(EFortQuestState::GetClaimed()) ? nullptr
                : Cast<UFortQuestItemDefinitionObjectives>(Quest->GetQuestDefinition());
            if (!Definition)
            {
                continue;
            }
            const std::wstring DefinitionName = Definition->GetName().ToStdString();
            const int32_t CurrentStage = Quest->CurrentStage;
            TStructArray<FFortMcpQuestObjectiveInfo>& Objectives = Definition->Objectives;
            for (int32_t ObjectiveIndex = 0; ObjectiveIndex < Objectives.Num(); ++ObjectiveIndex)
            {
                FFortMcpQuestObjectiveInfo& Objective = Objectives[ObjectiveIndex];
                const int32_t Stage = Objective.Stage;
                if (Stage != -1 && Stage != CurrentStage)
                {
                    continue;
                }
                const FName BackendName = Objective.BackendName;
                const bool bProcessed = std::find(Processed.begin(), Processed.end(), BackendName) != Processed.end();
                const FFortQuestObjectiveStatTableRow* Row = StatRowOf(Objective.ObjectiveStatHandle, DefinitionName.c_str());
                if (!bProcessed)
                {
                    bool bMatches = false;
                    if (bCustom)
                    {
                        FDataTableRowHandle& Handle = Objective.ObjectiveStatHandle;
                        bMatches = static_cast<UDataTable*>(Handle.DataTable) == static_cast<UDataTable*>(CustomStat->DataTable) && static_cast<FName>(Handle.RowName) == static_cast<FName>(CustomStat->RowName);
                    }
                    else if (static_cast<uint8_t>(Objective.ItemEvent) == NoItemEvent)
                    {
                        bMatches = ObjectiveMatches(Objective, Type, TargetTags, SourceTags, ContextTags, DefinitionName.c_str());
                    }
                    else if (ItemEvent != NoItemEvent && static_cast<uint8_t>(Objective.ItemEvent) == ItemEvent && TargetItem)
                    {
                        TAssetPtr<UFortItemDefinition>& Reference = Objective.ItemReference;
                        UFortItemDefinition* Item = Reference.Get();
                        if (!Item && Reference.ToStringReference().ToString().Len() > 0)
                        {
                            Item = FindObject<UFortItemDefinition>(nullptr, *Reference.ToStringReference().ToString());
                        }
                        bMatches = Item == TargetItem;
                    }

                    // The row's condition is evaluated against the event's target, the player and the zone.
                    const FString& Condition = Row ? static_cast<FString&>(Row->Condition) : FString();
                    if (bMatches && TargetObject && Row && Condition.Len() > 0)
                    {
                        if (!Conditions)
                        {
                            Conditions = MakeConditionContext(TargetObject, PlayerController);
                        }
                        const std::wstring ConditionText = Condition.ToStdString();
                        FConditionEvaluator Evaluator(ConditionText, *Conditions);
                        const std::optional<bool> Holds = Evaluator.Evaluate();
                        if (!Holds && ReportedConditions.insert(ConditionText).second)
                        {
                            Log::Warning(L"Failed to evaluate quest condition {}: {}!", ConditionText, Evaluator.GetError());
                        }
                        bMatches = Holds.value_or(false);
                    }
                    if (!bMatches)
                    {
                        continue;
                    }
                }

                // The objective's count: the profile's, the requests not yet answered, and the progress pending.
                const std::wstring StatName = BackendName.ToString().ToStdString();
                const int32_t Required = Objective.Count;
                const int32_t Saved = Quest->GetCompletionCount(BackendName) + CountInFlight(This, StatName);
                if (Saved >= Required)
                {
                    continue;
                }
                FFortQuestObjectiveCompletion* Entry = bProcessed ? FindPending(This, StatName) : nullptr;
                if (!bProcessed)
                {
                    Processed.push_back(BackendName);
                    Entry = &AddPending(This, StatName);
                    Entry->Count = Entry->Count + Count;
                }
                if (!Entry)
                {
                    continue;
                }
                int32_t NewCount = Entry->Count + Saved;
                if (NewCount >= Required)
                {
                    bFlush = true;
                    NewCount = Required;
                }
                ShowObjectiveCount(PlayerController, Definition, BackendName, NewCount);
            }
        }

        if (!Processed.empty())
        {
            Log::Info(L"SendStatEvent for {}: {} objective(s) listening to event {}{}", This->GetName().ToStdString(), Processed.size(),
                bCustom ? static_cast<FName>(CustomStat->RowName).ToString().ToStdString() : EFortQuestObjectiveStatEvent{ Type }.ToString(), bFlush ? L" (flushed)" : L"");
        }
        if (bFlush && !Processed.empty() && !Cast<AFortPlayerControllerAthena>(PlayerController))
        {
            SaveStatEvents(This);
        }
    }
}
#endif

UFortItemDefinition* UFortQuestItem::GetQuestDefinition() const
{
    return reinterpret_cast<const UFortItem*>(this)->GetItemDefinitionBP();
}

int32_t UFortQuestManager::AdvanceObjective(AFortPlayerController* PlayerController, UFortQuestItem* Quest, UFortQuestObjectiveInfo* Objective, int32_t Count)
{
#if WITH_SERVER_CODE
    // The flush is only looked for on servers at startup (Hook); Save the World's is the same function. Builds that strip
    // it save through Core's SaveStatEvents.
    if (!HasCurrentQuests() || !Quest || !Objective || Count <= 0 || (!Natives::UFortQuestManager_FlushStatEvents && !Finder::ResolveQuestStatEvents()))
    {
        return -1;
    }

    // SendStatEventWithTags adds the event's count to the pending entry of the objective's BackendName, or a new one
    // (1.9 0x8A69D4-0x8A6A7B); the entry's own list of the quests it moved stays empty here, as the flush reads only
    // the name and the count.
    const FString StatName = Objective->BackendName.ToString();
    const std::wstring StatNameText = StatName.ToStdString();
    TStructArray<FFortQuestObjectiveCompletion>& Pending = PendingChanges;
    FFortQuestObjectiveCompletion* Entry = nullptr;
    for (int32_t Index = 0; Index < Pending.Num() && !Entry; ++Index)
    {
        if (static_cast<FString&>(Pending[Index].StatName).ToStdString() == StatNameText)
        {
            Entry = &Pending[Index];
        }
    }
    if (!Entry)
    {
        Entry = &Pending.AddDefaulted();
        Entry->StatName = StatName;
        Entry->Count = 0;
    }
    Entry->Count = Entry->Count + Count;

    const int32_t Required = Objective->RequiredCount;
    const int32_t NewCount = std::min(Objective->AchievedCount + Count, std::max(Required, 0));
    Objective->AchievedCount = NewCount;
    SaveProgress(this);

    // The objective's new count for the client.
    ShowObjectiveCount(PlayerController, Quest->GetQuestDefinition(), Objective->BackendName, NewCount);
    return NewCount;
#else
    return -1;
#endif
}

void UFortQuestManager::GetSourceAndContextTags(FGameplayTagContainer& OutSourceTags, FGameplayTagContainer& OutContextTags)
{
    Reflection::FParams Params(StaticFunction_GetSourceAndContextTags(), true);
    if (Params.Call(this))
    {
        OutSourceTags = Params.Take<FGameplayTagContainer>(L"OutSourceTags");
        OutContextTags = Params.Take<FGameplayTagContainer>(L"OutContextTags");
    }
}

UFortQuestItem* UFortQuestManager::GetQuestWithDefinition(UFortQuestItemDefinition* Definition)
{
    Reflection::FParams Params(StaticFunction_GetQuestWithDefinition());
    Params.Set(L"Definition", Definition);
    return Params.Call(this) ? Params.Take<UFortQuestItem*>(L"ReturnValue") : nullptr;
}

bool UFortQuestManager::HasActiveQuest(UFortQuestItemDefinition* Definition)
{
    UFortQuestItem* Quest = Definition ? GetQuestWithDefinition(Definition) : nullptr;
    return Quest && Quest->quest_state == EFortQuestState::GetActive();
}

void UFortQuestManager::SendStatEvent(EFortQuestObjectiveStatEvent Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
{
    // Builds that strip the stat events (1.10) have the shared empty function as the typed wrapper.
    if (Natives::UFortQuestManager_bStatEventsStripped)
    {
        SendStatEventCall(this, static_cast<uint8_t>(Type), TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
        return;
    }
    if (Natives::UFortQuestManager_SendStatEvent)
    {
        Natives::UFortQuestManager_SendStatEvent(this, static_cast<uint8_t>(Type), TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
    }
}

// Collect and Craft events name the item definition, which item-event objectives (ItemReference) and stat rows with
// Item.* conditions are matched against.
void UFortQuestManager::SendItemStatEvent(EFortQuestObjectiveStatEvent Type, UFortItemDefinition* ItemDefinition, int32_t Count)
{
#if WITH_SERVER_CODE
    if (!ItemDefinition || Count <= 0)
    {
        return;
    }

    FGameplayTagContainer SourceTags;
    FGameplayTagContainer ContextTags;
    GetSourceAndContextTags(SourceTags, ContextTags);
    SendStatEvent(Type, ItemDefinition, ItemDefinition->GameplayTags, SourceTags, ContextTags, Count, false);
#endif
}

void UFortQuestManager::SendEndPrimaryMission(const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadges, int32_t NumMissionPoints, bool bCriticalMatchBonus)
{
    if (Natives::UFortQuestManager_SendEndPrimaryMission)
    {
        Natives::UFortQuestManager_SendEndPrimaryMission(this, MatchmakingSessionId, MissionName, MissionStatus, EarnedBadges, NumMissionPoints, bCriticalMatchBonus);
        return;
    }

#if WITH_SERVER_CODE
    // 1.10 strips it (Finder::ResolveZoneEndLive). As 1.9.1's does (0x8BA710), the quest manager's bound account profile
    // sends EndPrimaryMission with the objective progress not yet saved, through the dedicated server's command URL.
    UFortMcpProfileAccount* Account = Cast<UFortMcpProfileAccount>(static_cast<FWeakObjectPtr&>(BoundProfile).Get());
    FScriptArray& Pending = PendingChanges;
    if (!Account || !Account->EndPrimaryMission(Pending, MatchmakingSessionId, MissionName, MissionStatus, EarnedBadges, NumMissionPoints, bCriticalMatchBonus))
    {
        Log::Error(L"EndPrimaryMission: {} has no account profile to send it to", GetName().ToStdString());
    }
#endif
}

void UFortQuestManager::Hook()
{
#if WITH_SERVER_CODE
    // Battle Royale's challenge progress is saved here (SendStatEventWithTagsHK); Save the World's only needs the stat
    // events of builds that strip them.
    const bool bBattleRoyale = Configuration::Get().IsBattleRoyale();
    if (!Finder::ResolveQuestStatEvents())
    {
        return;
    }
    if (!Natives::UFortQuestManager_bStatEventsStripped)
    {
        if (bBattleRoyale)
        {
            Hooks::Detour(L"UFortQuestManager::SendStatEventWithTags", reinterpret_cast<void*>(Natives::UFortQuestManager_SendStatEventWithTags), &SendStatEventWithTagsHK, &SendStatEventWithTagsOG);
        }
        return;
    }

    // 1.10 strips SendStatEventWithTags and SaveStatEvents: the senders' calls in its place come to Core's, and
    // SendCustomStatEvent, which stops after validating the stat, is Core's.
    SendStatEventWithTagsOG = &SendStatEventWithTagsCore;
    for (uint8_t* Call : Finder::StrippedQuestStatEventCalls())
    {
        Hooks::RedirectCall(L"UFortQuestManager::SendStatEventWithTags (stripped)", Call, reinterpret_cast<void*>(&SendStatEventCall));
    }
    Hooks::Detour(L"UFortQuestManager::SendCustomStatEvent (stripped)", reinterpret_cast<void*>(Natives::UFortQuestManager_SendCustomStatEvent), &SendCustomStatEventHK, nullptr);
#endif
}

void UFortQuestManager::SendStatEventCall(UFortQuestManager* This, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
{
#if WITH_SERVER_CODE
    // The typed wrapper (1.9.1 0x8CBE10): no custom stat.
    const FDataTableRowHandle* NoCustomStat = nullptr;
    SendStatEventWithTagsHK(This, NoCustomStat, Type, TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
#endif
}

void UFortQuestManager::SendCustomStatEventHK(UFortQuestManager* This, const void* ObjectiveStat, int32_t Count, bool bForceFlush)
{
#if WITH_SERVER_CODE
    // 1.9.1 0x8CA3E0: the stat must be a Custom row; it goes to SendStatEventWithTags without a target or tags.
    const FDataTableRowHandle* Handle = static_cast<const FDataTableRowHandle*>(ObjectiveStat);
    const FFortQuestObjectiveStatTableRow* Row = Handle && This ? StatRowOf(*Handle, This->GetName().ToStdString().c_str()) : nullptr;
    if (!Row || static_cast<uint8_t>(Row->Type) != EFortQuestObjectiveStatEvent::GetCustom())
    {
        Log::Warning(L"SendCustomStatEvent: Called with invalid or non custom stat {}!", Handle ? static_cast<FName>(Handle->RowName).ToString().ToStdString() : std::wstring(L"None"));
        return;
    }
    const FGameplayTagContainer NoTags;
    SendStatEventWithTagsHK(This, Handle, EFortQuestObjectiveStatEvent::GetCustom(), nullptr, NoTags, NoTags, NoTags, Count, bForceFlush);
#endif
}

void UFortQuestManager::SendStatEventWithTagsHK(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
{
#if WITH_SERVER_CODE
    // Battle Royale's challenges (1.8), Core-Backend #92: a chest search (ServerAttemptInteract, 0x845020: an Interact
    // event with a flush) never reached the athena profile, for two reasons.
    // First, the event's context is the game mode's GameContextTags, which nothing fills in Battle Royale, and the
    // objectives ask for the playlist there (AFortGameModeAthena::AddPlaylistContextTag): the game mode gets it, and so
    // does the event, whose tags were copied from it before.
    UFortRegisteredPlayerInfo* PlayerInfo = Cast<UFortRegisteredPlayerInfo>(This ? This->GetOuter() : nullptr);
    AFortGameModeAthena* GameMode = PlayerInfo && PlayerInfo->GetAthenaQuestManager() == This ? AFortGameModeAthena::GetCurrent(GWorld) : nullptr;
    if (!GameMode)
    {
        SendStatEventWithTagsOG(This, CustomStatName, Type, TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
        return;
    }
    GameMode->AddPlaylistContextTag(static_cast<FGameplayTagContainer&>(GameMode->GameContextTags));
    FGameplayTagContainer EventContextTags = ContextTags;
    GameMode->AddPlaylistContextTag(EventContextTags);
    SendStatEventWithTagsOG(This, CustomStatName, Type, TargetObject, TargetTags, SourceTags, EventContextTags, Count, bFlush);

    // Second, SendStatEventWithTags (0x890730) adds what the event advanced to the quest manager's pending progress
    // (+0x128) and, when the event asked for a flush or completed an objective, saves it, but not for an
    // AFortPlayerControllerAthena (0x891500-0x891565). Nothing else saves it: its flush (0x88E060) has no other caller,
    // and UFortMcpProfileAthena::EndBattleRoyaleGame, which takes the same progress as Advance, has no caller left (its
    // FName, 0x662E1A0, is only made). Epic's servers took Battle Royale's challenges from its stats instead. The
    // athena quest manager's progress is saved here, at once when the event asked for it, as Save the World's is, and
    // within a few seconds otherwise, so bursts (every other player's death is an AthenaOutlive event, 0x599DA0,
    // unflushed) make one UpdateQuests.
    if (bFlush)
    {
        SaveProgress(This);
        return;
    }
    const FWeakObjectPtr Weak(This);
    if (std::none_of(DueSaves.begin(), DueSaves.end(), [&Weak](const auto& Due) { return Due.first == Weak; }))
    {
        const bool bIdle = DueSaves.empty();
        DueSaves.emplace_back(Weak, std::chrono::steady_clock::now() + SaveDelay);
        if (bIdle)
        {
            SaveWhenDue();
        }
    }
#else
    SendStatEventWithTagsOG(This, CustomStatName, Type, TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
#endif
}
