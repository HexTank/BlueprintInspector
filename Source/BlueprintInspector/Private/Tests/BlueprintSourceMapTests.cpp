// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include "BlueprintInspectorPort.h"
#include "BlueprintSourceMapBuilder.h"

namespace BlueprintSourceMapTests
{
FBlueprintLineInfo MakeStatement(const int64 Address)
{
    FBlueprintLineInfo Info;
    Info.Kind = EBlueprintLineKind::Statement;
    Info.ScriptIndex = Address;
    return Info;
}
} // namespace BlueprintSourceMapTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapBuilderTest, "BlueprintInspector.SourceMap.BuilderAssignsLinesAndKinds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapBuilderTest::RunTest(const FString& Parameters)
{
    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, BlueprintSourceMapTests::MakeStatement(10));
    Builder.AddLocation(14, BlueprintSourceMapTests::MakeStatement(20));

    const FBlueprintSourceMap Map = Builder.Build(TEXT("stmt;\n  more;\nnext;\n"));

    if (!TestEqual(TEXT("one entry per line, with no extra entry for the trailing newline"), Map.Lines.Num(), 3))
    {
        return false;
    }
    TestTrue(TEXT("line 1 is a statement"), Map.Lines[0].Kind == EBlueprintLineKind::Statement);
    TestEqual(TEXT("line 1 carries the first address"), Map.Lines[0].ScriptIndex, static_cast<int64>(10));
    TestTrue(TEXT("line 2 is the first statement's continuation"), Map.Lines[1].Kind == EBlueprintLineKind::SubStatement);
    TestEqual(TEXT("line 2 shares the first address"), Map.Lines[1].ScriptIndex, static_cast<int64>(10));
    TestTrue(TEXT("line 3 is the second statement"), Map.Lines[2].Kind == EBlueprintLineKind::Statement);
    TestEqual(TEXT("line 3 carries the second address"), Map.Lines[2].ScriptIndex, static_cast<int64>(20));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapTrimTest, "BlueprintInspector.SourceMap.BuilderTrimsRemovedText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapTrimTest::RunTest(const FString& Parameters)
{
    // A renderer that prunes text has to drop the record it wrote for it: "gone;\n" was recorded at offset
    // 6 and then removed, and the closing line records its own structural location at that same offset.
    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, BlueprintSourceMapTests::MakeStatement(10));
    Builder.AddLocation(6, BlueprintSourceMapTests::MakeStatement(20));
    Builder.TrimFrom(6);
    Builder.AddLocation(6, FBlueprintLineInfo{});

    TestEqual(TEXT("the trimmed record is gone"), Builder.Num(), 2);

    const FBlueprintSourceMap Map = Builder.Build(TEXT("kept;\n}\n"));
    if (!TestEqual(TEXT("the map has two lines"), Map.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("line 1 keeps its statement"), Map.Lines[0].Kind == EBlueprintLineKind::Statement);
    TestEqual(TEXT("line 1 keeps its address"), Map.Lines[0].ScriptIndex, static_cast<int64>(10));
    TestTrue(TEXT("the trimmed line is structural"), Map.Lines[1].Kind == EBlueprintLineKind::None);
    TestFalse(TEXT("the trimmed line is not breakpointable"), Map.Lines[1].IsBreakpointable());
    for (const FBlueprintLineInfo& Info : Map.Lines)
    {
        TestNotEqual(TEXT("no line reports the removed address"), Info.ScriptIndex, static_cast<int64>(20));
    }

    TestEqual(TEXT("Build consumes the records, so the next render starts empty"), Builder.Num(), 0);

    // Trimming a consumed builder is still legal, and it accepts the next render's records.
    Builder.AddLocation(0, BlueprintSourceMapTests::MakeStatement(30));
    Builder.AddLocation(6, BlueprintSourceMapTests::MakeStatement(40));
    Builder.TrimFrom(0);
    TestEqual(TEXT("a trim at zero drops every record"), Builder.Num(), 0);
    Builder.AddLocation(0, BlueprintSourceMapTests::MakeStatement(30));
    TestEqual(TEXT("the builder still records after a trim"), Builder.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapBuilderEmptyTest, "BlueprintInspector.SourceMap.BuilderHandlesUncoveredText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapBuilderEmptyTest::RunTest(const FString& Parameters)
{
    FBlueprintSourceMapBuilder Builder;

    TestEqual(TEXT("empty text has no lines"), Builder.Build(TEXT("")).Lines.Num(), 0);

    const FBlueprintSourceMap Uncovered = Builder.Build(TEXT("a\nb\n"));
    if (!TestEqual(TEXT("the uncovered text has two lines"), Uncovered.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("uncovered line 1 is not a statement"), Uncovered.Lines[0].Kind == EBlueprintLineKind::None);
    TestTrue(TEXT("uncovered line 2 is not a statement"), Uncovered.Lines[1].Kind == EBlueprintLineKind::None);
    TestFalse(TEXT("uncovered line 1 is not breakpointable"), Uncovered.Lines[0].IsBreakpointable());

    Builder.AddLocation(0, FBlueprintLineInfo{});
    const FBlueprintSourceMap Structural = Builder.Build(TEXT("a\nb\n"));
    if (!TestEqual(TEXT("the structural text has two lines"), Structural.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("a structural record stays None"), Structural.Lines[0].Kind == EBlueprintLineKind::None);
    TestFalse(TEXT("a structural line is not breakpointable"), Structural.Lines[0].IsBreakpointable());

    Builder.Reset();
    TestEqual(TEXT("reset drops the recorded locations"), Builder.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapReverseTest, "BlueprintInspector.SourceMap.BuilderBuildsReverseMap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapReverseTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass()), RF_Transient);
    if (Function == nullptr)
    {
        AddError(TEXT("Could not create the synthetic function."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepAlive(Function);

    FBlueprintLineInfo Statement;
    Statement.OwnerFunction = Function;
    Statement.ScriptIndex = 10;
    Statement.Kind = EBlueprintLineKind::Statement;

    FBlueprintLineInfo Continuation = Statement;
    Continuation.Kind = EBlueprintLineKind::SubStatement;

    FBlueprintLineInfo Other = Statement;
    Other.ScriptIndex = 20;

    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, Statement);
    Builder.AddLocation(3, Continuation);
    Builder.AddLocation(6, Other);

    const FBlueprintSourceMap Map = Builder.Build(TEXT("a;\nb;\nc;\n"));

    TestEqual(TEXT("one reverse entry per distinct address"), Map.LinesByAddress.Num(), 2);
    TestEqual(TEXT("the Statement line wins over its continuation"), Map.FindLine(Function, 10), 1);
    TestEqual(TEXT("the second address maps to its own line"), Map.FindLine(Function, 20), 3);
    TestEqual(TEXT("an address no line owns maps to nothing"), Map.FindLine(Function, 30), 0);
    TestEqual(TEXT("a null function maps to nothing"), Map.FindLine(nullptr, 10), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapBreakpointLineTest, "BlueprintInspector.SourceMap.BreakpointLineStaysInFunction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapBreakpointLineTest::RunTest(const FString& Parameters)
{
    UFunction* const FunctionA = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass()), RF_Transient);
    UFunction* const FunctionB = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass()), RF_Transient);
    if (FunctionA == nullptr || FunctionB == nullptr)
    {
        AddError(TEXT("Could not create the synthetic functions."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepA(FunctionA);
    TStrongObjectPtr<UFunction> KeepB(FunctionB);

    FBlueprintLineInfo StatementA;
    StatementA.OwnerFunction = FunctionA;
    StatementA.ScriptIndex = 10;
    StatementA.Kind = EBlueprintLineKind::Statement;

    FBlueprintLineInfo StatementB;
    StatementB.OwnerFunction = FunctionB;
    StatementB.ScriptIndex = 20;
    StatementB.Kind = EBlueprintLineKind::Statement;

    TArray<FBlueprintLineInfo> Lines;
    Lines.Add(StatementA);
    Lines.Add(FBlueprintLineInfo{});
    Lines.Add(StatementB);

    TestEqual(TEXT("a statement maps to itself"), FindBreakpointLine(Lines, 1), 1);
    TestEqual(TEXT("a structural line does not snap into the next function"), FindBreakpointLine(Lines, 2), 0);
    TestEqual(TEXT("the next function's statement maps to itself"), FindBreakpointLine(Lines, 3), 3);
    TestEqual(TEXT("line zero maps to nothing"), FindBreakpointLine(Lines, 0), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapNameFallbackTest, "BlueprintInspector.SourceMap.FindLineFallsBackToFunctionName", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapNameFallbackTest::RunTest(const FString& Parameters)
{
    // A PIE duplicate is a second class object with the same owner-class and function names, in a
    // package whose name carries the UEDPIE_<n>_ prefix and whose object name carries it too
    // (/Temp/UEDPIE_0_Foo.UEDPIE_0_Foo). The third package deliberately reuses the short class name to
    // prove the path identity keeps it out of the lookup.
    // Unique per run: a fixed /Temp package name returns the previous run's package (and its objects),
    // which made the duplicate-name scenario order-dependent.
    const FString UniqueId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString EditorPackageName = FString::Printf(TEXT("/Temp/BlueprintInspectorSourceMapOwner_%s"), *UniqueId);
    const FString PiePackageName = FString::Printf(TEXT("/Temp/UEDPIE_0_BlueprintInspectorSourceMapOwner_%s"), *UniqueId);
    const FString OtherPackageName = FString::Printf(TEXT("/Temp/BlueprintInspectorSourceMapOther_%s"), *UniqueId);
#if BI_ENGINE_AT_LEAST_5
    UPackage* const EditorPackage = CreatePackage(*EditorPackageName);
    UPackage* const PiePackage = CreatePackage(*PiePackageName);
    UPackage* const OtherPackage = CreatePackage(*OtherPackageName);
#else
    // 4.25's CreatePackage takes an outer; the UE5 overload without one does not exist yet.
    UPackage* const EditorPackage = CreatePackage(nullptr, *EditorPackageName);
    UPackage* const PiePackage = CreatePackage(nullptr, *PiePackageName);
    UPackage* const OtherPackage = CreatePackage(nullptr, *OtherPackageName);
#endif
    // Registered before the null check so a partial failure marks the packages that were created.
    ON_SCOPE_EXIT
    {
        // 5.x renamed the explicit-destruction mark; 4.25 and 4.27 only have MarkPendingKill.
#if BI_ENGINE_AT_LEAST_5
        if (EditorPackage != nullptr)
        {
            EditorPackage->MarkAsGarbage();
        }
        if (PiePackage != nullptr)
        {
            PiePackage->MarkAsGarbage();
        }
        if (OtherPackage != nullptr)
        {
            OtherPackage->MarkAsGarbage();
        }
#else
        if (EditorPackage != nullptr)
        {
            EditorPackage->MarkPendingKill();
        }
        if (PiePackage != nullptr)
        {
            PiePackage->MarkPendingKill();
        }
        if (OtherPackage != nullptr)
        {
            OtherPackage->MarkPendingKill();
        }
#endif
    };

    if (EditorPackage == nullptr || PiePackage == nullptr || OtherPackage == nullptr)
    {
        AddError(TEXT("Could not create the synthetic packages."));
        return false;
    }

    UClass* const EditorClass = NewObject<UClass>(EditorPackage, TEXT("SourceMapOwner"), RF_Transient);
    UClass* const PieClass = NewObject<UClass>(PiePackage, TEXT("UEDPIE_0_SourceMapOwner"), RF_Transient);
    UClass* const OtherClass = NewObject<UClass>(OtherPackage, TEXT("SourceMapOwner"), RF_Transient);
    if (EditorClass == nullptr || PieClass == nullptr || OtherClass == nullptr)
    {
        AddError(TEXT("Could not create the synthetic owner classes."));
        return false;
    }

    TStrongObjectPtr<UClass> KeepEditorClass(EditorClass);
    TStrongObjectPtr<UClass> KeepPieClass(PieClass);
    TStrongObjectPtr<UClass> KeepOtherClass(OtherClass);
    TestTrue(TEXT("the PIE duplicate carries the prefix in its object name"), PieClass->GetName().StartsWith(TEXT("UEDPIE_0_")));
    TestTrue(TEXT("the PIE owner is a different class object"), EditorClass != PieClass);
    TestEqual(TEXT("the unrelated package reuses the same short class name"), EditorClass->GetFName(), OtherClass->GetFName());

    UFunction* const EditorFunction = NewObject<UFunction>(EditorClass, TEXT("SourceMapDuplicateName"), RF_Transient);
    UFunction* const PieFunction = NewObject<UFunction>(PieClass, TEXT("SourceMapDuplicateName"), RF_Transient);
    UFunction* const OtherFunction = NewObject<UFunction>(OtherClass, TEXT("SourceMapDuplicateName"), RF_Transient);
    if (EditorFunction == nullptr || PieFunction == nullptr || OtherFunction == nullptr)
    {
        AddError(TEXT("Could not create the synthetic functions."));
        return false;
    }

    TStrongObjectPtr<UFunction> KeepEditor(EditorFunction);
    TStrongObjectPtr<UFunction> KeepPie(PieFunction);
    TStrongObjectPtr<UFunction> KeepOther(OtherFunction);
    if (!TestEqual(TEXT("the two functions share a name"), EditorFunction->GetFName(), PieFunction->GetFName()))
    {
        return false;
    }

    FBlueprintLineInfo Statement;
    Statement.OwnerFunction = EditorFunction;
    Statement.ScriptIndex = 42;
    Statement.Kind = EBlueprintLineKind::Statement;

    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, Statement);
    const FBlueprintSourceMap Map = Builder.Build(TEXT("stmt;\n"));

    TestEqual(TEXT("the editor function finds its line by pointer"), Map.FindLine(EditorFunction, 42), 1);
    TestEqual(TEXT("the PIE duplicate finds the same line through the normalized class path"), Map.FindLine(PieFunction, 42), 1);
    TestEqual(TEXT("a same-named class in another package does not claim the line"), Map.FindLine(OtherFunction, 42), 0);
    TestEqual(TEXT("an unknown address still maps to nothing"), Map.FindLine(PieFunction, 43), 0);

    TestEqual(TEXT("the editor function resolves a suspended offset"), Map.FindLineAtOrBefore(EditorFunction, 42), 1);
    TestEqual(TEXT("the PIE duplicate resolves a suspended offset through the normalized class path"), Map.FindLineAtOrBefore(PieFunction, 42), 1);
    TestEqual(TEXT("a same-named class in another package has no suspended offset"), Map.FindLineAtOrBefore(OtherFunction, 42), 0);

    FBlueprintLineInfo Transfer;
    Transfer.OwnerFunction = EditorFunction;
    Transfer.ScriptIndex = 44;
    Transfer.TransferTarget = 60;
    Transfer.Kind = EBlueprintLineKind::LoopTransfer;

    FBlueprintSourceMapBuilder TransferBuilder;
    TransferBuilder.AddLocation(0, Transfer);
    const FBlueprintSourceMap TransferMap = TransferBuilder.Build(TEXT("break;\n"));
    const int32* const PieTransferLine = TransferMap.LinesByTransfer.Find(FBlueprintTransferNameKey{GetFunctionClassPath(PieFunction), PieFunction->GetFName(), 44, 60});
    TestEqual(TEXT("the transfer key normalizes the PIE duplicate's class path"), PieTransferLine != nullptr ? *PieTransferLine : 0, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapAssembledIndexTest, "BlueprintInspector.SourceMap.AssembledClassMapRebuildsIndex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapAssembledIndexTest::RunTest(const FString& Parameters)
{
    // Unique names under the shared static outers: NewObject with a fixed name returns the previous
    // run's object, which made this test order-dependent.
    UFunction* const FunctionA = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass(), TEXT("ClassMapFunctionA")), RF_Transient);
    UFunction* const FunctionB = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass(), TEXT("ClassMapFunctionB")), RF_Transient);
    if (FunctionA == nullptr || FunctionB == nullptr)
    {
        AddError(TEXT("Could not create the synthetic functions."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepA(FunctionA);
    TStrongObjectPtr<UFunction> KeepB(FunctionB);

    FBlueprintLineInfo InfoA;
    InfoA.OwnerFunction = FunctionA;
    InfoA.ScriptIndex = 10;
    InfoA.Kind = EBlueprintLineKind::Statement;
    FBlueprintSourceMapBuilder BuilderA;
    BuilderA.AddLocation(0, InfoA);
    const FBlueprintSourceMap MapA = BuilderA.Build(TEXT("a();\n"));

    FBlueprintLineInfo InfoB;
    InfoB.OwnerFunction = FunctionB;
    InfoB.ScriptIndex = 20;
    InfoB.Kind = EBlueprintLineKind::Statement;
    FBlueprintSourceMapBuilder BuilderB;
    BuilderB.AddLocation(0, InfoB);
    const FBlueprintSourceMap MapB = BuilderB.Build(TEXT("b();\n"));

    FBlueprintSourceMap ClassMap;
    ClassMap.Lines.Append(MapA.Lines);
    ClassMap.Lines.AddDefaulted();
    ClassMap.Lines.Append(MapB.Lines);

    TestEqual(TEXT("the assembled map is not indexed yet"), ClassMap.FindLine(FunctionA, 10), 0);

    ClassMap.BuildReverseIndex();
    TestEqual(TEXT("the first function maps to its assembled line"), ClassMap.FindLine(FunctionA, 10), 1);
    TestEqual(TEXT("the second function maps past the separator"), ClassMap.FindLine(FunctionB, 20), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapAtOrBeforeTest, "BlueprintInspector.SourceMap.FindLineAtOrBefore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapAtOrBeforeTest::RunTest(const FString& Parameters)
{
    UFunction* const Caller = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass(), TEXT("CallerLineLookup")), RF_Transient);
    UFunction* const Other = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass(), TEXT("OtherLineLookup")), RF_Transient);
    if (Caller == nullptr || Other == nullptr)
    {
        AddError(TEXT("Could not create the synthetic functions."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepCaller(Caller);
    TStrongObjectPtr<UFunction> KeepOther(Other);

    const auto MakeLine = [](const UFunction* const Function, const int64 Address, const EBlueprintLineKind Kind) {
        FBlueprintLineInfo Info;
        Info.OwnerFunction = Function;
        Info.ScriptIndex = Address;
        Info.Kind = Kind;
        return Info;
    };

    FBlueprintSourceMap Map;
    Map.Lines.Add(MakeLine(Caller, 10, EBlueprintLineKind::Statement));
    Map.Lines.Add(FBlueprintLineInfo{});
    Map.Lines.Add(MakeLine(Caller, 10, EBlueprintLineKind::SubStatement));
    Map.Lines.Add(MakeLine(Caller, 40, EBlueprintLineKind::Statement));
    Map.Lines.Add(MakeLine(Other, 5, EBlueprintLineKind::Statement));
    Map.Lines.Add(MakeLine(Other, 90, EBlueprintLineKind::Statement));

    TestEqual(TEXT("an offset before the first statement has no line"), Map.FindLineAtOrBefore(Caller, 9), 0);
    TestEqual(TEXT("an exact statement offset maps to its line"), Map.FindLineAtOrBefore(Caller, 10), 1);
    TestEqual(TEXT("a caller suspended past the call reports the call line"), Map.FindLineAtOrBefore(Caller, 39), 1);
    TestEqual(TEXT("the nearest preceding statement wins over an earlier one"), Map.FindLineAtOrBefore(Caller, 40), 4);
    TestEqual(TEXT("an offset past the last statement keeps the last line"), Map.FindLineAtOrBefore(Caller, 100), 4);
    TestEqual(TEXT("another function's statements are ignored"), Map.FindLineAtOrBefore(Other, 50), 5);
    TestEqual(TEXT("the other function reaches its own later statement"), Map.FindLineAtOrBefore(Other, 90), 6);
    TestEqual(TEXT("a null function maps to nothing"), Map.FindLineAtOrBefore(nullptr, 40), 0);
    TestEqual(TEXT("a negative offset maps to nothing"), Map.FindLineAtOrBefore(Caller, INDEX_NONE), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapSequenceBoundaryTest, "BlueprintInspector.SourceMap.SequenceBoundarySharesStatementAddress", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapSequenceBoundaryTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass(), TEXT("SequenceBoundaryLookup")), RF_Transient);
    if (Function == nullptr)
    {
        AddError(TEXT("Could not create the synthetic function."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepAlive(Function);

    FBlueprintLineInfo Boundary;
    Boundary.OwnerFunction = Function;
    Boundary.ScriptIndex = 42;
    Boundary.Kind = EBlueprintLineKind::Sequence;

    FBlueprintLineInfo Statement = BlueprintSourceMapTests::MakeStatement(42);
    Statement.OwnerFunction = Function;

    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, Boundary);
    Builder.AddLocation(15, Statement);
    const FBlueprintSourceMap Shared = Builder.Build(TEXT("// Sequence 0.\nstmt;\n"));

    if (!TestEqual(TEXT("the boundary render has two lines"), Shared.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("the boundary line is breakpointable"), Shared.Lines[0].IsBreakpointable());
    TestTrue(TEXT("the boundary keeps its kind"), Shared.Lines[0].Kind == EBlueprintLineKind::Sequence);
    TestEqual(TEXT("a breakpoint on the boundary arms the boundary"), FindBreakpointLine(Shared.Lines, 1), 1);
    TestEqual(TEXT("the pin's statement is canonical for the shared address"), Shared.FindLine(Function, 42), 2);
    TestEqual(TEXT("a paused frame reports the statement, not the boundary"), Shared.FindLineAtOrBefore(Function, 42), 2);

    // An empty pin reaches no statement, so its boundary is the only line that owns the address.
    Builder.Reset();
    Builder.AddLocation(0, Boundary);
    const FBlueprintSourceMap Alone = Builder.Build(TEXT("// Sequence 0.\n"));
    TestEqual(TEXT("an empty pin's boundary covers its address"), Alone.FindLine(Function, 42), 1);
    TestEqual(TEXT("an empty pin's boundary arms itself"), FindBreakpointLine(Alone.Lines, 1), 1);

    // The marker is one line; text that inherits the record without a new location is not the boundary.
    Builder.Reset();
    Builder.AddLocation(0, Boundary);
    const FBlueprintSourceMap Continued = Builder.Build(TEXT("// Sequence 0.\nstray;\n"));
    if (!TestEqual(TEXT("the continued boundary render has two lines"), Continued.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("a line after a boundary stays structural"), Continued.Lines[1].Kind == EBlueprintLineKind::None);
    TestFalse(TEXT("a line after a boundary is not breakpointable"), Continued.Lines[1].IsBreakpointable());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapGotoTransferTest, "BlueprintInspector.SourceMap.GotoTransferSharesStatementAddress", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapGotoTransferTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass(), TEXT("GotoTransferLookup")), RF_Transient);
    if (Function == nullptr)
    {
        AddError(TEXT("Could not create the synthetic function."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepAlive(Function);

    FBlueprintLineInfo Transfer;
    Transfer.OwnerFunction = Function;
    Transfer.ScriptIndex = 42;
    Transfer.Kind = EBlueprintLineKind::Goto;

    FBlueprintLineInfo Statement = BlueprintSourceMapTests::MakeStatement(42);
    Statement.OwnerFunction = Function;

    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, Transfer);
    Builder.AddLocation(13, Statement);
    const FBlueprintSourceMap Shared = Builder.Build(TEXT("goto _0x02A;\nstmt;\n"));

    if (!TestEqual(TEXT("the transfer render has two lines"), Shared.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("the transfer line is breakpointable"), Shared.Lines[0].IsBreakpointable());
    TestTrue(TEXT("the transfer keeps its kind"), Shared.Lines[0].Kind == EBlueprintLineKind::Goto);
    TestEqual(TEXT("a breakpoint on the transfer arms the transfer line"), FindBreakpointLine(Shared.Lines, 1), 1);
    TestEqual(TEXT("the target statement is canonical for the shared address"), Shared.FindLine(Function, 42), 2);
    TestEqual(TEXT("a paused frame reports the statement, not the transfer"), Shared.FindLineAtOrBefore(Function, 42), 2);

    // A transfer whose target has no line (for example a trimmed return) owns the address itself.
    Builder.Reset();
    Builder.AddLocation(0, Transfer);
    const FBlueprintSourceMap Alone = Builder.Build(TEXT("goto _0x02A;\n"));
    TestEqual(TEXT("a transfer with no statement covers its address"), Alone.FindLine(Function, 42), 1);

    // The marker is one line; text that inherits the record without a new location is not the transfer.
    Builder.Reset();
    Builder.AddLocation(0, Transfer);
    const FBlueprintSourceMap Continued = Builder.Build(TEXT("goto _0x02A;\nstray;\n"));
    if (!TestEqual(TEXT("the continued transfer render has two lines"), Continued.Lines.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("a line after a transfer stays structural"), Continued.Lines[1].Kind == EBlueprintLineKind::None);
    TestEqual(TEXT("an inherited transfer line does not keep the address"), Continued.Lines[1].ScriptIndex, static_cast<int64>(INDEX_NONE));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPiePrefixStripTest, "BlueprintInspector.SourceMap.PiePrefixStripping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPiePrefixStripTest::RunTest(const FString& Parameters)
{
    const auto Stripped = [](const TCHAR* Input) {
        FString Path(Input);
        StripPiePackagePrefixesInline(Path);
        return Path;
    };

    TestEqual(TEXT("the package and object prefixes are both removed"), Stripped(TEXT("/Game/BP/UEDPIE_0_Foo.UEDPIE_0_Foo_C")), FString(TEXT("/Game/BP/Foo.Foo_C")));
    TestEqual(TEXT("a multi-digit generation is removed"), Stripped(TEXT("/Game/UEDPIE_12_Bar")), FString(TEXT("/Game/Bar")));
    TestEqual(TEXT("a prefix with no digits is left alone"), Stripped(TEXT("/Game/UEDPIE_Foo")), FString(TEXT("/Game/UEDPIE_Foo")));
    TestEqual(TEXT("a prefix with no trailing underscore is left alone"), Stripped(TEXT("/Game/UEDPIE_12Foo")), FString(TEXT("/Game/UEDPIE_12Foo")));
    TestEqual(TEXT("a well-formed prefix after a malformed one is removed"), Stripped(TEXT("/Game/UEDPIE_X/UEDPIE_1_Bar")), FString(TEXT("/Game/UEDPIE_X/Bar")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceMapTransferTest, "BlueprintInspector.SourceMap.TransferLinesIndexByJumpAndLanding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceMapTransferTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = NewObject<UFunction>(UObject::StaticClass(), MakeUniqueObjectName(UObject::StaticClass(), UFunction::StaticClass()), RF_Transient);
    if (Function == nullptr)
    {
        AddError(TEXT("Could not create the synthetic function."));
        return false;
    }
    TStrongObjectPtr<UFunction> KeepAlive(Function);

    FBlueprintLineInfo Landed;
    Landed.OwnerFunction = Function;
    Landed.ScriptIndex = 30;
    Landed.Kind = EBlueprintLineKind::Statement;

    FBlueprintLineInfo Transfer;
    Transfer.OwnerFunction = Function;
    Transfer.ScriptIndex = 10;
    Transfer.TransferTarget = 30;
    Transfer.Kind = EBlueprintLineKind::LoopTransfer;

    FBlueprintSourceMapBuilder Builder;
    Builder.AddLocation(0, Landed);
    Builder.AddLocation(6, Transfer);

    const FBlueprintSourceMap Map = Builder.Build(TEXT("next;\nbreak;\n"));

    TestTrue(TEXT("the transfer line is breakpointable"), Map.IsBreakpointable(2));
    TestEqual(TEXT("the transfer line is not in the address index"), Map.LinesByAddress.Num(), 1);
    TestEqual(TEXT("the transfer line is not in the path index"), Map.LinesByAddressByPath.Num(), 1);
    TestEqual(TEXT("the jump/landing pair is indexed"), Map.LinesByTransfer.Num(), 1);
    const int32* const TransferLine = Map.LinesByTransfer.Find(FBlueprintTransferNameKey{GetFunctionClassPath(Function), Function->GetFName(), 10, 30});
    if (!TestNotNull(TEXT("the jump/landing pair names the function and addresses"), TransferLine))
    {
        return false;
    }
    TestEqual(TEXT("the jump/landing pair points at the transfer line"), *TransferLine, 2);

    FBlueprintLineInfo Invalid = Transfer;
    Invalid.TransferTarget = INDEX_NONE;
    FBlueprintSourceMapBuilder InvalidBuilder;
    InvalidBuilder.AddLocation(0, Landed);
    InvalidBuilder.AddLocation(6, Invalid);
    const FBlueprintSourceMap InvalidMap = InvalidBuilder.Build(TEXT("next;\nbreak;\n"));

    TestEqual(TEXT("an invalid target is not indexed"), InvalidMap.LinesByTransfer.Num(), 0);
    TestFalse(TEXT("an invalid target is not breakpointable"), InvalidMap.IsBreakpointable(2));
    return true;
}

#endif
