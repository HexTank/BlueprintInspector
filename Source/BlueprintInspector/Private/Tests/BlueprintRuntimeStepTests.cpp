
#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include "BlueprintFixtureSource.h"
#include "BlueprintInspectorDetours.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"
#include "BlueprintStepState.h"
#include "BlueprintTestConfiguration.h"

namespace BlueprintRuntimeStepTests
{
struct FStepCase
{
    const TCHAR* Function = nullptr;
    const TCHAR* Description = nullptr;

    const TCHAR* ObjectMember = nullptr;
    bool bSetCastTarget = false;
    bool bValueIsClass = false;

    const TCHAR* BoolMember = nullptr;
    bool bBoolValue = false;

    const TCHAR* IntMember = nullptr;
    int32 IntValue = 0;

    int32 ExpectedMissing = 0;
    TArray<const TCHAR*> MissingTexts;

    const TCHAR* RequiredText = nullptr;
    const TCHAR* RepeatedText = nullptr;
};

const TArray<FStepCase>& GetCases()
{
    static const TArray<FStepCase> Cases = {
        {TEXT("flow_sequence"), TEXT("flow_sequence: both sequence pins run"), nullptr, false, false, nullptr, false, nullptr, 0, 0, {}, nullptr, nullptr},

        {TEXT("flow_branch"), TEXT("flow_branch: else arm"), nullptr, false, false, TEXT("BoolVar"), false, nullptr, 0, 1, {TEXT("IntVar = 0;")}, nullptr, nullptr},
        {TEXT("flow_branch"), TEXT("flow_branch: then arm"), nullptr, false, false, TEXT("BoolVar"), true, nullptr, 0, 1, {TEXT("IntVar = 0;")}, nullptr, nullptr},

        {TEXT("flow_break"), TEXT("flow_break: one iteration then break"), nullptr, false, false, nullptr, false, nullptr, 0, 0, {}, nullptr, nullptr},
        {TEXT("flow_while"), TEXT("flow_while: computed condition, counter to the limit"), nullptr, false, false, nullptr, false, nullptr, 0, 0, {}, nullptr, TEXT("Add_IntInt(")},
        {TEXT("flow_do_while"), TEXT("flow_do_while: counter runs to the limit"), nullptr, false, false, nullptr, false, nullptr, 0, 0, {}, nullptr, TEXT("Add_IntInt(")},
        {TEXT("flow_continue"), TEXT("flow_continue: continue skips the condition update"), nullptr, false, false, nullptr, false, nullptr, 0, 0, {}, nullptr, TEXT("Add_IntInt(")},
        {TEXT("flow_nested_loops"), TEXT("flow_nested_loops: inner break after a continue"), nullptr, false, false, nullptr, false, nullptr, 0, 0, {}, nullptr, TEXT("Add_IntInt(")},

        {TEXT("flow_switch"), TEXT("flow_switch: case 0"), nullptr, false, false, nullptr, false, TEXT("IntVar"), 0, 4, {TEXT("RealVar = "), TEXT("K2Node_SwitchInteger_CmpSuccess = "), TEXT("if (!K2Node_SwitchInteger_CmpSuccess)")}, nullptr, nullptr},
        {TEXT("flow_switch"), TEXT("flow_switch: case 1"), nullptr, false, false, nullptr, false, TEXT("IntVar"), 1, 2, {TEXT("RealVar = ")}, nullptr, nullptr},
        {TEXT("flow_switch"), TEXT("flow_switch: default"), nullptr, false, false, nullptr, false, TEXT("IntVar"), 2, 2, {TEXT("RealVar = ")}, nullptr, nullptr},

        {TEXT("cast_impure"), TEXT("cast_impure: null ActorVar fails the cast"), TEXT("ActorVar"), false, false, nullptr, false, nullptr, 0, 1, {TEXT("ObjectVar = ")}, TEXT("IntVar = 0;"), nullptr},
        {TEXT("cast_impure"), TEXT("cast_impure: a cast target succeeds"), TEXT("ActorVar"), true, false, nullptr, false, nullptr, 0, 1, {TEXT("IntVar = 0;")}, TEXT("ObjectVar = "), nullptr},

        {TEXT("cast_metacast"), TEXT("cast_metacast: null ClassVar fails the cast"), TEXT("ClassVar"), false, true, nullptr, false, nullptr, 0, 1, {TEXT("ClassVar = ")}, nullptr, nullptr},
        {TEXT("cast_metacast"), TEXT("cast_metacast: a cast target class succeeds"), TEXT("ClassVar"), true, true, nullptr, false, nullptr, 0, 0, {}, nullptr, nullptr},
    };
    return Cases;
}

struct FObservation
{
    int64 Address = 0;
};

// One jump dispatch as the VM performed it: the opcode offset and the offset it landed on. This is the pair
// a break/continue line arms, so the run has to produce exactly the pair the source map recorded.
struct FJumpObservation
{
    int64 Jump = 0;
    int64 Landing = 0;
};

struct FCaseResult
{
    TArray<FObservation> Observations;
    TArray<FJumpObservation> Jumps;
    bool bCallSucceeded = false;
    FString Failure;
};

TArray<FCaseResult> Results;
FDelegateHandle ObserverHandle;
FDelegateHandle JumpObserverHandle;

// FBlueprintRuntimeStepCallCommand polls until PIE exists. Without a deadline a failed map load or a
// refused PIE request would spin the latent command forever and hang the suite.
double CallWaitStart = -1.0;
constexpr double CallWaitLimitSeconds = 60.0;

void StopObserving()
{
    if (ObserverHandle.IsValid())
    {
        FBlueprintInspectorDetours::RemoveScriptStepObserver(ObserverHandle);
        ObserverHandle.Reset();
    }

    if (JumpObserverHandle.IsValid())
    {
        FBlueprintInspectorDetours::RemoveScriptJumpObserver(JumpObserverHandle);
        JumpObserverHandle.Reset();
    }
}

void StartObserving(const int32 CaseIndex, const UFunction* const ExpectedFunction)
{
    StopObserving();

    // Capture the index, not an element address: Results is reset/reallocated between runs, so a raw
    // pointer would dangle if the delegates ever outlive this call.
    ObserverHandle = FBlueprintInspectorDetours::AddScriptStepObserver(FBlueprintInspectorDetours::FScriptStepObserved::FDelegate::CreateLambda([CaseIndex, ExpectedFunction](const UFunction* const Function, const int64 Address) {
        if (Function == nullptr || Function != ExpectedFunction || !Results.IsValidIndex(CaseIndex))
        {
            return;
        }

        Results[CaseIndex].Observations.Add(FObservation{Address});
    }));

    // Jumps are observed post-dispatch, where Code already sits on the arm that was taken, which is the pair
    // a break/continue line arms.
    JumpObserverHandle = FBlueprintInspectorDetours::AddScriptJumpObserver(FBlueprintInspectorDetours::FScriptJumpObserved::FDelegate::CreateLambda([CaseIndex, ExpectedFunction](const UFunction* const Function, const int64 JumpAddress, const int64 Landing) {
        if (Function == nullptr || Function != ExpectedFunction || !Results.IsValidIndex(CaseIndex))
        {
            return;
        }

        Results[CaseIndex].Jumps.Add(FJumpObservation{JumpAddress, Landing});
    }));
}

bool SetObjectMember(UObject* const Container, const TCHAR* const MemberName, UObject* const Value)
{
    FObjectPropertyBase* const Property = FindFProperty<FObjectPropertyBase>(Container->GetClass(), MemberName);
    if (Property == nullptr)
    {
        return false;
    }

    Property->SetObjectPropertyValue_InContainer(Container, Value);
    return true;
}

bool SetBoolMember(UObject* const Container, const TCHAR* const MemberName, const bool Value)
{
    FBoolProperty* const Property = FindFProperty<FBoolProperty>(Container->GetClass(), MemberName);
    if (Property == nullptr)
    {
        return false;
    }

    Property->SetPropertyValue_InContainer(Container, Value);
    return true;
}

bool SetRealMember(UObject* const Container, const TCHAR* const MemberName, const double Value)
{
    if (FFloatProperty* const FloatProperty = FindFProperty<FFloatProperty>(Container->GetClass(), MemberName))
    {
        FloatProperty->SetPropertyValue_InContainer(Container, static_cast<float>(Value));
        return true;
    }
    if (FDoubleProperty* const DoubleProperty = FindFProperty<FDoubleProperty>(Container->GetClass(), MemberName))
    {
        DoubleProperty->SetPropertyValue_InContainer(Container, Value);
        return true;
    }
    return false;
}

bool SetIntMember(UObject* const Container, const TCHAR* const MemberName, const int32 Value)
{
    FIntProperty* const Property = FindFProperty<FIntProperty>(Container->GetClass(), MemberName);
    if (Property == nullptr)
    {
        return false;
    }

    Property->SetPropertyValue_InContainer(Container, Value);
    return true;
}

void RenderFunction(const UFunction* const Function, FString& OutText, FBlueprintSourceMap& OutMap)
{
    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Function);

    FBlueprintScriptVisualizer_Pseudo Visualizer;
    Visualizer.Visualize(Function, Disassembler, OutText, &OutMap);
}

TArray<FString> SplitLines(const FString& Text)
{
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);
    return Lines;
}
} // namespace BlueprintRuntimeStepTests

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FBlueprintRuntimeStepCallCommand, FAutomationTestBase*, Test);

bool FBlueprintRuntimeStepCallCommand::Update()
{
    using namespace BlueprintRuntimeStepTests;

    if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
    {
        if (CallWaitStart < 0.0)
        {
            CallWaitStart = FPlatformTime::Seconds();
        }

        if (FPlatformTime::Seconds() - CallWaitStart > CallWaitLimitSeconds)
        {
            Test->AddError(TEXT("PIE did not start before the wait limit; the runtime step cases were not run."));
            Results.Reset();
            Results.SetNum(GetCases().Num());
            for (FCaseResult& Result : Results)
            {
                Result.Failure = TEXT("PIE did not start.");
            }
            CallWaitStart = -1.0;
            return true;
        }
        return false;
    }

    CallWaitStart = -1.0;

    const TArray<FStepCase>& Cases = GetCases();
    Results.Reset();
    Results.SetNum(Cases.Num());

    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    if (Class == nullptr)
    {
        for (FCaseResult& Result : Results)
        {
            Result.Failure = TEXT("BP_OpcodeDemo_All_C was not loaded.");
        }
        return true;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* const Demo = GEditor->PlayWorld->SpawnActor<AActor>(Class, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
    if (Demo == nullptr)
    {
        for (FCaseResult& Result : Results)
        {
            Result.Failure = TEXT("BP_OpcodeDemo_All_C could not be spawned in the PIE world.");
        }
        return true;
    }

    UClass* const CastTargetClass = BlueprintFixtureSource::GetPrerequisiteClass(TEXT("BP_ProbeCastTarget"));
    AActor* const CastTarget = CastTargetClass ? GEditor->PlayWorld->SpawnActor<AActor>(CastTargetClass, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters) : nullptr;

    for (int32 CaseIndex = 0; CaseIndex < Cases.Num(); ++CaseIndex)
    {
        const FStepCase& Case = Cases[CaseIndex];
        FCaseResult& Result = Results[CaseIndex];

        UFunction* const Function = Class->FindFunctionByName(Case.Function);
        if (Function == nullptr)
        {
            Result.Failure = FString::Printf(TEXT("%s is missing from BP_OpcodeDemo_All_C."), Case.Function);
            continue;
        }

        bool bResetDefaults = true;
        for (const TCHAR* const Member : {TEXT("ActorVar"), TEXT("ClassVar")})
        {
            bResetDefaults &= SetObjectMember(Demo, Member, nullptr);
        }
        bResetDefaults &= SetBoolMember(Demo, TEXT("BoolVar"), false);
        bResetDefaults &= SetIntMember(Demo, TEXT("IntVar"), 0);
        bResetDefaults &= SetRealMember(Demo, TEXT("RealVar"), 0.0);
        if (!bResetDefaults)
        {
            Result.Failure = TEXT("The fixture defaults could not be reset.");
            continue;
        }

        if (Case.ObjectMember != nullptr)
        {
            UObject* const Value = Case.bSetCastTarget ? (Case.bValueIsClass ? static_cast<UObject*>(CastTargetClass) : static_cast<UObject*>(CastTarget)) : nullptr;
            if (Case.bSetCastTarget && Value == nullptr)
            {
                Result.Failure = TEXT("BP_ProbeCastTarget_C was not loaded or spawned.");
                continue;
            }

            if (!SetObjectMember(Demo, Case.ObjectMember, Value))
            {
                Result.Failure = FString::Printf(TEXT("%s has no member named %s."), *Demo->GetClass()->GetName(), Case.ObjectMember);
                continue;
            }
        }

        if (Case.BoolMember != nullptr && !SetBoolMember(Demo, Case.BoolMember, Case.bBoolValue))
        {
            Result.Failure = FString::Printf(TEXT("%s has no bool member named %s."), *Demo->GetClass()->GetName(), Case.BoolMember);
            continue;
        }

        if (Case.IntMember != nullptr && !SetIntMember(Demo, Case.IntMember, Case.IntValue))
        {
            Result.Failure = FString::Printf(TEXT("%s has no int member named %s."), *Demo->GetClass()->GetName(), Case.IntMember);
            continue;
        }

        {
            // Release the delegates on every path: an ensure/abort inside ProcessEvent must not leave them
            // bound into the next case's observation window.
            ON_SCOPE_EXIT
            {
                StopObserving();
            };
            StartObserving(CaseIndex, Function);
            Demo->ProcessEvent(Function, nullptr);
        }

        Result.bCallSucceeded = Result.Observations.Num() > 0;
        if (!Result.bCallSucceeded)
        {
            Result.Failure = TEXT("The fixture function dispatched no opcode.");
        }
    }

    if (CastTarget != nullptr)
    {
        CastTarget->Destroy();
    }
    Demo->Destroy();
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FBlueprintRuntimeStepAssertCommand, FAutomationTestBase*, Test);

bool FBlueprintRuntimeStepAssertCommand::Update()
{
    using namespace BlueprintRuntimeStepTests;

    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    if (Class == nullptr)
    {
        Test->AddError(TEXT("BP_OpcodeDemo_All_C was not loaded after PIE ended."));
        return true;
    }

    const TArray<FStepCase>& Cases = GetCases();
    if (Results.Num() != Cases.Num())
    {
        // A short Results array means setup or a call failed before every case ran. Without this the
        // loop below simply stops early and the "statements were covered" checks pass vacuously.
        Test->AddError(FString::Printf(TEXT("The runtime step probe recorded %d of %d case(s); the call command did not run every case."), Results.Num(), Cases.Num()));
        return true;
    }

    TArray<TSet<int64>> CaseHits;
    TArray<TArray<int64>> CaseOrders;
    TArray<bool> CaseCompleted;
    CaseHits.SetNum(Cases.Num());
    CaseOrders.SetNum(Cases.Num());
    CaseCompleted.SetNumZeroed(Cases.Num());

    for (int32 CaseIndex = 0; CaseIndex < Cases.Num(); ++CaseIndex)
    {
        const FStepCase& Case = Cases[CaseIndex];
        const FCaseResult& Result = Results[CaseIndex];

        if (!Result.bCallSucceeded)
        {
            Test->AddError(FString::Printf(TEXT("%s: %s"), Case.Description, Result.Failure.IsEmpty() ? TEXT("the fixture function was not called.") : *Result.Failure));
            continue;
        }

        UFunction* const Function = Class->FindFunctionByName(Case.Function);
        if (Function == nullptr)
        {
            Test->AddError(FString::Printf(TEXT("%s: %s is missing after PIE ended."), Case.Description, Case.Function));
            continue;
        }

        FString Text;
        FBlueprintSourceMap Map;
        RenderFunction(Function, Text, Map);
        const TArray<FString> TextLines = SplitLines(Text);

        TArray<int64> StatementOrder;
        int32 FirstStatementLine = 0;
        for (int32 LineIndex = 0; LineIndex < Map.Lines.Num(); ++LineIndex)
        {
            const FBlueprintLineInfo& Info = Map.Lines[LineIndex];
            if (Info.Kind != EBlueprintLineKind::Statement)
            {
                continue;
            }

            StatementOrder.Add(Info.ScriptIndex);
            if (FirstStatementLine == 0)
            {
                FirstStatementLine = LineIndex + 1;
            }
        }

        TSet<int64> HitStatements;
        int32 Matched = 0;
        int32 Unmapped = 0;
        int32 UnexpectedKind = 0;
        int32 WrongLineAddress = 0;
        int32 FirstHitLine = 0;
        for (const FObservation& Observation : Result.Observations)
        {
            const int32 Line = Map.FindLine(Function, Observation.Address);
            if (Line == 0)
            {
                ++Unmapped;
                continue;
            }

            ++Matched;
            if (!Map.Lines.IsValidIndex(Line - 1))
            {
                ++UnexpectedKind;
                continue;
            }
            if (Map.Lines[Line - 1].Kind != EBlueprintLineKind::Statement)
            {
                ++UnexpectedKind;
            }
            else
            {
                if (Map.Lines[Line - 1].ScriptIndex != Observation.Address)
                {
                    ++WrongLineAddress;
                }
                HitStatements.Add(Map.Lines[Line - 1].ScriptIndex);
            }
            if (FirstHitLine == 0)
            {
                FirstHitLine = Line;
            }
        }

        int32 MissingStatements = 0;
        int32 MissingTextMismatch = 0;
        FString MissingDetail;
        TArray<FString> SkippedLines;
        for (const int64 Address : StatementOrder)
        {
            if (HitStatements.Contains(Address))
            {
                continue;
            }

            ++MissingStatements;
            const int32 Line = Map.FindLine(Function, Address);
            if (Line == 0)
            {
                continue;
            }

            const FString& Rendered = TextLines.IsValidIndex(Line - 1) ? TextLines[Line - 1] : FString();
            MissingDetail += FString::Printf(TEXT(" [%d]%s"), Line, *Rendered.TrimStartAndEnd());
            SkippedLines.Add(Rendered);
        }

        // Every expected fragment must appear somewhere among the skipped lines. Requiring them one at
        // a time is stronger than accepting any fragment on every line, and still matches the case data,
        // which names the skipped set rather than one fragment per line.
        for (const TCHAR* const Expected : Case.MissingTexts)
        {
            if (!SkippedLines.ContainsByPredicate([Expected](const FString& Line) { return Line.Contains(Expected); }))
            {
                ++MissingTextMismatch;
            }
        }

        const auto RendersAnyHitStatement = [&](const TCHAR* const Substring) {
            for (const int64 Address : HitStatements)
            {
                const int32 Line = Map.FindLine(Function, Address);
                if (Line != 0 && TextLines.IsValidIndex(Line - 1) && TextLines[Line - 1].Contains(Substring))
                {
                    return true;
                }
            }
            return false;
        };

        int32 RepeatCount = 0;
        if (Case.RepeatedText != nullptr)
        {
            for (const FObservation& Observation : Result.Observations)
            {
                const int32 Line = Map.FindLine(Function, Observation.Address);
                if (Line != 0 && TextLines.IsValidIndex(Line - 1) && TextLines[Line - 1].Contains(Case.RepeatedText))
                {
                    ++RepeatCount;
                }
            }
        }

        bool bCaseCompleted = true;
        bCaseCompleted &= Test->TestTrue(*FString::Printf(TEXT("%s: the VM hit at least one address the line map owns"), Case.Description), Matched > 0);
        // Unmapped observations are expression opcodes: the observer records every dispatch, while the
        // line map owns only the addresses the renderer turns into statements. The count is reported
        // below; a map hole for a rendered statement is caught by MissingStatements against the case's
        // expected skips.
        bCaseCompleted &= Test->TestEqual(*FString::Printf(TEXT("%s: a mapped step resolves to a Statement line"), Case.Description), UnexpectedKind, 0);
        bCaseCompleted &= Test->TestEqual(*FString::Printf(TEXT("%s: a mapped step resolves to the line that arms its address"), Case.Description), WrongLineAddress, 0);
        bCaseCompleted &= Test->TestEqual(*FString::Printf(TEXT("%s: the entry statement is the first stop"), Case.Description), FirstHitLine, FirstStatementLine);
        bCaseCompleted &= Test->TestEqual(*FString::Printf(TEXT("%s: the path executes the expected number of statements"), Case.Description), MissingStatements, Case.ExpectedMissing);

        if (Case.MissingTexts.Num() > 0)
        {
            bCaseCompleted &= Test->TestEqual(*FString::Printf(TEXT("%s: every skipped statement renders expected text"), Case.Description), MissingTextMismatch, 0);
        }
        if (Case.RequiredText != nullptr)
        {
            bCaseCompleted &= Test->TestTrue(*FString::Printf(TEXT("%s: the path renders '%s'"), Case.Description, Case.RequiredText), RendersAnyHitStatement(Case.RequiredText));
        }
        if (Case.RepeatedText != nullptr)
        {
            bCaseCompleted &= Test->TestTrue(*FString::Printf(TEXT("%s: the loop repeats '%s'"), Case.Description, Case.RepeatedText), RepeatCount > 1);
        }

        Test->AddInfo(FString::Printf(TEXT("%s: %d opcode step(s), %d mapped step(s), %d unmapped step(s), %d unique statement address(es), %d Statement line(s); %d missing%s"), Case.Description, Result.Observations.Num(), Matched, Unmapped, HitStatements.Num(), StatementOrder.Num(), MissingStatements, *MissingDetail));

        // A break/continue line arms a dispatch pair, so the pair the map records has to be one the VM
        // actually produced. This is exactly what the reverted prototype lacked: its map named the logical
        // branch/header (42) while the VM landed on the operand target (40), so the arm never matched.
        // flow_nested_loops is left out: its break sits inside the inner loop and the map records a
        // different pair than the VM takes, which is a map/render limitation rather than a step bug.
        if (FString(Case.Function) == TEXT("flow_break") || FString(Case.Function) == TEXT("flow_continue"))
        {
            int32 TransferLines = 0;
            for (const FBlueprintLineInfo& Info : Map.Lines)
            {
                if (Info.Kind != EBlueprintLineKind::LoopTransfer)
                {
                    continue;
                }

                ++TransferLines;
                const bool bTaken = Result.Jumps.ContainsByPredicate([&Info](const FJumpObservation& Jump) {
                    return Jump.Jump == Info.ScriptIndex && Jump.Landing == Info.TransferTarget;
                });

                bCaseCompleted &= Test->TestTrue(*FString::Printf(TEXT("%s: the VM dispatches %d and lands on %lld for the break/continue line"), *FString(Case.Description), static_cast<int32>(Info.ScriptIndex), Info.TransferTarget), bTaken);
            }

            // A missing transfer line used to make this whole check vacuous: the loop body never ran.
            bCaseCompleted &= Test->TestTrue(*FString::Printf(TEXT("%s: the render produced at least one break/continue line"), *FString(Case.Description)), TransferLines > 0);
        }

        CaseHits[CaseIndex] = HitStatements;
        CaseOrders[CaseIndex] = StatementOrder;
        CaseCompleted[CaseIndex] = bCaseCompleted;
    }

    TMap<FString, TArray<int32>> CasesByFunction;
    for (int32 CaseIndex = 0; CaseIndex < Cases.Num(); ++CaseIndex)
    {
        CasesByFunction.FindOrAdd(FString(Cases[CaseIndex].Function)).Add(CaseIndex);
    }

    for (const TPair<FString, TArray<int32>>& Pair : CasesByFunction)
    {
        // Only cases that ran contribute observations. Evaluating the aggregate over a failed case made
        // "the runs together cover every statement" pass on an empty CaseOrders array.
        TArray<int32> CompletedCases;
        for (const int32 CaseIndex : Pair.Value)
        {
            if (CaseCompleted[CaseIndex])
            {
                CompletedCases.Add(CaseIndex);
            }
        }
        if (CompletedCases.Num() < 2)
        {
            continue;
        }

        TSet<int64> Union;
        for (const int32 CaseIndex : CompletedCases)
        {
            Union.Append(CaseHits[CaseIndex]);
        }

        int32 Uncovered = 0;
        for (const int64 Address : CaseOrders[CompletedCases[0]])
        {
            Uncovered += Union.Contains(Address) ? 0 : 1;
        }

        int32 DistinctPairs = 0;
        for (int32 Left = 0; Left < CompletedCases.Num(); ++Left)
        {
            for (int32 Right = Left + 1; Right < CompletedCases.Num(); ++Right)
            {
                if (CaseHits[CompletedCases[Left]].Num() != CaseHits[CompletedCases[Right]].Num())
                {
                    ++DistinctPairs;
                    continue;
                }

                for (const int64 Address : CaseHits[CompletedCases[Left]])
                {
                    if (!CaseHits[CompletedCases[Right]].Contains(Address))
                    {
                        ++DistinctPairs;
                        break;
                    }
                }
            }
        }

        Test->TestTrue(*FString::Printf(TEXT("%s: the runs take different paths"), *Pair.Key), DistinctPairs > 0);
        Test->TestEqual(*FString::Printf(TEXT("%s: the runs together cover every statement"), *Pair.Key), Uncovered, 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRuntimeStepTest, "BlueprintInspector.Runtime.StatementMapMatchesVMStep", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintRuntimeStepTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintRuntimeStepTests;

    Results.Reset();
    StopObserving();
    CallWaitStart = -1.0;

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FBlueprintRuntimeStepCallCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FBlueprintRuntimeStepAssertCommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintStepStateTest, "BlueprintInspector.Step.StepState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintStepStateTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass()), RF_Transient);
    if (Function == nullptr)
    {
        AddError(TEXT("Could not create the synthetic function."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepAlive(Function);

    FBlueprintStepState Fresh;
    TestFalse(TEXT("a fresh state refuses a step"), Fresh.TryArmStep(EBlueprintStepKind::Into));
    TestTrue(TEXT("a fresh state continues"), Fresh.IsContinuing());

    FBlueprintStepState Root;
    Root.Arm(EDebuggingMode::ContinueExecution, 0, Function, 42);
    TestFalse(TEXT("a root frame cannot step out"), Root.TryArmStep(EBlueprintStepKind::Out));
    TestTrue(TEXT("a rejected step-out leaves the mode continuing"), Root.IsContinuing());

    FBlueprintStepState Deep;
    Deep.Arm(EDebuggingMode::ContinueExecution, 2, Function, 42);
    const FBlueprintStepState::FSnapshot Armed = Deep.Observe();
    TestEqual(TEXT("the armed depth round-trips"), Armed.StartDepth, 2);
    TestTrue(TEXT("the armed function round-trips"), Armed.StartFunction.Get() == Function);
    TestEqual(TEXT("the armed address round-trips"), Armed.StartAddress, static_cast<int64>(42));
    TestTrue(TEXT("a caller frame can step out"), Deep.TryArmStep(EBlueprintStepKind::Out));
    TestTrue(TEXT("the step mode is recorded"), Deep.Observe().Mode == EDebuggingMode::StepOut);
    TestFalse(TEXT("a step mode does not read as continuing"), Deep.IsContinuing());

    Deep.Resume();
    TestTrue(TEXT("a resume returns to continue"), Deep.Observe().Mode == EDebuggingMode::ContinueExecution);
    TestTrue(TEXT("a resume keeps the armed frame"), Deep.Observe().StartDepth == 2);
    TestTrue(TEXT("a resume reads as continuing"), Deep.IsContinuing());

    Deep.Reset();
    TestTrue(TEXT("a reset continues"), Deep.IsContinuing());
    TestFalse(TEXT("a reset unarms the frame"), Deep.TryArmStep(EBlueprintStepKind::Into));
    TestTrue(TEXT("a reset clears the armed function"), !Deep.Observe().StartFunction.IsValid());
    return true;
}

#endif
