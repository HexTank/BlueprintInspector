
#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "Engine/Blueprint.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "BlueprintClassRegistry.h"
#include "BlueprintFixtureSource.h"
#include "BlueprintHotCapture.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorDAP.h"
#include "BlueprintInspectorSubsystem.h"
#include "BlueprintInspectorVariableEdit.h"
#include "BlueprintOpcodeLogger.h"
#include "BlueprintPauseSession.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"
#include "BlueprintStructDefaults.h"
#include "BlueprintTestConfiguration.h"
#include "BlueprintTestKeepAlives.h"
#include "OpcodeProbeHost.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace BlueprintInspectorTests
{
// Built bytecode, kept alive for the whole run. FunctionToNodes() re-constructs its reader over
// UFunction::Script by reference, and the visualizer caches rendered names by raw function pointer, so a
// collected function could otherwise hand its address and its cached name to a later test.
TArray<TStrongObjectPtr<UFunction>> GScriptFunctions;

// Called from the plugin module's ShutdownModule; see BlueprintTestKeepAlives.h.
void ReleaseScriptFunctions()
{
    if (!IsInGameThread())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Script function release skipped because it ran off the game thread."));
        return;
    }
    GScriptFunctions.Reset();
}

// AddExpectedErrorPlain escaped plain text; UE 4.25 only has the regex/wildcard AddExpectedError.
void AddExpectedPlainError(FAutomationTestBase& Test, const TCHAR* Message, const int32 Occurrences)
{
#if BI_ENGINE_AT_LEAST_5_4
    Test.AddExpectedErrorPlain(Message, EAutomationExpectedErrorFlags::Contains, Occurrences);
#else
    Test.AddExpectedError(Message, EAutomationExpectedErrorFlags::Contains, Occurrences);
#endif
}

int32 GFunctionCounter = 0;

// FBlueprintScriptArchiveReader::Serialize() copies raw bytes straight out of Script, so operands are
// written in the host's own byte order. This module only builds for Win64.
void AppendOpcode(TArray<uint8>& Script, const EExprToken Opcode)
{
    Script.Add(static_cast<uint8>(Opcode));
}

void AppendInt32(TArray<uint8>& Script, const int32 Value)
{
    Script.Append(reinterpret_cast<const uint8*>(&Value), static_cast<int32>(sizeof(Value)));
}

// UFunction is UCLASS(Within=UClass), so its outer has to be a UClass: an outer that is not one trips the
// editor-only ClassWithin validation in StaticAllocateObjectErrorTests (a Warning plus ensureMsgf, which
// fails whichever test is running). See UObjectGlobals.cpp:3355-3363. Creating the function does not register
// it in the class's function map (that takes an explicit AddFunctionToFunctionMap call), so an existing
// class can be used as the owner without mutating it.
UFunction* MakeFunction(const TCHAR* const Name, TArray<uint8>&& Script)
{
    const FName UniqueName = *FString::Printf(TEXT("%s_%d"), Name, ++GFunctionCounter);

    UFunction* const Function = NewObject<UFunction>(UObject::StaticClass(), UniqueName, RF_Transient);
    Function->Script = MoveTemp(Script);
    GScriptFunctions.Add(TStrongObjectPtr<UFunction>(Function));

    return Function;
}

FString RenderFunction(const UFunction* const Function)
{
    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Function);

    FString Script;
    FBlueprintScriptVisualizer_Pseudo Visualizer;
    Visualizer.Visualize(Function, Disassembler, Script);

    return Script;
}

FBlueprintRenderedSource RenderFunctionWithMap(const UFunction* const Function)
{
    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Function);

    FBlueprintRenderedSource Rendered;
    FBlueprintScriptVisualizer_Pseudo Visualizer;
    Visualizer.Visualize(Function, Disassembler, Rendered.Text, &Rendered.Map);

    return Rendered;
}

FBlueprintNode* FindSwitchValueNode(FBlueprintNode* const Node, TSet<const FBlueprintNode*>& Visited)
{
    if (Node == nullptr || Visited.Contains(Node))
    {
        return nullptr;
    }
    Visited.Add(Node);

    if (Node->Opcode == EX_SwitchValue && Node->InputNodes.Num() > 1)
    {
        return Node;
    }

    for (const FBlueprintNode::FWireVariant& Wire : Node->OutputWires)
    {
        if (Wire.IsType<FBlueprintNode*>())
        {
            if (FBlueprintNode* const Found = FindSwitchValueNode(Wire.Get<FBlueprintNode*>(), Visited))
            {
                return Found;
            }
        }
    }
    for (FBlueprintNode* const Input : Node->InputWires)
    {
        if (FBlueprintNode* const Found = FindSwitchValueNode(Input, Visited))
        {
            return Found;
        }
    }
    for (const FBlueprintNode::FNodeVariant& Input : Node->InputNodes)
    {
        if (Input.IsType<FBlueprintNode*>())
        {
            if (FBlueprintNode* const Found = FindSwitchValueNode(Input.Get<FBlueprintNode*>(), Visited))
            {
                return Found;
            }
        }
    }
    return nullptr;
}

FBlueprintNode* FindSwitchValueNode(FBlueprintScriptDisassembler& Disassembler)
{
    TSet<const FBlueprintNode*> Visited;
    for (const FBlueprintNode::FWireVariant& Wire : Disassembler.GetEntryPointNode().OutputWires)
    {
        if (Wire.IsType<FBlueprintNode*>())
        {
            if (FBlueprintNode* const Found = FindSwitchValueNode(Wire.Get<FBlueprintNode*>(), Visited))
            {
                return Found;
            }
        }
    }
    return nullptr;
}

void AppendOperand(TArray<uint8>& Script, const uint32 Value)
{
    Script.Append(reinterpret_cast<const uint8*>(&Value), static_cast<int32>(sizeof(Value)));
}

void PatchOperand(TArray<uint8>& Script, const int32 Offset, const uint32 Value)
{
    checkf(Offset >= 0 && static_cast<int64>(Offset) + static_cast<int64>(sizeof(Value)) <= Script.Num(), TEXT("PatchOperand offset is outside the script."));
    FMemory::Memcpy(Script.GetData() + Offset, &Value, sizeof(Value));
}

TArray<uint8> MakeConvergingReturnScript(uint32* const OutReturnValueAddress = nullptr)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_JumpIfNot);
    const int32 FalsePathOperand = Script.Num();
    AppendOperand(Script, 0);

    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    const uint32 ReturnAddress = Script.Num();
    AppendOpcode(Script, EX_Return);
    // The value token that follows EX_Return is the opcode the VM dispatches for the return statement.
    if (OutReturnValueAddress != nullptr)
    {
        *OutReturnValueAddress = Script.Num();
    }
    AppendOpcode(Script, EX_Nothing);
    const uint32 FalsePath = Script.Num();
    AppendOpcode(Script, EX_Jump);
    AppendOperand(Script, ReturnAddress);

    AppendOpcode(Script, EX_EndOfScript);
    PatchOperand(Script, FalsePathOperand, FalsePath);
    return Script;
}

static_assert(sizeof(void*) == 8, "Synthetic scripts carry host pointers; only 64-bit hosts are supported.");

void AppendPointer(TArray<uint8>& Script, const void* const Pointer)
{
    Script.Append(reinterpret_cast<const uint8*>(&Pointer), static_cast<int32>(sizeof(Pointer)));
}

TArray<uint8> MakeWhileLoopScript(const UFunction* const Callee)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_JumpIfNot);
    const int32 ExitOperand = Script.Num();
    AppendOperand(Script, 0);

    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_Jump);
    AppendOperand(Script, 0);

    const uint32 ExitAddress = Script.Num();
    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_Nothing);
    AppendOpcode(Script, EX_EndOfScript);
    PatchOperand(Script, ExitOperand, ExitAddress);
    return Script;
}

TArray<uint8> MakeDoWhileScript(const UFunction* const Callee)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_JumpIfNot);
    const int32 ExitOperand = Script.Num();
    AppendOperand(Script, 0);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    AppendOpcode(Script, EX_Jump);
    AppendOperand(Script, 0);

    const uint32 ExitAddress = Script.Num();
    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_Nothing);
    AppendOpcode(Script, EX_EndOfScript);
    PatchOperand(Script, ExitOperand, ExitAddress);
    return Script;
}

TArray<uint8> MakeBreakLoopScript(const UFunction* const Callee)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_JumpIfNot);
    const int32 ExitOperand = Script.Num();
    AppendOperand(Script, 0);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_JumpIfNot);
    const int32 BreakOperand = Script.Num();
    AppendOperand(Script, 0);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_Jump);
    AppendOperand(Script, 0);

    const uint32 ExitAddress = Script.Num();
    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_Nothing);
    AppendOpcode(Script, EX_EndOfScript);
    PatchOperand(Script, ExitOperand, ExitAddress);
    PatchOperand(Script, BreakOperand, ExitAddress);
    return Script;
}

TArray<uint8> MakeContinueLoopScript(const UFunction* const Callee)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_JumpIfNot);
    const int32 ExitOperand = Script.Num();
    AppendOperand(Script, 0);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_JumpIfNot);
    const int32 ContinueOperand = Script.Num();
    AppendOperand(Script, 0);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);

    AppendOpcode(Script, EX_Jump);
    AppendOperand(Script, 0);

    const uint32 AfterContinue = Script.Num();
    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_Jump);
    AppendOperand(Script, 0);

    const uint32 ExitAddress = Script.Num();
    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_Nothing);
    AppendOpcode(Script, EX_EndOfScript);
    PatchOperand(Script, ExitOperand, ExitAddress);
    PatchOperand(Script, ContinueOperand, AfterContinue);
    return Script;
}

TArray<uint8> MakeRepeatedSequencePinScript(const UFunction* const Callee)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_PushExecutionFlow);
    const int32 PinOperand = Script.Num();
    AppendOperand(Script, 0);

    AppendOpcode(Script, EX_Jump);
    const int32 JumpOperand = Script.Num();
    AppendOperand(Script, 0);

    const uint32 PinAddress = Script.Num();
    AppendOpcode(Script, EX_CallMath);
    AppendPointer(Script, Callee);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, 1);
    AppendOpcode(Script, EX_EndFunctionParms);

    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_Nothing);
    AppendOpcode(Script, EX_EndOfScript);
    PatchOperand(Script, PinOperand, PinAddress);
    PatchOperand(Script, JumpOperand, PinAddress);
    return Script;
}

TArray<uint8> MakeReturnValueScript(const int32 Value)
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_IntConst);
    AppendInt32(Script, Value);
    AppendOpcode(Script, EX_EndOfScript);
    return Script;
}

TArray<uint8> MakeVoidReturnScript()
{
    TArray<uint8> Script;
    AppendOpcode(Script, EX_Return);
    AppendOpcode(Script, EX_Nothing);
    AppendOpcode(Script, EX_EndOfScript);
    return Script;
}

TArray<EExprToken> CaptureOpcodes(const UFunction* const Function)
{
    TArray<EExprToken> Tokens;
    FBlueprintOpcodeLogger::SetOpcodeSink([&Tokens](const EExprToken Opcode, const int64 /*ScriptIndex*/) { Tokens.Add(Opcode); });
    ON_SCOPE_EXIT
    {
        FBlueprintOpcodeLogger::ClearOpcodeSink();
    };

    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Function);
    return Tokens;
}

bool BracesAreBalanced(const FString& Text)
{
    int32 Depth = 0;
    bool bInString = false;
    bool bInCharLiteral = false;
    bool bInLineComment = false;
    bool bInBlockComment = false;
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR Character = Text[Index];
        if (bInLineComment)
        {
            bInLineComment = Character != TEXT('\n');
            continue;
        }
        if (bInBlockComment)
        {
            if (Character == TEXT('*') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/'))
            {
                bInBlockComment = false;
                ++Index;
            }
            continue;
        }
        if (bInString)
        {
            if (Character == TEXT('\\') && Index + 1 < Text.Len())
            {
                ++Index;
            }
            else if (Character == TEXT('"'))
            {
                bInString = false;
            }
            continue;
        }
        if (bInCharLiteral)
        {
            if (Character == TEXT('\\') && Index + 1 < Text.Len())
            {
                ++Index;
            }
            else if (Character == TEXT('\''))
            {
                bInCharLiteral = false;
            }
            continue;
        }
        if (Character == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/'))
        {
            bInLineComment = true;
            ++Index;
            continue;
        }
        if (Character == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('*'))
        {
            bInBlockComment = true;
            ++Index;
            continue;
        }
        if (Character == TEXT('"'))
        {
            bInString = true;
            continue;
        }
        if (Character == TEXT('\''))
        {
            bInCharLiteral = true;
            continue;
        }
        if (Character == TEXT('{'))
        {
            ++Depth;
        }
        else if (Character == TEXT('}'))
        {
            --Depth;
            if (Depth < 0)
            {
                return false;
            }
        }
    }
    return !bInString && !bInCharLiteral && !bInBlockComment && Depth == 0;
}

void CollectGraphAddresses(const FBlueprintNode* const Node, TSet<int64>& OutAddresses, TSet<const FBlueprintNode*>& Visited, const TSet<const FBlueprintNode*>* const FoldedBranches = nullptr)
{
    if (Node == nullptr || Visited.Contains(Node))
    {
        return;
    }
    Visited.Add(Node);

    const bool bFolded = FoldedBranches != nullptr && FoldedBranches->Contains(Node);
    if (!bFolded && Node->Opcode != EX_Max && Node->Opcode != EX_PushExecutionFlow && Node->Opcode != EX_PopExecutionFlow && Node->Opcode != EX_Return && !(Node->Opcode == EX_JumpIfNot && Node->BackEdgeTarget != INDEX_NONE))
    {
        OutAddresses.Add(Node->ScriptIndex);
    }

    // Only the output side is walked: a node reached through an input wire renders inside the expression
    // that consumes it instead of owning a line of its own, so it has no address to assert on.
    for (const FBlueprintNode::FWireVariant& Wire : Node->OutputWires)
    {
        if (Wire.IsType<FBlueprintNode*>())
        {
            CollectGraphAddresses(Wire.Get<FBlueprintNode*>(), OutAddresses, Visited, FoldedBranches);
        }
    }
}

FString MakeGoldenManifest(const TArray<TPair<FString, FString>>& Entries, const FString& Title, const FString& Description)
{
    FString Manifest = FString::Printf(TEXT("# BlueprintInspector %s golden.\n"), *Title);
    Manifest += TEXT("# Regenerate with -BlueprintGoldenUpdate -BlueprintGoldenVariant (or -BlueprintGoldenUpdateShared for the")
        TEXT(" shared file), then review the diff before committing.\n");
    Manifest += FString::Printf(TEXT("# %s\n"), *Description);

    for (const TPair<FString, FString>& Entry : Entries)
    {
        Manifest += FString::Printf(TEXT("===== %s =====\n"), *Entry.Key);
        Manifest += Entry.Value;
        if (!Manifest.EndsWith(TEXT("\n")))
        {
            Manifest += TEXT("\n");
        }
    }

    return Manifest;
}

void ParseGoldenManifest(FAutomationTestBase& Test, const FString& Manifest, TMap<FString, FString>& OutSections)
{
    static const FString SectionPrefix = TEXT("===== ");
    static const FString SectionSuffix = TEXT(" =====");

    TArray<FString> Lines;
    Manifest.ParseIntoArrayLines(Lines, false);
    if (Lines.Num() > 0 && Lines.Last().IsEmpty())
    {
        Lines.Pop();
    }

    FString Current;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(SectionPrefix) && Line.EndsWith(SectionSuffix) && Line.Len() > SectionPrefix.Len() + SectionSuffix.Len())
        {
            Current = Line.Mid(SectionPrefix.Len(), Line.Len() - SectionPrefix.Len() - SectionSuffix.Len());
            if (OutSections.Contains(Current))
            {
                Test.AddError(FString::Printf(TEXT("Duplicate golden section '%s'."), *Current));
                Current.Reset();
                continue;
            }
            OutSections.Add(Current, FString());
        }
        else if (!Current.IsEmpty())
        {
            OutSections[Current] += Line + TEXT("\n");
        }
    }
}

void ReportGoldenDiff(FAutomationTestBase& Test, const FString& FunctionName, const FString& Expected, const FString& Actual)
{
    TArray<FString> ExpectedLines;
    TArray<FString> ActualLines;
    Expected.ParseIntoArrayLines(ExpectedLines, false);
    Actual.ParseIntoArrayLines(ActualLines, false);
    if (ExpectedLines.Num() > 0 && ExpectedLines.Last().IsEmpty())
    {
        ExpectedLines.Pop();
    }
    if (ActualLines.Num() > 0 && ActualLines.Last().IsEmpty())
    {
        ActualLines.Pop();
    }

    int32 Reported = 0;
    const int32 LineCount = FMath::Max(ExpectedLines.Num(), ActualLines.Num());
    for (int32 Index = 0; Index < LineCount && Reported < 3; ++Index)
    {
        const FString ExpectedLine = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index] : TEXT("<missing>");
        const FString ActualLine = ActualLines.IsValidIndex(Index) ? ActualLines[Index] : TEXT("<missing>");
        if (ExpectedLine != ActualLine)
        {
            Test.AddError(FString::Printf(TEXT("%s: line %d: golden '%s' vs rendered '%s'"), *FunctionName, Index + 1, *ExpectedLine, *ActualLine));
            ++Reported;
        }
    }
}

FString FormatSourceMap(const FBlueprintSourceMap& InMap)
{
    FString Out;
    for (int32 Index = 0; Index < InMap.Lines.Num(); ++Index)
    {
        const FBlueprintLineInfo& Info = InMap.Lines[Index];
        const TCHAR* Kind = TEXT("none");
        if (Info.Kind == EBlueprintLineKind::Statement)
        {
            Kind = TEXT("statement");
        }
        else if (Info.Kind == EBlueprintLineKind::Sequence)
        {
            Kind = TEXT("sequence");
        }
        else if (Info.Kind == EBlueprintLineKind::Goto)
        {
            Kind = TEXT("goto");
        }
        else if (Info.Kind == EBlueprintLineKind::SubStatement)
        {
            Kind = TEXT("sub");
        }
        else if (Info.Kind == EBlueprintLineKind::LoopTransfer)
        {
            Kind = TEXT("transfer");
        }

        if (Info.IsBreakpointable() && Info.Kind == EBlueprintLineKind::LoopTransfer && Info.OwnerFunction.IsValid())
        {
            // The dispatch pair, so the golden pins both the jump opcode and the landing the VM holds.
            Out += FString::Printf(TEXT("%d|%s|%s@%lld->%lld\n"), Index + 1, Kind, *Info.OwnerFunction->GetName(), Info.ScriptIndex, Info.TransferTarget);
        }
        else if (Info.IsBreakpointable())
        {
            const UFunction* const Owner = Info.OwnerFunction.Get();
            Out += FString::Printf(TEXT("%d|%s|%s@%lld\n"), Index + 1, Kind, Owner ? *Owner->GetName() : TEXT("<none>"), Info.ScriptIndex);
        }
        else
        {
            Out += FString::Printf(TEXT("%d|%s\n"), Index + 1, Kind);
        }
    }
    return Out;
}

// A golden for one engine is "<BaseName>.<major>.<minor>.golden" next to the shared "<BaseName>.golden".
// The shared file is the reference engine's; any other engine records its own variant so a per-engine
// compiler difference never weakens the shared expectation.
FString GoldenPathForEngine(const FString& GoldenDir, const FString& BaseName, const bool bVariant)
{
    return FPaths::Combine(GoldenDir, bVariant ? FString::Printf(TEXT("%s.%s.golden"), *BaseName, *GetBlueprintEngineTag()) : FString::Printf(TEXT("%s.golden"), *BaseName));
}

void CheckGoldenEntries(FAutomationTestBase& Test, const FString& Label, const FString& Title, const FString& Description, const FString& GoldenDir, const FString& BaseName, const TArray<TPair<FString, FString>>& Entries, const bool bUpdate, const bool bWriteVariant)
{
    const FString VariantPath = GoldenPathForEngine(GoldenDir, BaseName, true);
    const FString BasePath = GoldenPathForEngine(GoldenDir, BaseName, false);
    const bool bHasVariant = IFileManager::Get().FileExists(*VariantPath);

    // The shared file is the reference expectation. Only an explicit opt-in may rewrite it; inferring
    // that from a variant's existence lets the first update run on another engine replace it silently.
    const bool bWriteShared = FParse::Param(FCommandLine::Get(), TEXT("BlueprintGoldenUpdateShared"));
    if (bUpdate && !bWriteVariant && !bWriteShared)
    {
        Test.AddError(FString::Printf(TEXT("Refusing to overwrite the shared %s golden from %s. Pass -BlueprintGoldenVariant to update this engine's variant, or -BlueprintGoldenUpdateShared to replace the shared expectation."), *Label, *GetBlueprintEngineTag()));
        return;
    }

    const FString GoldenPath = bUpdate ? (bWriteVariant ? VariantPath : BasePath) : (bHasVariant ? VariantPath : BasePath);

    const FString Manifest = MakeGoldenManifest(Entries, Title, Description);

    if (bUpdate)
    {
        if (!FFileHelper::SaveStringToFile(Manifest, *GoldenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            Test.AddError(FString::Printf(TEXT("Could not write the %s golden to %s"), *Label, *GoldenPath));
        }
        else
        {
            Test.AddInfo(FString::Printf(TEXT("Wrote %d %s section(s) to %s"), Entries.Num(), *Label, *GoldenPath));
        }
        return;
    }

    FString Golden;
    if (!FFileHelper::LoadFileToString(Golden, *GoldenPath))
    {
        Test.AddError(FString::Printf(TEXT("No %s golden at %s. Generate it with -BlueprintGoldenUpdate "
                                           "-BlueprintGoldenVariant (or add -BlueprintGoldenUpdateShared to replace the "
                                           "shared expectation)."),
                                      *Label, *GoldenPath));
        return;
    }
    Golden.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

    TMap<FString, FString> Sections;
    ParseGoldenManifest(Test, Golden, Sections);

    TSet<FString> EntryNames;
    int32 Compared = 0;
    for (const TPair<FString, FString>& Entry : Entries)
    {
        EntryNames.Add(Entry.Key);

        const FString* const Expected = Sections.Find(Entry.Key);
        if (Expected == nullptr)
        {
            Test.AddError(FString::Printf(TEXT("%s: not present in the %s golden"), *Entry.Key, *Label));
            continue;
        }

        ++Compared;
        FString Actual = Entry.Value;
        Actual.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
        if (*Expected != Actual)
        {
            ReportGoldenDiff(Test, Entry.Key, *Expected, Actual);
        }
    }

    for (const TPair<FString, FString>& Section : Sections)
    {
        if (!EntryNames.Contains(Section.Key))
        {
            Test.AddError(FString::Printf(TEXT("%s: %s golden section has no matching fixture function"), *Section.Key, *Label));
        }
    }

    Test.TestEqual(FString::Printf(TEXT("compared every %s section"), *Label), Compared, Entries.Num());
    Test.AddInfo(FString::Printf(TEXT("Compared %d %s section(s) against %s"), Compared, *Label, *GoldenPath));
}
} // namespace BlueprintInspectorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorReturnValueTest, "BlueprintInspector.Disassembler.ReturnValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorReturnValueTest::RunTest(const FString& Parameters)
{
    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestIntValue"), BlueprintInspectorTests::MakeReturnValueScript(3));

    const FString Script = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Script);

    TestTrue(TEXT("renders the function header"), Script.Contains(TEXT("BlueprintInspectorTestIntValue")));
    TestTrue(TEXT("a returned constant reaches the output"), Script.Contains(TEXT("return 3;")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorVoidReturnTest, "BlueprintInspector.Disassembler.VoidReturnIsDropped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorVoidReturnTest::RunTest(const FString& Parameters)
{
    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestVoidBody"), BlueprintInspectorTests::MakeVoidReturnScript());

    const FString Script = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Script);

    TestTrue(TEXT("renders the function header"), Script.Contains(TEXT("BlueprintInspectorTestVoidBody")));
    TestFalse(TEXT("an implicit void return is not rendered"), Script.Contains(TEXT("return")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReturnLineBreakpointTest, "BlueprintInspector.SourceMap.ReturnLineIsBreakpointable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintReturnLineBreakpointTest::RunTest(const FString& Parameters)
{
    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestReturnLine"), BlueprintInspectorTests::MakeReturnValueScript(3));
    const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);

    TArray<FString> Lines;
    Rendered.Text.ParseIntoArrayLines(Lines, false);

    int32 ReturnLine = INDEX_NONE;
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        if (Lines[Index].TrimStartAndEnd().StartsWith(TEXT("return")))
        {
            ReturnLine = Index;
            break;
        }
    }

    TestTrue(TEXT("the render has a return line"), ReturnLine != INDEX_NONE);
    if (ReturnLine == INDEX_NONE)
    {
        return true;
    }
    if (!TestTrue(TEXT("the return line has a source-map entry"), Rendered.Map.Lines.IsValidIndex(ReturnLine)))
    {
        return true;
    }

    const FBlueprintLineInfo& Info = Rendered.Map.Lines[ReturnLine];
    const int32 Address = static_cast<int32>(Info.ScriptIndex);
    const bool bAddressValid = Function->Script.IsValidIndex(Address);
    const int32 Token = bAddressValid ? static_cast<int32>(Function->Script[Address]) : -1;

    TestTrue(TEXT("the return is a statement"), Info.Kind == EBlueprintLineKind::Statement);
    TestTrue(TEXT("the return line is breakpointable"), Info.IsBreakpointable());
    TestTrue(TEXT("the return address is in the script"), bAddressValid);
    TestTrue(TEXT("the return address is not EX_Return"), Token != static_cast<int32>(EX_Return));
    TestEqual(TEXT("the return address is the operand token"), Token, static_cast<int32>(EX_IntConst));
    AddInfo(FString::Printf(TEXT("return line %d: address %d, token 0x%X"), ReturnLine + 1, Address, Token));
    return true;
}

// A do/while's tail condition is dispatched by the VM on every iteration, so its line has to own the
// condition statement's address.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDoWhileConditionBreakpointTest, "BlueprintInspector.SourceMap.DoWhileConditionIsBreakpointable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDoWhileConditionBreakpointTest::RunTest(const FString& Parameters)
{
    const UFunction* const Function = BlueprintFixtureSource::FindFunction(TEXT("flow_do_while"));
    if (Function == nullptr)
    {
        AddError(TEXT("flow_do_while is missing from the fixture"));
        return false;
    }

    const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
    TArray<FString> Lines;
    Rendered.Text.ParseIntoArrayLines(Lines, false);

    int32 ConditionLine = INDEX_NONE;
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        if (Lines[Index].TrimStartAndEnd().StartsWith(TEXT("while (")))
        {
            ConditionLine = Index;
            break;
        }
    }

    TestTrue(TEXT("the render has a do/while tail condition"), ConditionLine != INDEX_NONE);
    if (ConditionLine == INDEX_NONE || !Rendered.Map.Lines.IsValidIndex(ConditionLine))
    {
        return true;
    }

    const FBlueprintLineInfo& Info = Rendered.Map.Lines[ConditionLine];
    const int32 Address = static_cast<int32>(Info.ScriptIndex);

    TestTrue(TEXT("the tail condition is a statement"), Info.Kind == EBlueprintLineKind::Statement);
    TestTrue(TEXT("the tail condition line is breakpointable"), Info.IsBreakpointable());
    TestTrue(TEXT("the tail condition address is in the script"), Function->Script.IsValidIndex(Address));
    TestEqual(TEXT("the address reports the condition line, not another line"), Rendered.Map.FindLine(Function, Info.ScriptIndex), ConditionLine + 1);
    AddInfo(FString::Printf(TEXT("tail condition line %d: address %d"), ConditionLine + 1, Address));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintLoopTransferBreakpointTest, "BlueprintInspector.SourceMap.LoopTransferIsBreakpointable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintLoopTransferBreakpointTest::RunTest(const FString& Parameters)
{
    // A `break;`/`continue;` arm owns no address of its own: its jump opcode is dispatched for both arms, so
    // an address breakpoint on it would fire on every condition evaluation. The line records the dispatch
    // pair instead. flow_nested_loops is in the list on purpose: its arms come through a conditional jump,
    // whose own offset differs from the last opcode of the condition it decodes.
    const TCHAR* const Cases[] = {TEXT("flow_continue"), TEXT("flow_break"), TEXT("flow_nested_loops")};

    for (const TCHAR* const Name : Cases)
    {
        const UFunction* const Function = BlueprintFixtureSource::FindFunction(FName(Name));
        if (Function == nullptr)
        {
            AddError(FString::Printf(TEXT("%s is missing from the fixture"), Name));
            continue;
        }

        const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
        TArray<FString> Lines;
        Rendered.Text.ParseIntoArrayLines(Lines, false);

        // Every break/continue line has to name a real dispatch: the offset must hold a jump opcode and the
        // landing must be inside the script.
        int32 Transfers = 0;
        for (int32 Index = 0; Index < Rendered.Map.Lines.Num(); ++Index)
        {
            const FBlueprintLineInfo& Info = Rendered.Map.Lines[Index];
            if (Info.Kind != EBlueprintLineKind::LoopTransfer)
            {
                continue;
            }

            const int32 Line = Index + 1;
            const int32 JumpIndex = static_cast<int32>(Info.ScriptIndex);
            const int32 LandingIndex = static_cast<int32>(Info.TransferTarget);
            ++Transfers;

            TestTrue(*FString::Printf(TEXT("%s: line %d is a break/continue line"), Name, Line), Lines.IsValidIndex(Index) && (Lines[Index].TrimStartAndEnd() == TEXT("break;") || Lines[Index].TrimStartAndEnd() == TEXT("continue;")));
            TestTrue(*FString::Printf(TEXT("%s: line %d is breakpointable"), Name, Line), Info.IsBreakpointable());
            TestTrue(*FString::Printf(TEXT("%s: line %d jumps inside the script"), Name, Line), Function->Script.IsValidIndex(JumpIndex));
            TestTrue(*FString::Printf(TEXT("%s: line %d lands inside the script"), Name, Line), Function->Script.IsValidIndex(LandingIndex));

            if (Function->Script.IsValidIndex(JumpIndex))
            {
                const int32 Token = static_cast<int32>(Function->Script[JumpIndex]);
                TestTrue(*FString::Printf(TEXT("%s: line %d points at a jump opcode"), Name, Line), Token == static_cast<int32>(EX_Jump) || Token == static_cast<int32>(EX_JumpIfNot));
            }

            // The transfer owns no address. A conditional arm's jump opcode is also the `if` line's address,
            // so the reverse lookup must keep reporting that statement and never the transfer itself.
            const int32 ReverseLine = Rendered.Map.FindLine(Function, Info.ScriptIndex);
            TestTrue(*FString::Printf(TEXT("%s: line %d owns no reverse entry"), Name, Line), ReverseLine == 0 || ReverseLine - 1 != Index);
            TestEqual(*FString::Printf(TEXT("%s: a request on line %d snaps to itself"), Name, Line), FindBreakpointLine(Rendered.Map.Lines, Line), Line);

            AddInfo(FString::Printf(TEXT("%s: line %d dispatches %d and lands on %d"), Name, Line, JumpIndex, LandingIndex));
        }

        TestTrue(FString::Printf(TEXT("%s: the render has break/continue transfer lines"), Name), Transfers > 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorUnhandledOpcodeTest, "BlueprintInspector.Disassembler.UnhandledOpcodeStopsTheWalk", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorUnhandledOpcodeTest::RunTest(const FString& Parameters)
{
    // 0x7F is not a declared EExprToken (see UObject/Script.h; 0x73-0xFE are unassigned), so ProcessOpcode
    // falls to its default case, which logs at Error and sets the reader error. Declare that log: the opcode
    // logger's own "not supported" line is a Warning, which does not fail a test by default.
    BlueprintInspectorTests::AddExpectedPlainError(*this, TEXT("Unhandled opcode 0x7F at script offset 0"), 1);

    TArray<uint8> Script;
    BlueprintInspectorTests::AppendOpcode(Script, static_cast<EExprToken>(0x7F));
    BlueprintInspectorTests::AppendOpcode(Script, EX_Return);
    BlueprintInspectorTests::AppendOpcode(Script, EX_Nothing);
    BlueprintInspectorTests::AppendOpcode(Script, EX_EndOfScript);

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestUnhandledOpcode"), MoveTemp(Script));

    const FString Rendered = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Rendered);

    TestTrue(TEXT("renders the function header"), Rendered.Contains(TEXT("BlueprintInspectorTestUnhandledOpcode")));
    TestFalse(TEXT("the walk stops at the unknown opcode"), Rendered.Contains(TEXT("return")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorTruncatedOperandTest, "BlueprintInspector.Disassembler.TruncatedOperandDoesNotCrash", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorTruncatedOperandTest::RunTest(const FString& Parameters)
{
    // EX_IntConst promises a 4-byte operand that is not there. The reader logs the bounds failure at Error, and
    // an undeclared Error fails an automation test, so declare it. That also pins that this path logs exactly
    // once per render. The operand defaults to zero, so the render is reproducible.
    BlueprintInspectorTests::AddExpectedPlainError(*this, TEXT("FBlueprintScriptArchiveReader out of bounds"), 2);

    TArray<uint8> Script;
    BlueprintInspectorTests::AppendOpcode(Script, EX_Return);
    BlueprintInspectorTests::AppendOpcode(Script, EX_IntConst);

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestTruncatedOperand"), MoveTemp(Script));

    const FString Rendered = BlueprintInspectorTests::RenderFunction(Function);
    const FString SecondPass = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Rendered);

    TestTrue(TEXT("terminates and still renders the function header"), Rendered.Contains(TEXT("BlueprintInspectorTestTruncatedOperand")));
    TestTrue(TEXT("the truncated operand renders a deterministic zero"), Rendered.Contains(TEXT("return 0;")));
    TestEqual(TEXT("the truncated render is reproducible"), Rendered, SecondPass);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorDeterminismTest, "BlueprintInspector.Disassembler.RenderingIsDeterministic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorDeterminismTest::RunTest(const FString& Parameters)
{
    const UFunction* const First = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestDeterminismA"), BlueprintInspectorTests::MakeReturnValueScript(41));
    const UFunction* const Other = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestDeterminismB"), BlueprintInspectorTests::MakeReturnValueScript(42));

    // Rendering is stateful in two places: the visualizer's static indent pointer and its name caches.
    // The same function must render identically either side of an unrelated render.
    const FString FirstPass = BlueprintInspectorTests::RenderFunction(First);
    const FString OtherPass = BlueprintInspectorTests::RenderFunction(Other);
    const FString SecondPass = BlueprintInspectorTests::RenderFunction(First);
    AddInfo(FirstPass);

    TestEqual(TEXT("the same function renders identically on a second pass"), *FirstPass, *SecondPass);
    TestTrue(TEXT("a second function renders its own value"), OtherPass.Contains(TEXT("return 42;")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorConvergentFlowTest, "BlueprintInspector.Disassembler.ConvergentFlowRendersOnce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorConvergentFlowTest::RunTest(const FString& Parameters)
{
    uint32 ReturnValueAddress = 0;
    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestConvergentReturn"), BlueprintInspectorTests::MakeConvergingReturnScript(&ReturnValueAddress));

    const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
    AddInfo(Rendered.Text);

    TArray<FString> Lines;
    Rendered.Text.ParseIntoArrayLines(Lines, false);

    int32 ReturnLines = 0;
    int32 ReturnValueAddressLines = 0;
    int32 ReturnOpcodeAddressLines = 0;
    for (int32 Index = 0; Index < Rendered.Map.Lines.Num() && Index < Lines.Num(); ++Index)
    {
        const FString LineText = Lines[Index].TrimStartAndEnd();
        const FBlueprintLineInfo& Info = Rendered.Map.Lines[Index];

        if (LineText == TEXT("return;"))
        {
            ++ReturnLines;
        }

        // The return statement is mapped to the value token that follows EX_Return; the EX_Return byte
        // itself is never dispatched, so no line may claim it.
        if (Info.Kind == EBlueprintLineKind::Statement && Info.ScriptIndex == static_cast<int64>(ReturnValueAddress))
        {
            ++ReturnValueAddressLines;
        }
        if (Info.ScriptIndex == static_cast<int64>(ReturnValueAddress) - 1)
        {
            ++ReturnOpcodeAddressLines;
        }
    }

    TestEqual(TEXT("the branch still renders"), Rendered.Text.Contains(TEXT("if (")), true);
    TestFalse(TEXT("the second edge is not a goto"), Rendered.Text.Contains(TEXT("goto")));
    // This shape folds the second edge into the branch, so the shared return is a jump target rather
    // than a rendered line: the guard is "never duplicated", not "always rendered".
    TestTrue(TEXT("the shared return is rendered at most once"), ReturnLines <= 1);
    TestTrue(TEXT("at most one statement owns the return's value token"), ReturnValueAddressLines <= 1);
    TestEqual(TEXT("no statement owns the EX_Return byte"), ReturnOpcodeAddressLines, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorWhileLoopTest, "BlueprintInspector.Disassembler.WhileLoopRendersAsLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorWhileLoopTest::RunTest(const FString& Parameters)
{
    const UFunction* const Callee = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetMathLibrary:Abs_Int"));
    if (Callee == nullptr)
    {
        AddError(TEXT("KismetMathLibrary.Abs_Int was not found; the loop body cannot be built."));
        return false;
    }

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestWhileLoop"), BlueprintInspectorTests::MakeWhileLoopScript(Callee));

    const FString Script = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Script);

    TestTrue(TEXT("the header branch renders as a while loop"), Script.Contains(TEXT("while (")));
    TestTrue(TEXT("the loop body renders"), Script.Contains(TEXT("Abs_Int(1);")));
    TestFalse(TEXT("the back-edge is not a goto"), Script.Contains(TEXT("goto")));
    TestFalse(TEXT("the loop exit is not duplicated"), Script.Contains(TEXT("return;")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorDoWhileTest, "BlueprintInspector.Disassembler.DoWhileRendersAsLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorDoWhileTest::RunTest(const FString& Parameters)
{
    const UFunction* const Callee = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetMathLibrary:Abs_Int"));
    if (Callee == nullptr)
    {
        AddError(TEXT("KismetMathLibrary.Abs_Int was not found; the loop body cannot be built."));
        return false;
    }

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestDoWhile"), BlueprintInspectorTests::MakeDoWhileScript(Callee));

    const FString Script = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Script);

    TestTrue(TEXT("the body opens a do block"), Script.Contains(TEXT("do\n")) || Script.Contains(TEXT("do {")));
    TestTrue(TEXT("the tail branch closes the loop"), Script.Contains(TEXT("while (1);")));
    TestTrue(TEXT("the body renders once"), Script.Contains(TEXT("Abs_Int(1);")));
    TestFalse(TEXT("the back-edge is not a goto"), Script.Contains(TEXT("goto")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorBreakTest, "BlueprintInspector.Disassembler.BreakRendersInsideLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorBreakTest::RunTest(const FString& Parameters)
{
    const UFunction* const Callee = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetMathLibrary:Abs_Int"));
    if (Callee == nullptr)
    {
        AddError(TEXT("KismetMathLibrary.Abs_Int was not found; the loop body cannot be built."));
        return false;
    }

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestBreakLoop"), BlueprintInspectorTests::MakeBreakLoopScript(Callee));

    const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
    AddInfo(Rendered.Text);

    TestTrue(TEXT("the loop renders"), Rendered.Text.Contains(TEXT("while (")));
    TestTrue(TEXT("the exit edge renders as a break"), Rendered.Text.Contains(TEXT("break;")));
    TestFalse(TEXT("the exit path is not walked inside the loop"), Rendered.Text.Contains(TEXT("goto")));

    // This break is the false arm of a JumpIfNot, so the recorded offset has to be the branch itself and not
    // the last opcode of the condition the branch decodes: a wrong offset binds a breakpoint that never fires.
    int32 BreakLine = INDEX_NONE;
    for (int32 Index = 0; Index < Rendered.Map.Lines.Num(); ++Index)
    {
        if (Rendered.Map.Lines[Index].Kind == EBlueprintLineKind::LoopTransfer)
        {
            BreakLine = Index;
            break;
        }
    }

    if (TestTrue(TEXT("the break line is a transfer line"), BreakLine != INDEX_NONE))
    {
        const FBlueprintLineInfo& Info = Rendered.Map.Lines[BreakLine];
        const int32 JumpIndex = static_cast<int32>(Info.ScriptIndex);
        TestTrue(TEXT("the break jump is inside the script"), Function->Script.IsValidIndex(JumpIndex));
        if (Function->Script.IsValidIndex(JumpIndex))
        {
            TestEqual(TEXT("the break arms the conditional jump itself"), static_cast<int32>(Function->Script[JumpIndex]), static_cast<int32>(EX_JumpIfNot));
        }
        TestTrue(TEXT("the break landing is inside the script"), Info.TransferTarget >= 0 && Function->Script.IsValidIndex(static_cast<int32>(Info.TransferTarget)));
        AddInfo(FString::Printf(TEXT("break line %d dispatches %d and lands on %lld"), BreakLine + 1, JumpIndex, Info.TransferTarget));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorContinueTest, "BlueprintInspector.Disassembler.ContinueRendersInsideLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorContinueTest::RunTest(const FString& Parameters)
{
    const UFunction* const Callee = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetMathLibrary:Abs_Int"));
    if (Callee == nullptr)
    {
        AddError(TEXT("KismetMathLibrary.Abs_Int was not found; the loop body cannot be built."));
        return false;
    }

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestContinueLoop"), BlueprintInspectorTests::MakeContinueLoopScript(Callee));

    const FString Script = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Script);

    TestTrue(TEXT("the loop renders"), Script.Contains(TEXT("while (")));
    TestTrue(TEXT("the back edge renders as a continue"), Script.Contains(TEXT("continue;")));
    TestFalse(TEXT("the continue is not a goto"), Script.Contains(TEXT("goto")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorRepeatedSequencePinTest, "BlueprintInspector.Disassembler.SequencePinRendersOnce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorRepeatedSequencePinTest::RunTest(const FString& Parameters)
{
    const UFunction* const Callee = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetMathLibrary:Abs_Int"));
    if (Callee == nullptr)
    {
        AddError(TEXT("KismetMathLibrary.Abs_Int was not found; the sequence body cannot be built."));
        return false;
    }

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestRepeatedSequencePin"), BlueprintInspectorTests::MakeRepeatedSequencePinScript(Callee));

    const FString Script = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Script);

    TestTrue(TEXT("the shared pin statement renders"), Script.Contains(TEXT("Abs_Int(1);")));
    TestFalse(TEXT("a repeated sequence pin is not a goto"), Script.Contains(TEXT("goto")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorNodePoolTest, "BlueprintInspector.Disassembler.NodePoolOverflowStops", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorNodePoolTest::RunTest(const FString& Parameters)
{
    const UFunction* const Callee = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetMathLibrary:Abs_Int"));
    if (Callee == nullptr)
    {
        AddError(TEXT("KismetMathLibrary.Abs_Int was not found; the overflow chain cannot be built."));
        return false;
    }

    BlueprintInspectorTests::AddExpectedPlainError(*this, TEXT("Node pool exhausted"), 1);

    TArray<uint8> Script;
    for (int32 Index = 0; Index < 1100; ++Index)
    {
        BlueprintInspectorTests::AppendOpcode(Script, EX_CallMath);
        BlueprintInspectorTests::AppendPointer(Script, Callee);
        BlueprintInspectorTests::AppendOpcode(Script, EX_IntConst);
        BlueprintInspectorTests::AppendInt32(Script, 1);
        BlueprintInspectorTests::AppendOpcode(Script, EX_EndFunctionParms);
    }
    BlueprintInspectorTests::AppendOpcode(Script, EX_Return);
    BlueprintInspectorTests::AppendOpcode(Script, EX_Nothing);
    BlueprintInspectorTests::AppendOpcode(Script, EX_EndOfScript);

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestNodePoolOverflow"), MoveTemp(Script));
    const FString Rendered = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(FString::Printf(TEXT("overflow render length: %d"), Rendered.Len()));

    TestTrue(TEXT("the walk terminates and renders"), !Rendered.IsEmpty());
    TestTrue(TEXT("the render still contains the call"), Rendered.Contains(TEXT("Abs_Int(1);")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureTest, "BlueprintInspector.Fixture.FixtureBlueprintDisassembles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureTest::RunTest(const FString& Parameters)
{
    const TArray<UFunction*> FixtureFunctions = BlueprintFixtureSource::GetAllFunctions();
    if (FixtureFunctions.Num() == 0)
    {
        AddError(TEXT("The fixture functions are missing."));
        return false;
    }

    int32 NumFunctions = 0;
    int32 FunctionsWithStatements = 0;
    TArray<FString> FunctionNames;

    for (const UFunction* const Function : FixtureFunctions)
    {
        if (!Function || Function->Script.Num() < 8 || Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            continue;
        }

        ++NumFunctions;
        FunctionNames.Add(Function->GetName());

        const FString Script = BlueprintInspectorTests::RenderFunction(Function);
        const FString SecondPass = BlueprintInspectorTests::RenderFunction(Function);

        TestFalse(FString::Printf(TEXT("%s: failed to produce output"), *Function->GetName()), Script.IsEmpty());
        TestEqual(FString::Printf(TEXT("%s: disassembly is deterministic"), *Function->GetName()), *Script, *SecondPass);

        FunctionsWithStatements += Script.Contains(TEXT(";")) ? 1 : 0;
    }

    AddInfo(FString::Printf(TEXT("Disassembled %d function(s) from the fixture blueprint; %d rendered at least one statement."), NumFunctions, FunctionsWithStatements));
    AddInfo(FString::Printf(TEXT("Fixture functions: %s"), *FString::Join(FunctionNames, TEXT(", "))));

    if (NumFunctions == 0)
    {
        AddError(TEXT("The fixture blueprint has no bytecode-bearing non-ubergraph function."));
        return false;
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureSingleRenderTest, "BlueprintInspector.Fixture.StatementsRenderOnce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureSingleRenderTest::RunTest(const FString& Parameters)
{
    const TArray<UFunction*> FixtureFunctions = BlueprintFixtureSource::GetAllFunctions();
    if (FixtureFunctions.Num() == 0)
    {
        AddError(TEXT("The fixture functions are missing."));
        return false;
    }

    int32 CheckedFunctions = 0;
    int32 CheckedStatements = 0;

    for (const UFunction* const Function : FixtureFunctions)
    {
        if (!Function || Function->Script.Num() < 8 || Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            continue;
        }

        ++CheckedFunctions;

        FBlueprintScriptDisassembler Disassembler;
        Disassembler.FunctionToNodes(Function);

        FBlueprintRenderedSource Rendered;
        FBlueprintScriptVisualizer_Pseudo Visualizer;
        Visualizer.Visualize(Function, Disassembler, Rendered.Text, &Rendered.Map);

        TestFalse(FString::Printf(TEXT("%s: render contains a goto"), *Function->GetName()), Rendered.Text.Contains(TEXT("goto")));

        TSet<int64> Addresses;
        for (int32 Index = 0; Index < Rendered.Map.Lines.Num(); ++Index)
        {
            const FBlueprintLineInfo& Info = Rendered.Map.Lines[Index];
            if (Info.Kind != EBlueprintLineKind::Statement || !Info.IsBreakpointable())
            {
                continue;
            }

            ++CheckedStatements;
            TestFalse(FString::Printf(TEXT("%s: statement at %lld renders twice (line %d)"), *Function->GetName(), Info.ScriptIndex, Index + 1), Addresses.Contains(Info.ScriptIndex));
            Addresses.Add(Info.ScriptIndex);
        }

        TSet<int64> GraphAddresses;
        TSet<const FBlueprintNode*> Visited;
        const TSet<const FBlueprintNode*>& FoldedBranches = Visualizer.GetFoldedBranches();
        BlueprintInspectorTests::CollectGraphAddresses(&Disassembler.GetEntryPointNode(), GraphAddresses, Visited, &FoldedBranches);
        for (const int64 GraphAddress : GraphAddresses)
        {
            TestTrue(FString::Printf(TEXT("%s: statement at %lld is rendered"), *Function->GetName(), GraphAddress), Addresses.Contains(GraphAddress));
        }
    }

    TestTrue(TEXT("at least one function was checked"), CheckedFunctions > 0);
    TestTrue(TEXT("at least one statement was checked"), CheckedStatements > 0);
    AddInfo(FString::Printf(TEXT("Checked %d statement(s) across %d function(s) for duplicate rendering."), CheckedStatements, CheckedFunctions));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureShapesTest, "BlueprintInspector.Fixture.KnownShapesRender", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureShapesTest::RunTest(const FString& Parameters)
{
    const auto Expect = [this](const TCHAR* Name, const TArray<FString>& Contains, const TArray<FString>& Absent) {
#if !BI_ENGINE_AT_LEAST_5
        // 4.25 cannot import the archive's struct literals, so the three struct rows render with
        // default member values. Their token plans still match; only the literal fragments differ.
        if (FCString::Strcmp(Name, TEXT("const_struct")) == 0 || FCString::Strcmp(Name, TEXT("let_struct")) == 0 || FCString::Strcmp(Name, TEXT("structconst_containers")) == 0)
        {
            AddInfo(FString::Printf(TEXT("%s: struct literals are skipped on 4.25; literal fragment checks skipped."), Name));
            return;
        }
#endif

        const UFunction* const Function = BlueprintFixtureSource::FindFunction(FName(Name));
        if (Function == nullptr)
        {
            const FString MissingId = FString::Printf(TEXT("F_%s"), Name);
            if (GetBlueprintKnownMissingFixtures().Contains(MissingId))
            {
                AddInfo(FString::Printf(TEXT("%s: known-missing on this engine; skipped."), Name));
                return;
            }
            AddError(FString::Printf(TEXT("%s: fixture function is missing"), Name));
            return;
        }

        const FString Text = BlueprintInspectorTests::RenderFunction(Function);
        for (const FString& Fragment : Contains)
        {
            TestTrue(FString::Printf(TEXT("%s should contain '%s'"), Name, *Fragment), Text.Contains(Fragment));
        }
        for (const FString& Fragment : Absent)
        {
            TestFalse(FString::Printf(TEXT("%s should not contain '%s'"), Name, *Fragment), Text.Contains(Fragment));
        }
    };

    Expect(TEXT("cast_objecttointerface"),
           {TEXT("Cast<IBPI_ProbeA_C>(ActorVar)"), TEXT("if (K2Node_DynamicCast_bSuccess)"), TEXT("InterfaceVar = ")},
           {TEXT("else"), TEXT("goto"), TEXT("return;")});
    Expect(TEXT("cast_pure"),
           {TEXT("Cast<ABP_ProbeCastTarget_C>(ActorVar)"), TEXT("if (K2Node_DynamicCast_bSuccess)")},
           {TEXT("else"), TEXT("goto")});
    Expect(TEXT("cast_metacast"),
           {TEXT("Cast<ABP_ProbeCastTarget_C>(ClassVar)")},
           {TEXT("goto")});

    Expect(TEXT("flow_branch"), {TEXT("if (!BoolVar)"), TEXT("else"), TEXT("IntVar = 0;")}, {TEXT("goto")});
    Expect(TEXT("flow_while"), {TEXT("while (UKismetMathLibrary::Less_IntInt(IntVar, 3))"), TEXT("Add_IntInt(IntVar, 1)"), TEXT("IntVar = CallFunc_Add_IntInt_ReturnValue;")}, {TEXT("goto")});
    Expect(TEXT("flow_do_while"), {TEXT("do"), TEXT("while (BoolVar);")}, {TEXT("goto")});
    Expect(TEXT("flow_break"), {TEXT("while (BoolVar)"), TEXT("break;")}, {TEXT("goto")});
    Expect(TEXT("flow_continue"), {TEXT("while (BoolVar)"), TEXT("continue;")}, {TEXT("goto")});
    Expect(TEXT("flow_sequence"), {TEXT("// Sequence 0."), TEXT("// Sequence 1."), TEXT("// Sequence 2.")}, {TEXT("goto")});
    Expect(TEXT("flow_switch"), {TEXT("NotEqual_IntInt(IntVar, 0)"), TEXT("if (!K2Node_SwitchInteger_CmpSuccess)"), TEXT("RealVar = 0.0000;"), TEXT("NotEqual_IntInt(IntVar, 1)"), TEXT("RealVar = 2.0000;")}, {TEXT("goto")});
    Expect(TEXT("flow_nested_loops"), {TEXT("while (LoopFlag)"), TEXT("while (BoolVar)"), TEXT("break;"), TEXT("continue;")}, {TEXT("goto")});
    Expect(TEXT("flow_switchvalue"),
           {TEXT("switch (Temp_int_Variable)"), TEXT("case 0:"), TEXT("case 1:"), TEXT("default:"), TEXT("[&Temp_int_Variable]()")},
           {TEXT("goto")});
    Expect(TEXT("flow_return_value"), {TEXT("return ReturnValue;")}, {TEXT("goto")});

    Expect(TEXT("delegate_callmulticast"), {TEXT("OnProbeMulticast.Broadcast(7);")}, {});
    Expect(TEXT("delegate_addelegate"), {TEXT("OnProbeMulticast.Add(")}, {});
    Expect(TEXT("delegate_removedelegate"), {TEXT("OnProbeMulticast.Remove(")}, {});
    Expect(TEXT("delegate_cleardelegate"), {TEXT("OnProbeMulticast.Clear();")}, {});

    Expect(TEXT("setarray_makearray"), {TEXT("TArray<int32>{1, 2}")}, {});
    Expect(TEXT("setset_makeset"), {TEXT("TSet<FName>{\"Alpha\", \"Beta\"}")}, {});
    Expect(TEXT("setmap_makemap"), {TEXT("TMap<int32, FName>{{1, \"Alpha\"}, {2, \"Beta\"}}")}, {});
    Expect(TEXT("structconst_containers"),
           {TEXT("TArray<int32>{1, 2, 3}"), TEXT("TSet<FName>{\"Alpha\", \"Beta\"}"), TEXT("TMap<int32, FName>{{1, \"Alpha\"}, {2, \"Beta\"}}")},
           {});

    Expect(TEXT("let_int"), {TEXT("IntVar = 7;")}, {});
    Expect(TEXT("let_vector"), {TEXT("FVector(1.0000, 2.0000, 3.0000)")}, {});
    Expect(TEXT("let_struct"), {TEXT("S_ProbeStruct(false, 1, 2.0000, \"probe\", nullptr, S_ProbeNested(false, true))")}, {});
    Expect(TEXT("const_string_ascii"), {TEXT("Echo_String(\"probe\", this);")}, {});
    Expect(TEXT("const_text_localized"), {TEXT("Echo_Text(\"Probe text\", this);")}, {});
    Expect(TEXT("const_softobject"), {TEXT("SoftRef(\"/Temp/BlueprintInspectorFixture/M_Probe.M_Probe\", this)")}, {});
    Expect(TEXT("const_transform"), {TEXT("S=(1.0000, 1.0000, 1.0000)")}, {});
    Expect(TEXT("context_getactorlocation"), {TEXT("ActorVar->K2_GetActorLocation()")}, {});
    Expect(TEXT("classcontext_getclassdefaults"), {TEXT("IntVar = ProbeInt;")}, {});
    Expect(TEXT("interfacecontext_messagecall"), {TEXT("IProbeFunc(7)")}, {});
    Expect(TEXT("F_finalcall_parent"), {TEXT("F_ParentTarget(7);")}, {});
    Expect(TEXT("localfinal_bpfunc"), {TEXT("F_ProbeLocalFunc();")}, {});
    Expect(TEXT("none_interface"), {TEXT("EExprToken(0x2D)")}, {});
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureWellFormedTest, "BlueprintInspector.Fixture.RenderedTextIsWellFormed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureWellFormedTest::RunTest(const FString& Parameters)
{
    const TArray<UFunction*> FixtureFunctions = BlueprintFixtureSource::GetAllFunctions();
    if (FixtureFunctions.Num() == 0)
    {
        AddError(TEXT("The fixture functions are missing."));
        return false;
    }

    int32 CheckedFunctions = 0;
    for (const UFunction* const Function : FixtureFunctions)
    {
        if (!Function || Function->Script.Num() < 8 || Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            continue;
        }

        ++CheckedFunctions;
        const FString Text = BlueprintInspectorTests::RenderFunction(Function);
        TestTrue(FString::Printf(TEXT("%s: braces balance"), *Function->GetName()), BlueprintInspectorTests::BracesAreBalanced(Text));
        TestTrue(FString::Printf(TEXT("%s: ends with a closing brace"), *Function->GetName()), Text.TrimEnd().EndsWith(TEXT("}")));
        TestFalse(FString::Printf(TEXT("%s: has no carriage returns"), *Function->GetName()), Text.Contains(TEXT("\r")));
    }

    AddInfo(FString::Printf(TEXT("Checked %d rendered function(s) for balanced braces and a closed body."), CheckedFunctions));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureSequenceBoundaryTest, "BlueprintInspector.Fixture.SequencePinsAreBreakpointable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureSequenceBoundaryTest::RunTest(const FString& Parameters)
{
    const UFunction* const Function = BlueprintFixtureSource::FindFunction(FName(TEXT("flow_sequence")));
    if (Function == nullptr)
    {
        AddError(TEXT("The flow_sequence fixture function is missing."));
        return false;
    }

    const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
    TArray<FString> TextLines;
    // Keep empty lines: Map.Lines has one entry per rendered line, so culling would shift the indexes.
    Rendered.Text.ParseIntoArrayLines(TextLines, false);

    int32 Boundaries = 0;
    for (int32 Index = 0; Index < Rendered.Map.Lines.Num(); ++Index)
    {
        const FBlueprintLineInfo& Info = Rendered.Map.Lines[Index];
        if (Info.Kind != EBlueprintLineKind::Sequence)
        {
            continue;
        }

        ++Boundaries;
        const int32 Line = Index + 1;
        TestTrue(FString::Printf(TEXT("line %d is a sequence comment"), Line), TextLines.IsValidIndex(Index) && TextLines[Index].Contains(TEXT("// Sequence")));
        TestTrue(FString::Printf(TEXT("line %d is breakpointable"), Line), Rendered.Map.IsBreakpointable(Line));
        TestEqual(FString::Printf(TEXT("line %d arms itself, not a snapped line"), Line), FindBreakpointLine(Rendered.Map.Lines, Line), Line);
        TestTrue(FString::Printf(TEXT("line %d is owned by flow_sequence"), Line), Info.OwnerFunction.Get() == Function);

        const int32 ReverseLine = Rendered.Map.FindLine(Function, Info.ScriptIndex);
        TestTrue(FString::Printf(TEXT("line %d has a reverse entry"), Line), ReverseLine != 0);
        if (ReverseLine != 0)
        {
            TestEqual(FString::Printf(TEXT("line %d reverses to its address"), Line), Rendered.Map.Lines[ReverseLine - 1].ScriptIndex, Info.ScriptIndex);
        }
    }

    TestTrue(TEXT("flow_sequence renders sequence boundaries"), Boundaries >= 2);
    AddInfo(FString::Printf(TEXT("Checked %d sequence boundary line(s)."), Boundaries));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureMapCoverageTest, "BlueprintInspector.Fixture.SourceMapCoversTheText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureMapCoverageTest::RunTest(const FString& Parameters)
{
    const TArray<UFunction*> FixtureFunctions = BlueprintFixtureSource::GetAllFunctions();
    if (FixtureFunctions.Num() == 0)
    {
        AddError(TEXT("The fixture functions are missing."));
        return false;
    }

    int32 CheckedFunctions = 0;
    int32 CheckedLines = 0;
    int32 BreakpointableLines = 0;
    for (const UFunction* const Function : FixtureFunctions)
    {
        if (!Function || Function->Script.Num() < 8 || Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            continue;
        }

        ++CheckedFunctions;
        const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);

        TestEqual(FString::Printf(TEXT("%s: one map entry per rendered line"), *Function->GetName()), Rendered.Map.Lines.Num(), CountRenderedLines(Rendered.Text));

        for (int32 Index = 0; Index < Rendered.Map.Lines.Num(); ++Index)
        {
            ++CheckedLines;
            const FBlueprintLineInfo& Info = Rendered.Map.Lines[Index];
            if (!Info.IsBreakpointable())
            {
                continue;
            }

            ++BreakpointableLines;
            const UFunction* const Owner = Info.OwnerFunction.Get();
            TestTrue(FString::Printf(TEXT("%s: line %d addresses its owner's script"), *Function->GetName(), Index + 1), Owner != nullptr && Owner->Script.IsValidIndex(static_cast<int32>(Info.ScriptIndex)));

            // A break/continue line arms a dispatch pair and deliberately owns no reverse entry, so the jump
            // opcode keeps reporting the statement line that owns it.
            if (Info.Kind == EBlueprintLineKind::LoopTransfer)
            {
                TestTrue(FString::Printf(TEXT("%s: line %d arms a landing in the script"), *Function->GetName(), Index + 1), Owner != nullptr && Owner->Script.IsValidIndex(static_cast<int32>(Info.TransferTarget)));
                continue;
            }

            const int32 ReverseLine = Rendered.Map.FindLine(Owner, Info.ScriptIndex);
            TestTrue(FString::Printf(TEXT("%s: line %d has a reverse entry"), *Function->GetName(), Index + 1), ReverseLine != 0);
            if (ReverseLine != 0)
            {
                const FBlueprintLineInfo& ReverseInfo = Rendered.Map.Lines[ReverseLine - 1];
                TestTrue(FString::Printf(TEXT("%s: line %d reverses to the same address"), *Function->GetName(), Index + 1), ReverseInfo.OwnerFunction.Get() == Owner && ReverseInfo.ScriptIndex == Info.ScriptIndex);
                if (Info.Kind == EBlueprintLineKind::SubStatement)
                {
                    TestTrue(FString::Printf(TEXT("%s: line %d reverses to its Statement"), *Function->GetName(), Index + 1), ReverseInfo.Kind == EBlueprintLineKind::Statement);
                }
            }
        }
    }

    TestTrue(TEXT("at least one fixture function was checked"), CheckedFunctions > 0);
    TestTrue(TEXT("at least one rendered line was checked"), CheckedLines > 0);
    TestTrue(TEXT("at least one line is breakpointable"), BreakpointableLines > 0);
    AddInfo(FString::Printf(TEXT("Checked %d line(s) across %d function(s); %d are breakpointable."), CheckedLines, CheckedFunctions, BreakpointableLines));
    return true;
}

// An enum value renders as the name the Blueprint graph shows for it. That is the enumerant's authored
// display name, not the raw enumerant name and not the localized display text. The renderer reads the
// display text's source string, so the text cannot follow the editor culture. The fixture authors
// display names that differ from its enumerant names, so this test can tell the two apart.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorEnumRenderTest, "BlueprintInspector.Rendering.EnumValueUsesTheAuthoredName", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorEnumRenderTest::RunTest(const FString& Parameters)
{
    const UEnum* const Enum = LoadObject<UEnum>(nullptr, TEXT("/BlueprintInspector/Tests/OpcodeShowcase/E_OpcodeDemoEnum.E_OpcodeDemoEnum"));
    if (!TestNotNull(TEXT("the fixture enum loads"), Enum))
    {
        return false;
    }

    if (!TestTrue(TEXT("the fixture enum has the enumerant under test"), Enum->NumEnums() > 1))
    {
        return false;
    }

    const FString EnumerantName = Enum->GetNameStringByIndex(1);
    const FText AuthoredText = Enum->GetDisplayNameTextByIndex(1);
    const FString* const AuthoredSource = FTextInspector::GetSourceString(AuthoredText);
    const FString AuthoredName = (AuthoredSource != nullptr && !AuthoredSource->IsEmpty()) ? *AuthoredSource : Enum->GetNameStringByIndex(1);
    TestNotEqual(TEXT("the fixture enum's authored name differs from its enumerant name"), AuthoredName, EnumerantName);

    const UFunction* const Function = BlueprintFixtureSource::FindFunction(TEXT("let_enum"));
    if (!TestNotNull(TEXT("the fixture has let_enum"), Function))
    {
        return false;
    }

    const FString Rendered = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Rendered);

    const FString ExpectedAssignment = FString::Printf(TEXT("= %s;"), *FBlueprintScriptVisualizer_Pseudo::SanitiseIdentifier(AuthoredName));
    TestTrue(FString::Printf(TEXT("let_enum names the value as the graph does (%s)"), *ExpectedAssignment), Rendered.Contains(ExpectedAssignment));
    TestFalse(FString::Printf(TEXT("let_enum does not print the raw enumerant name (%s)"), *EnumerantName), Rendered.Contains(FString::Printf(TEXT("= %s;"), *EnumerantName)));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorGoldenPseudoSourceTest, "BlueprintInspector.Fixture.PseudoSourceMatchesGolden", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorGoldenPseudoSourceTest::RunTest(const FString& Parameters)
{
    const TArray<UFunction*> FixtureFunctions = BlueprintFixtureSource::GetAllFunctions();
    if (FixtureFunctions.Num() == 0)
    {
        AddError(TEXT("The fixture functions are missing."));
        return false;
    }

    TArray<TPair<FString, FString>> TextEntries;
    TArray<TPair<FString, FString>> MapEntries;
    for (const UFunction* const Function : FixtureFunctions)
    {
        if (!Function || Function->Script.Num() < 8 || Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            continue;
        }

        const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
        TextEntries.Emplace(Function->GetName(), Rendered.Text);
        MapEntries.Emplace(Function->GetName(), BlueprintInspectorTests::FormatSourceMap(Rendered.Map));
    }

    const auto SortByKey = [](const TPair<FString, FString>& Left, const TPair<FString, FString>& Right) {
        return Left.Key < Right.Key;
    };
    TextEntries.Sort(SortByKey);
    MapEntries.Sort(SortByKey);

    if (!TestTrue(TEXT("at least one fixture function was rendered into the golden"), TextEntries.Num() > 0 && MapEntries.Num() > 0))
    {
        return false;
    }

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BlueprintInspector"));
    const FString GoldenDir = Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/BlueprintInspector/Private/Tests/Golden"))) : FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("BlueprintInspector/Source/BlueprintInspector/Private/Tests/Golden")));
    const bool bUpdate = FParse::Param(FCommandLine::Get(), TEXT("BlueprintGoldenUpdate"));
    const bool bWriteVariant = FParse::Param(FCommandLine::Get(), TEXT("BlueprintGoldenVariant"));

    BlueprintInspectorTests::CheckGoldenEntries(*this, TEXT("pseudo-source"), TEXT("pseudo-source text"), TEXT("Each section is \"===== Function =====\" followed by that function's exact render."), GoldenDir, TEXT("PseudoSource"), TextEntries, bUpdate, bWriteVariant);
    BlueprintInspectorTests::CheckGoldenEntries(*this, TEXT("source-map"), TEXT("pseudo-source line map"), TEXT("Each section lists \"line|kind[|Function@Address]\" for one rendered function."), GoldenDir, TEXT("PseudoSource.map"), MapEntries, bUpdate, bWriteVariant);
    return true;
}

namespace BlueprintInspectorTests
{
void SplitCsvLine(const FString& Line, TArray<FString>& OutFields)
{
    OutFields.Reset();
    FString Field;
    bool bInQuotes = false;

    for (int32 Index = 0; Index < Line.Len(); ++Index)
    {
        const TCHAR Character = Line[Index];
        if (bInQuotes)
        {
            if (Character == TEXT('"'))
            {
                if (Index + 1 < Line.Len() && Line[Index + 1] == TEXT('"'))
                {
                    Field.AppendChar(TEXT('"'));
                    ++Index;
                }
                else
                {
                    bInQuotes = false;
                }
            }
            else
            {
                Field.AppendChar(Character);
            }
        }
        else if (Character == TEXT('"'))
        {
            bInQuotes = true;
        }
        else if (Character == TEXT(','))
        {
            OutFields.Add(Field);
            Field.Reset();
        }
        else
        {
            Field.AppendChar(Character);
        }
    }

    OutFields.Add(Field);
}

/** The guide ships inside the plugin, so a consumer project needs no copy next to its own docs. */
FString FixturePlanPath()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BlueprintInspector"));
    const FString BaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("BlueprintInspector"));
    return FPaths::Combine(BaseDir, TEXT("Data"), TEXT("BlueprintLayoutGuide.md"));
}

bool LoadFixtureRows(TArray<TArray<FString>>& OutRows)
{
    const FString Path = FixturePlanPath();
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        return false;
    }

    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);

    int32 HeaderIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        // The base plan's header, not the per-engine override table's "fixture_id,engine,opcodes".
        if (Lines[Index].StartsWith(TEXT("fixture_id,family,")))
        {
            HeaderIndex = Index;
            break;
        }
    }
    if (HeaderIndex == INDEX_NONE)
    {
        return false;
    }

    for (int32 Index = HeaderIndex + 1; Index < Lines.Num(); ++Index)
    {
        const FString& Line = Lines[Index];
        if (Line.StartsWith(TEXT("```")))
        {
            break;
        }
        if (Line.IsEmpty())
        {
            continue;
        }

        TArray<FString> Fields;
        SplitCsvLine(Line, Fields);
        if (Fields.Num() >= 11)
        {
            OutRows.Add(MoveTemp(Fields));
        }
    }

    return OutRows.Num() > 0;
}

/**
 * A per-engine correction to the fixture plan. The plan's own `opcodes` field describes the reference
 * engine (UE 5.8); when another engine's compiler emits a different sequence, the row is restated in
 * the override table instead of weakening the shared plan.
 */
struct FFixturePlanOverride
{
    FString FixtureId;
    FString Engine;
    TArray<FString> Opcodes;
};

bool LoadFixturePlanOverrides(TArray<FFixturePlanOverride>& OutOverrides)
{
    const FString Path = FixturePlanPath();
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        return false;
    }

    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);

    int32 HeaderIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        if (Lines[Index].StartsWith(TEXT("fixture_id,engine,opcodes")))
        {
            HeaderIndex = Index;
            break;
        }
    }
    if (HeaderIndex == INDEX_NONE)
    {
        return false;
    }

    for (int32 Index = HeaderIndex + 1; Index < Lines.Num(); ++Index)
    {
        const FString& Line = Lines[Index];
        if (Line.StartsWith(TEXT("```")))
        {
            break;
        }
        if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
        {
            continue;
        }

        TArray<FString> Fields;
        SplitCsvLine(Line, Fields);
        if (Fields.Num() < 3)
        {
            continue;
        }

        FFixturePlanOverride Override;
        Override.FixtureId = Fields[0].TrimStartAndEnd();
        Override.Engine = Fields[1].TrimStartAndEnd();
        Fields[2].TrimStartAndEnd().ParseIntoArray(Override.Opcodes, TEXT(";"), true);
        for (FString& Token : Override.Opcodes)
        {
            Token.TrimStartAndEndInline();
        }

        if (!Override.FixtureId.IsEmpty() && Override.Opcodes.Num() > 0)
        {
            OutOverrides.Add(MoveTemp(Override));
        }
    }

    return true;
}

// The last override for a fixture and engine pair wins, so a later row can correct an earlier one.
FString ResolveFixtureOpcodes(const TArray<FFixturePlanOverride>& Overrides, const FString& FixtureId, const FString& Engine, const FString& Base)
{
    const FFixturePlanOverride* Match = nullptr;
    for (const FFixturePlanOverride& Override : Overrides)
    {
        if (Override.FixtureId.Equals(FixtureId, ESearchCase::CaseSensitive) && Override.Engine.Equals(Engine, ESearchCase::CaseSensitive))
        {
            Match = &Override;
        }
    }

    return Match != nullptr ? FString::Join(Match->Opcodes, TEXT(";")) : Base;
}
} // namespace BlueprintInspectorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorOpcodeSinkTest, "BlueprintInspector.Disassembler.OpcodeSinkCaptures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorOpcodeSinkTest::RunTest(const FString& Parameters)
{
    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestOpcodeSink"), BlueprintInspectorTests::MakeReturnValueScript(7));

    const TArray<EExprToken> Tokens = BlueprintInspectorTests::CaptureOpcodes(Function);
    TestTrue(TEXT("captures EX_Return"), Tokens.Contains(EX_Return));
    TestTrue(TEXT("captures EX_IntConst"), Tokens.Contains(EX_IntConst));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorFixtureTokenTest, "BlueprintInspector.Fixture.TokenSequencesMatchThePlan", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorFixtureTokenTest::RunTest(const FString& Parameters)
{
    if (!CanSuppressAccessViolation())
    {
        AddError(FString::Printf(TEXT("Kismet.bCanSuppressAccessViolation is false; pass %s. The fixture's EX_Context_FailSilent rows cannot match without it."), FailSilentCommandLineSwitch()));
        return false;
    }

    UBlueprint* const Fixture = BlueprintFixtureSource::GetMain();
    const UClass* const GeneratedClass = Fixture ? Fixture->GeneratedClass.Get() : nullptr;
    if (!GeneratedClass)
    {
        AddError(TEXT("The transient main fixture is missing or has no generated class."));
        return false;
    }

    TArray<TArray<FString>> Rows;
    if (!BlueprintInspectorTests::LoadFixtureRows(Rows))
    {
        AddError(TEXT("Could not read the fixture CSV from Data/BlueprintLayoutGuide.md."));
        return false;
    }

    TArray<BlueprintInspectorTests::FFixturePlanOverride> Overrides;
    if (!BlueprintInspectorTests::LoadFixturePlanOverrides(Overrides))
    {
        AddError(TEXT("Could not read the per-engine override table from Data/BlueprintLayoutGuide.md."));
        return false;
    }
    const FString EngineTag = GetBlueprintEngineTag();
    int32 OverriddenRows = 0;

    TMap<FName, UFunction*> Functions;
    for (UFunction* const Function : BlueprintFixtureSource::GetAllFunctions())
    {
        if (Function != nullptr)
        {
            Functions.Add(Function->GetFName(), Function);
        }
    }

    UFunction* Ubergraph = nullptr;
    for (TFieldIterator<UFunction> It(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (It->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            Ubergraph = *It;
            break;
        }
    }

    int32 Checked = 0;
    int32 Mismatched = 0;
    int32 MissingFunction = 0;
    TArray<FString> MissingFunctions;

    for (const TArray<FString>& Fields : Rows)
    {
        const FString RowId = Fields[0].TrimStartAndEnd();
        const FString Graph = Fields[4].TrimStartAndEnd();
        const FString BaseExpected = Fields[10].TrimStartAndEnd();
        const FString Expected = BlueprintInspectorTests::ResolveFixtureOpcodes(Overrides, RowId, EngineTag, BaseExpected);
        if (Expected != BaseExpected)
        {
            ++OverriddenRows;
        }
        const FString Status = Fields.IsValidIndex(12) ? Fields[12].TrimStartAndEnd() : FString();

        if (Status == TEXT("n/a") || Status == TEXT("nothing"))
        {
            continue;
        }

        // EventGraph rows have no function-graph name: the data-output event lives in its stub, and the
        // computed-jump table lives in the generated ubergraph.
        const bool bEventGraphRow = Graph == TEXT("EventGraph");
        if (!bEventGraphRow && !Graph.StartsWith(TEXT("F_")))
        {
            continue;
        }
        if (Expected.IsEmpty() || Expected == TEXT("n/a"))
        {
            continue;
        }

        UFunction* Function = nullptr;
        FString CaptureName = Graph;
        if (bEventGraphRow)
        {
            if (RowId == TEXT("let_persistent_frame"))
            {
                CaptureName = TEXT("E_WithDataOut");
                Function = Functions.FindRef(FName(*CaptureName));
            }
            else
            {
                Function = Ubergraph;
                CaptureName = Ubergraph != nullptr ? Ubergraph->GetName() : TEXT("ExecuteUbergraph");
            }
        }
        else
        {
            Function = Functions.FindRef(FName(*Graph));
            if (!Function)
            {
                Function = Functions.FindRef(FName(*Graph.RightChop(2)));
            }
        }

        if (!Function || Function->Script.Num() == 0)
        {
            if (bEventGraphRow)
            {
                AddError(FString::Printf(TEXT("%s: %s is missing or empty; the EventGraph fixture was not authored."), *RowId, *CaptureName));
            }
            else
            {
                ++MissingFunction;
                MissingFunctions.Add(Graph);
            }
            continue;
        }

        const TArray<EExprToken> Captured = BlueprintInspectorTests::CaptureOpcodes(Function);
        TArray<FString> ExpectedTokens;
        Expected.ParseIntoArray(ExpectedTokens, TEXT(";"), true);
        for (FString& Token : ExpectedTokens)
        {
            Token.TrimStartAndEndInline();
        }

        TArray<FString> CapturedNames;
        CapturedNames.Reserve(Captured.Num());
        for (const EExprToken Token : Captured)
        {
            CapturedNames.Add(FBlueprintOpcodeLogger::OpcodeToString(Token));
        }

        // A plan that starts with a tracepoint is a full capture: compare every token. The short plans
        // describe the statement's core, so they are matched as an ordered subsequence.
        const bool bFullPlan = ExpectedTokens.Num() > 1 && ExpectedTokens[0] == TEXT("EX_Tracepoint");
        bool bMatched = true;

        if (bFullPlan)
        {
            bMatched = CapturedNames == ExpectedTokens;
            if (!bMatched)
            {
                AddError(FString::Printf(TEXT("%s: expected exact capture %s; captured %s"), *RowId, *FString::Join(ExpectedTokens, TEXT(";")), *FString::Join(CapturedNames, TEXT(";"))));
            }
        }
        else
        {
            // A short plan is matched as an ordered subsequence. The capture also carries runtime
            // instrumentation (tracepoints) and the implicit return/terminator opcodes that the plan does
            // not list, so the plan cannot be anchored to the capture's ends without rejecting valid rows.
            bMatched = true;
            int32 Cursor = 0;
            for (const FString& Token : ExpectedTokens)
            {
                while (Cursor < Captured.Num() && FBlueprintOpcodeLogger::OpcodeToString(Captured[Cursor]) != Token)
                {
                    ++Cursor;
                }
                if (Cursor >= Captured.Num())
                {
                    bMatched = false;
                    AddError(FString::Printf(TEXT("%s: expected %s in order; captured %s"), *RowId, *Token, *FString::Join(CapturedNames, TEXT(";"))));
                    break;
                }
                ++Cursor;
            }
        }

        ++Checked;
        if (!bMatched)
        {
            ++Mismatched;
        }

        // A plan that ends on a jump is a loop latch capture; it stops at the latch instead of the
        // script terminator.
        const bool bLoopFixture = ExpectedTokens.Num() > 0 && ExpectedTokens.Last() == TEXT("EX_Jump");
        const bool bEndsAtScriptEnd = Captured.Num() > 0 && Captured.Last() == EX_EndOfScript;
        const bool bEndsAtLatch = bLoopFixture && Captured.Num() > 0 && Captured.Last() == EX_Jump;

        // A computed jump ends the walk where it stands: its target is a runtime value, so the capture
        // stops on the operand instead of reaching the script terminator. Accept that only for the row
        // whose plan is the computed jump itself.
        const bool bEndsAtComputedJump = ExpectedTokens.Num() > 0 && ExpectedTokens.Last() == TEXT("EX_ComputedJump") && Captured.Num() > 0 && Captured.Contains(EX_ComputedJump);

        if (Captured.Num() > 0 && Captured.Last() == EX_Jump)
        {
            AddInfo(FString::Printf(TEXT("%s: capture ends on EX_Jump (loop fixture: %s)"), *RowId, bLoopFixture ? TEXT("yes") : TEXT("no")));
        }
        if (bLoopFixture)
        {
            AddInfo(FString::Printf(TEXT("%s exact tokens: %s"), *RowId, *FString::Join(CapturedNames, TEXT(";"))));
        }

        if (!bEndsAtScriptEnd && !bEndsAtLatch && !bEndsAtComputedJump)
        {
            const FString LastToken = Captured.Num() ? FBlueprintOpcodeLogger::OpcodeToString(Captured.Last()) : TEXT("<empty>");
            AddError(FString::Printf(TEXT("%s: capture ends on %s, not EX_EndOfScript, a loop latch or a computed jump"), *RowId, *LastToken));
        }
    }

    if (MissingFunctions.Num() > 0)
    {
        AddInfo(FString::Printf(TEXT("Missing fixture functions: %s"), *FString::Join(MissingFunctions, TEXT(", "))));
    }

    // The fixtures this engine cannot author (see GetBlueprintKnownMissingFixtures); pinning the set
    // makes an unexpected absence fail here.
    const TArray<FString> KnownMissing = GetBlueprintKnownMissingFixtures();

    TArray<FString> UnexpectedMissing;
    for (const FString& Function : MissingFunctions)
    {
        if (!KnownMissing.Contains(Function))
        {
            UnexpectedMissing.Add(Function);
        }
    }

    TArray<FString> Recovered;
    for (const FString& Function : KnownMissing)
    {
        if (!MissingFunctions.Contains(Function))
        {
            Recovered.Add(Function);
        }
    }

    if (Recovered.Num() > 0)
    {
        AddInfo(FString::Printf(TEXT("Known fixture gaps now present (update the pinned list): %s"), *FString::Join(Recovered, TEXT(", "))));
    }

    if (UnexpectedMissing.Num() > 0)
    {
        AddError(FString::Printf(TEXT("Unexpected missing fixture functions: %s"), *FString::Join(UnexpectedMissing, TEXT(", "))));
    }

    AddInfo(FString::Printf(TEXT("Fixture plan: engine %s, %d per-engine override row(s) applied."), *EngineTag, OverriddenRows));
    AddInfo(FString::Printf(TEXT("Fixture token check: %d function(s) checked, %d mismatch(es), %d function(s) absent from the asset."), Checked, Mismatched, MissingFunction));

    TestTrue(TEXT("at least one fixture function was checked"), Checked > 0);
    TestEqual(TEXT("fixture token mismatches"), Mismatched, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorPlanOverridesTest, "BlueprintInspector.Fixture.PlanOverridesResolve", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorPlanOverridesTest::RunTest(const FString& Parameters)
{
    using BlueprintInspectorTests::FFixturePlanOverride;

    TArray<FFixturePlanOverride> Overrides;
    FFixturePlanOverride First;
    First.FixtureId = TEXT("const_struct");
    First.Engine = TEXT("9.9");
    First.Opcodes.Add(TEXT("EX_StructConst"));
    Overrides.Add(First);

    // A later row for the same fixture and engine corrects the earlier one.
    FFixturePlanOverride Corrected = First;
    Corrected.Opcodes.Add(TEXT("EX_EndStructConst"));
    Overrides.Add(Corrected);

    FFixturePlanOverride OtherEngine = First;
    OtherEngine.Engine = TEXT("9.10");
    OtherEngine.Opcodes.Add(TEXT("EX_Tracepoint"));
    Overrides.Add(OtherEngine);

    TestEqual(TEXT("the matching engine's override replaces the base plan"), BlueprintInspectorTests::ResolveFixtureOpcodes(Overrides, TEXT("const_struct"), TEXT("9.9"), TEXT("EX_Base")), FString(TEXT("EX_StructConst;EX_EndStructConst")));
    TestEqual(TEXT("each engine reads only its own row"), BlueprintInspectorTests::ResolveFixtureOpcodes(Overrides, TEXT("const_struct"), TEXT("9.10"), TEXT("EX_Base")), FString(TEXT("EX_StructConst;EX_Tracepoint")));
    TestEqual(TEXT("an engine with no row keeps the base plan"), BlueprintInspectorTests::ResolveFixtureOpcodes(Overrides, TEXT("const_struct"), TEXT("9.8"), TEXT("EX_Base")), FString(TEXT("EX_Base")));
    TestEqual(TEXT("a fixture with no row keeps the base plan"), BlueprintInspectorTests::ResolveFixtureOpcodes(Overrides, TEXT("let_int"), TEXT("9.9"), TEXT("EX_Base")), FString(TEXT("EX_Base")));

    // The documented table has to parse, and every row has to name a fixture the base plan defines:
    // a typo would otherwise silently do nothing.
    TArray<FFixturePlanOverride> Documented;
    if (!TestTrue(TEXT("the layout guide carries the override table"), BlueprintInspectorTests::LoadFixturePlanOverrides(Documented)))
    {
        return false;
    }

    TArray<TArray<FString>> Rows;
    if (!TestTrue(TEXT("the layout guide carries the fixture CSV"), BlueprintInspectorTests::LoadFixtureRows(Rows)))
    {
        return false;
    }

    TSet<FString> FixtureIds;
    for (const TArray<FString>& Fields : Rows)
    {
        if (Fields.Num() > 0)
        {
            FixtureIds.Add(Fields[0].TrimStartAndEnd());
        }
    }
    for (const FFixturePlanOverride& Override : Documented)
    {
        TestTrue(FString::Printf(TEXT("override row %s names a fixture in the base plan"), *Override.FixtureId), FixtureIds.Contains(Override.FixtureId));

        FString Major;
        FString Minor;
        Override.Engine.Split(TEXT("."), &Major, &Minor);
        TestTrue(FString::Printf(TEXT("override row %s names an engine as <major>.<minor>"), *Override.FixtureId), !Major.IsEmpty() && !Minor.IsEmpty() && Major.IsNumeric() && Minor.IsNumeric());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInterfaceReceiverTest, "BlueprintInspector.Disassembler.InterfaceReceiverResolves", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInterfaceReceiverTest::RunTest(const FString& Parameters)
{
    UClass* const MainClass = BlueprintFixtureSource::GetMainClass();
    UFunction* const Source = BlueprintFixtureSource::FindFunction(TEXT("interfacecontext_messagecall"));
    FProperty* const InterfaceVar = MainClass ? MainClass->FindPropertyByName(TEXT("InterfaceVar")) : nullptr;
    if (Source == nullptr || InterfaceVar == nullptr)
    {
        AddError(TEXT("interfacecontext_messagecall or InterfaceVar is missing."));
        return false;
    }

    TArray<uint8> Script = Source->Script;

    // Retarget the interface-context receiver: the EX_LocalVariable that follows EX_InterfaceContext.
    // The same temp also appears as the cast's assignment destination, so a plain first-occurrence scan
    // would change the assignment instead of the receiver.
    FProperty* const CastResult = FindFProperty<FProperty>(Source, TEXT("K2Node_DynamicCast_AsBPI_Probe_A"));
    if (CastResult == nullptr)
    {
        AddError(TEXT("The interface message-call fixture has no K2Node_DynamicCast_AsBPI_Probe_A temp to retarget."));
        return false;
    }

    int32 VariableOpcodeOffset = INDEX_NONE;
    for (int32 Offset = 0; Offset + 2 + static_cast<int32>(sizeof(FProperty*)) <= Script.Num(); ++Offset)
    {
        if (static_cast<EExprToken>(Script[Offset]) != EX_InterfaceContext || static_cast<EExprToken>(Script[Offset + 1]) != EX_LocalVariable)
        {
            continue;
        }

        FProperty* Operand = nullptr;
        FMemory::Memcpy(static_cast<void*>(&Operand), Script.GetData() + Offset + 2, sizeof(FProperty*));
        if (Operand == CastResult)
        {
            VariableOpcodeOffset = Offset + 1;
            break;
        }
    }

    if (VariableOpcodeOffset == INDEX_NONE)
    {
        AddError(TEXT("The interface message-call fixture has no interface-context receiver naming the cast result."));
        return false;
    }

    const int32 VariableOperandOffset = VariableOpcodeOffset + 1;
    FProperty* const Property = InterfaceVar;
    FMemory::Memcpy(Script.GetData() + VariableOperandOffset, static_cast<const void*>(&Property), sizeof(FProperty*));

    UFunction* const Function = NewObject<UFunction>(MainClass, MakeUniqueObjectName(MainClass, UFunction::StaticClass(), TEXT("InterfaceReceiver")), RF_Transient);
    Function->Script = MoveTemp(Script);
    TStrongObjectPtr<UFunction> KeepAlive(Function);

    const FString Rendered = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Rendered);

    TestTrue(TEXT("the interface receiver renders"), Rendered.Contains(TEXT("InterfaceVar->IProbeFunc(7);")));
    TestTrue(TEXT("the enclosing cast still renders"), Rendered.Contains(TEXT("if (K2Node_DynamicCast_bSuccess)")));
    TestFalse(TEXT("the call is not a transfer"), Rendered.Contains(TEXT("goto")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSwitchOperandTest, "BlueprintInspector.Rendering.SwitchOperandCapturesWhenNotIdentifier", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSwitchOperandTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = BlueprintFixtureSource::FindFunction(TEXT("flow_switchvalue"));
    if (!TestNotNull(TEXT("the fixture has flow_switchvalue"), Function))
    {
        return false;
    }

    FBlueprintScriptDisassembler Disassembler;
    if (!TestTrue(TEXT("the switch value fixture disassembles"), Disassembler.FunctionToNodes(Function)))
    {
        return false;
    }

    FBlueprintNode* const Switch = BlueprintInspectorTests::FindSwitchValueNode(Disassembler);
    if (!TestNotNull(TEXT("the graph has a switch value node"), Switch))
    {
        return false;
    }

    Switch->InputNodes[0].Emplace<EExprToken>(EX_IntOne);

    FString Script;
    FBlueprintScriptVisualizer_Pseudo Visualizer;
    Visualizer.Visualize(Function, Disassembler, Script);
    AddInfo(Script);

    TestTrue(TEXT("a non-identifier switch operand captures by default"), Script.Contains(TEXT("[&]()")));
    TestTrue(TEXT("the literal remains the switch subject"), Script.Contains(TEXT("switch (1)")));
    TestFalse(TEXT("the operand is not used as a capture name"), Script.Contains(TEXT("[&1]")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorDelegateLiteralTest, "BlueprintInspector.Disassembler.DelegateLiteralIsTyped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorDelegateLiteralTest::RunTest(const FString& Parameters)
{
    TArray<uint8> Script;
    BlueprintInspectorTests::AppendOpcode(Script, EX_Return);
    BlueprintInspectorTests::AppendOpcode(Script, EX_InstanceDelegate);
    const FScriptName Name = NameToScriptName(FName(TEXT("F_ProbeDelegateHandler")));
    Script.Append(reinterpret_cast<const uint8*>(&Name), static_cast<int32>(sizeof(Name)));
    BlueprintInspectorTests::AppendOpcode(Script, EX_EndOfScript);

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestDelegateLiteral"), MoveTemp(Script));
    const FString Rendered = BlueprintInspectorTests::RenderFunction(Function);
    AddInfo(Rendered);

    TestTrue(TEXT("the delegate literal is typed"), Rendered.Contains(TEXT("return FScriptDelegate{\"F_ProbeDelegateHandler\"};")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorOpcodeStatsTest, "BlueprintInspector.Disassembler.OpcodeStatsCountUnsupported", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorOpcodeStatsTest::RunTest(const FString& Parameters)
{
    BlueprintInspectorTests::AddExpectedPlainError(*this, TEXT("Unhandled opcode 0x7F at script offset 0"), 1);

    TArray<uint8> Script;
    BlueprintInspectorTests::AppendOpcode(Script, static_cast<EExprToken>(0x7F));
    BlueprintInspectorTests::AppendOpcode(Script, EX_Return);
    BlueprintInspectorTests::AppendOpcode(Script, EX_Nothing);
    BlueprintInspectorTests::AppendOpcode(Script, EX_EndOfScript);

    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestOpcodeStats"), MoveTemp(Script));

    FString CapturedFunction;
    FBlueprintOpcodeStats CapturedStats;
    FBlueprintOpcodeLogger::SetSummarySink([&CapturedFunction, &CapturedStats](FString FunctionName, FBlueprintOpcodeStats Stats) {
        CapturedFunction = FunctionName;
        CapturedStats = Stats;
    });
    ON_SCOPE_EXIT
    {
        FBlueprintOpcodeLogger::ClearSummarySink();
    };

    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Function);

    TestTrue(TEXT("the summary names the function"), CapturedFunction.Contains(TEXT("BlueprintInspectorTestOpcodeStats")));
    TestEqual(TEXT("the unsupported opcode is counted"), CapturedStats.Unsupported, 1);
    TestTrue(TEXT("the run counted opcodes"), CapturedStats.Opcodes > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorDapAuthTest, "BlueprintInspector.Debugger.DapAuthRejectsWrongToken", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorDapAuthTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FUBlueprintInspectorDap> Dap = FUBlueprintInspectorDap::Create();
    TestFalse(TEXT("a fresh adapter is unauthenticated"), Dap->IsAuthenticated());
    TestFalse(TEXT("an empty token is rejected before a session publishes one"), Dap->Authenticate(FString(), 0));
    TestFalse(TEXT("a wrong token is rejected"), Dap->Authenticate(TEXT("not-the-session-token"), 0));
    TestFalse(TEXT("a rejected token leaves the adapter unauthenticated"), Dap->IsAuthenticated());

    // A queued request owns a cppdap Respond callback, so a reconnect or a reset must answer it rather
    // than drop it. A reset for an older generation must leave the current one's queue alone.
    {
        FSessionAuth Auth;
        uint64 Generation = 0;
        Auth.Begin(Generation);

        int32 Calls = 0;
        bool bLastAllowed = true;
        uint64 TimerGeneration = 0;
        uint64 TimerBatch = 0;
        Auth.Queue(Generation, [&Calls, &bLastAllowed](const bool bAllowed) { ++Calls; bLastAllowed = bAllowed; }, TimerGeneration, TimerBatch);

        uint64 NextGeneration = 0;
        Auth.Begin(NextGeneration);
        TestEqual(TEXT("a reconnect answers the previous generation's queued request"), Calls, 1);
        TestFalse(TEXT("the reconnected request is answered as unauthorized"), bLastAllowed);
        TestTrue(TEXT("the new generation is distinct"), NextGeneration != Generation);

        int32 StaleCalls = 0;
        bool bStaleAllowed = true;
        uint64 StaleTimerGeneration = 0;
        uint64 StaleTimerBatch = 0;
        TestEqual(TEXT("a request from a retired connection is rejected instead of queued"), Auth.Queue(Generation, [&StaleCalls, &bStaleAllowed](const bool bAllowed) { ++StaleCalls; bStaleAllowed = bAllowed; }, StaleTimerGeneration, StaleTimerBatch), FSessionAuth::EQueueResult::Rejected);
        TestEqual(TEXT("the retired request is not held for the next handshake"), StaleCalls, 0);

        // A local client can send gated requests faster than it authenticates; the queue is capped.
        FSessionAuth BoundedAuth;
        uint64 BoundedGeneration = 0;
        BoundedAuth.Begin(BoundedGeneration);
        uint64 BoundedTimerGeneration = 0;
        uint64 BoundedTimerBatch = 0;
        for (int32 Index = 0; Index < FSessionAuth::MaxPendingRequests; ++Index)
        {
            BoundedAuth.Queue(BoundedGeneration, [](const bool) {}, BoundedTimerGeneration, BoundedTimerBatch);
        }
        TestEqual(TEXT("the pending queue rejects a request past its cap"), BoundedAuth.Queue(BoundedGeneration, [](const bool) {}, BoundedTimerGeneration, BoundedTimerBatch), FSessionAuth::EQueueResult::Rejected);

        int32 ResetCalls = 0;
        bool bResetAllowed = true;
        uint64 NextTimerGeneration = 0;
        uint64 NextTimerBatch = 0;
        Auth.Queue(NextGeneration, [&ResetCalls, &bResetAllowed](const bool bAllowed) { ++ResetCalls; bResetAllowed = bAllowed; }, NextTimerGeneration, NextTimerBatch);
        Auth.Reset(Generation);
        TestEqual(TEXT("a stale reset leaves the current generation's request queued"), ResetCalls, 0);
        Auth.Reset(NextGeneration);
        TestEqual(TEXT("a reset answers its own queued request"), ResetCalls, 1);
        TestFalse(TEXT("the reset request is answered as unauthorized"), bResetAllowed);
    }

    // The auth negatives the releasing contract names: an empty handshake, a stale generation, a stale
    // token after a reconnect, and a timeout that belongs to one generation only.
    {
        FSessionAuth Auth;
        uint64 Generation = 0;
        const FString Token = Auth.Begin(Generation);

        int32 Attempts = 0;
        uint64 AttemptGeneration = 0;
        TestTrue(TEXT("the published token authenticates"), Auth.Authenticate(Token, Generation, Attempts, AttemptGeneration) == FSessionAuth::EHandshakeResult::Accepted);
        TestTrue(TEXT("the accepted handshake belongs to the current generation"), AttemptGeneration == Generation);

        TestTrue(TEXT("an empty token cannot ride the sticky flag"), Auth.Authenticate(FString(), Generation, Attempts, AttemptGeneration) == FSessionAuth::EHandshakeResult::Rejected);
        TestEqual(TEXT("the empty handshake counts as a rejected attempt"), Attempts, 1);
        TestTrue(TEXT("a wrong non-empty token is rejected after authentication"), Auth.Authenticate(TEXT("not-the-session-token"), Generation, Attempts, AttemptGeneration) == FSessionAuth::EHandshakeResult::Rejected);
        TestTrue(TEXT("a rejected empty handshake leaves the connection authenticated"), Auth.IsAuthenticated());

        uint64 NextGeneration = 0;
        const FString NextToken = Auth.Begin(NextGeneration);
        TestTrue(TEXT("a handshake from the retired generation is not applied"), Auth.Authenticate(NextToken, Generation, Attempts, AttemptGeneration) == FSessionAuth::EHandshakeResult::StaleGeneration);
        TestEqual(TEXT("a stale handshake does not count an attempt"), Attempts, 0);
        TestTrue(TEXT("the previous token is stale after a reconnect"), Auth.Authenticate(Token, NextGeneration, Attempts, AttemptGeneration) == FSessionAuth::EHandshakeResult::Rejected);
        TestTrue(TEXT("the new token authenticates"), Auth.Authenticate(NextToken, NextGeneration, Attempts, AttemptGeneration) == FSessionAuth::EHandshakeResult::Accepted);
        TestTrue(TEXT("the accepted handshake belongs to the new generation"), AttemptGeneration == NextGeneration);
    }
    {
        FSessionAuth Auth;
        uint64 First = 0;
        Auth.Begin(First);
        Auth.Reset(First);
        TestFalse(TEXT("a reset generation is stale"), Auth.IsUnauthenticatedFor(First));
        uint64 TimerGeneration = 0;
        uint64 TimerBatch = 0;
        TestTrue(TEXT("a request from the reset generation is rejected"), Auth.Queue(First, [](const bool) {}, TimerGeneration, TimerBatch) == FSessionAuth::EQueueResult::Rejected);
    }
    {
        FSessionAuth Auth;
        uint64 First = 0;
        uint64 Second = 0;
        Auth.Begin(First);
        TestTrue(TEXT("an unauthenticated current generation times out"), Auth.IsUnauthenticatedFor(First));
        Auth.Begin(Second);
        TestFalse(TEXT("a stale generation cannot time out the current one"), Auth.IsUnauthenticatedFor(First));
        TestTrue(TEXT("the current generation times out"), Auth.IsUnauthenticatedFor(Second));
    }
    {
        FSessionAuth Auth;
        uint64 Generation = 0;
        Auth.Begin(Generation);

        uint64 TimerGeneration = 0;
        uint64 FirstBatch = 0;
        TestTrue(TEXT("an unauthenticated request arms a deadline"), Auth.Queue(Generation, [](const bool) {}, TimerGeneration, FirstBatch) == FSessionAuth::EQueueResult::QueuedWithTimer);

        Auth.Drain();

        uint64 SecondBatch = 0;
        TestTrue(TEXT("a request after the drain arms a new deadline"), Auth.Queue(Generation, [](const bool) {}, TimerGeneration, SecondBatch) == FSessionAuth::EQueueResult::QueuedWithTimer);
        TestTrue(TEXT("the new deadline has its own batch"), SecondBatch != FirstBatch);
        TestFalse(TEXT("a timer for the drained batch cannot reject the new one"), Auth.IsExpiredFor(Generation, FirstBatch));
        TestTrue(TEXT("the current batch can expire"), Auth.IsExpiredFor(Generation, SecondBatch));
    }

    // The accept slot: a stale close must not mark the accept that replaced it as dead.
    {
        FConnectingSlot Slot;
        TestFalse(TEXT("a fresh accept slot is idle"), Slot.IsConnecting());
        Slot.Begin();
        TestTrue(TEXT("the accept slot is claimed"), Slot.IsConnecting());
        TestFalse(TEXT("an unclaimed generation does not own the slot"), Slot.IsFor(1));
        Slot.SetGeneration(1);
        TestTrue(TEXT("the claimed generation owns the slot"), Slot.IsFor(1));
        Slot.MarkClosed(2);
        TestFalse(TEXT("a stale close does not mark the slot closed"), Slot.Finish());
        TestFalse(TEXT("the slot is idle after finishing"), Slot.IsConnecting());

        Slot.Begin();
        Slot.SetGeneration(3);
        Slot.MarkClosed(3);
        TestTrue(TEXT("the owning generation marks the slot closed"), Slot.Finish());
    }

    // The published session, socket and generation move as one: a decision taken for an older
    // generation must not reach the connection that replaced it.
    {
        struct FStubReaderWriter final : dap::ReaderWriter
        {
            bool isOpen() override { return true; }
            void close() override {}
            size_t read(void*, size_t) override { return 0; }
            bool write(const void*, size_t) override { return true; }
        };

        FPublishedConnection Published;
        TestFalse(TEXT("nothing is published initially"), Published.IsPublished());
        TestTrue(TEXT("a close for an unpublished generation reaches nothing"), Published.SocketForClose(1) == nullptr);

        const TSharedPtr<dap::Session> FirstSession(dap::Session::create().release());
        const TSharedPtr<dap::Session> SecondSession(dap::Session::create().release());
        const std::shared_ptr<dap::ReaderWriter> FirstSocket = std::make_shared<FStubReaderWriter>();
        const std::shared_ptr<dap::ReaderWriter> SecondSocket = std::make_shared<FStubReaderWriter>();

        Published.Publish(FirstSession, FirstSocket, 1);
        TestTrue(TEXT("the published generation reaches its socket"), Published.SocketForClose(1) == FirstSocket);
        TestTrue(TEXT("an older generation reaches no socket"), Published.SocketForClose(2) == nullptr);
        TestTrue(TEXT("a stale generation cannot retire the connection"), !Published.Retire(2).IsValid());
        TestTrue(TEXT("the current connection stays published"), Published.IsPublished());

        Published.Publish(SecondSession, SecondSocket, 2);
        TestTrue(TEXT("the replacement reaches its own socket"), Published.SocketForClose(2) == SecondSocket);
        TestTrue(TEXT("the replaced generation reaches no socket"), Published.SocketForClose(1) == nullptr);

        const TSharedPtr<dap::Session> Retired = Published.Retire(2);
        TestTrue(TEXT("the current generation retires its session"), Retired == SecondSession);
        TestFalse(TEXT("nothing is published after the retire"), Published.IsPublished());
        TestTrue(TEXT("a retired generation reaches no socket"), Published.SocketForClose(2) == nullptr);
    }

    // A default-constructed result must not look like a legitimate empty answer: the gate returns one
    // when a callback copy loses its session, and the handlers report bTimedOut as an error.
    TestTrue(TEXT("a default game-thread result reads as unavailable"), TGameThreadCallResult<FString>().bTimedOut);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintVariableWriteStressTest, "BlueprintInspector.Debugger.VariableWriteRequest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintVariableWriteStressTest::RunTest(const FString& Parameters)
{
    // Deterministic: a timeout cancels the request, then a stale queue snapshot applies it. Only the
    // cancel may complete, and the work must not run.
    {
        TSharedPtr<FBlueprintVariableWriteRequest> Request = MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"));
        int32 WorkRuns = 0;
        Request->Wait(FTimespan::Zero());
        Request->Apply([&WorkRuns]() {
            ++WorkRuns;
            FBlueprintSetVariableResult Result;
            Result.bSuccess = true;
            return Result;
        });
        TestEqual(TEXT("the canceled request completes once"), Request->GetCompletionCount(), 1);
        TestEqual(TEXT("the canceled request never applies"), WorkRuns, 0);
    }

    // A setter that throws still has to complete the request: the waiter must see an error, and the
    // single-shot counter must stay at one rather than the request staying claimed forever.
    {
        TSharedPtr<FBlueprintVariableWriteRequest> Request = MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"));
        Request->Apply([]() -> FBlueprintSetVariableResult {
            throw std::runtime_error("setter threw");
        });
        TestEqual(TEXT("a throwing write completes once"), Request->GetCompletionCount(), 1);
        const FBlueprintSetVariableResult Result = Request->Wait(FTimespan::Zero());
        TestFalse(TEXT("a throwing write reports failure"), Result.bSuccess);
        TestTrue(TEXT("a throwing write reports the exception"), Result.Error.Contains(TEXT("threw")));
    }

    // The race: a waiter timing out against an apply, 1,000 times. Exactly one completion and at most
    // one application each time, whichever side wins.
    int32 BadCompletions = 0;
    int32 DoubleApplies = 0;
    for (int32 Index = 0; Index < 1000; ++Index)
    {
        TSharedPtr<FBlueprintVariableWriteRequest> Request = MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"));
        std::atomic<int32> WorkRuns{0};
        std::atomic<bool> bStarted{false};

        std::thread Waiter([Request, &bStarted]() {
            while (!bStarted.load())
            {
                std::this_thread::yield();
            }
            Request->Wait(FTimespan::Zero());
        });

        bStarted.store(true);
        Request->Apply([&WorkRuns]() {
            ++WorkRuns;
            FBlueprintSetVariableResult Result;
            Result.bSuccess = true;
            return Result;
        });
        Waiter.join();

        BadCompletions += Request->GetCompletionCount() != 1 ? 1 : 0;
        DoubleApplies += WorkRuns.load() > 1 ? 1 : 0;
    }
    TestEqual(TEXT("every raced request completes exactly once"), BadCompletions, 0);
    TestEqual(TEXT("no raced request applies twice"), DoubleApplies, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintVariableWriteUnknownTest, "BlueprintInspector.Debugger.VariableWriteUnknownOutcome", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintVariableWriteUnknownTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FBlueprintVariableWriteRequest> Request = MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"));
    std::atomic<bool> bWorkStarted{false};

    std::thread Applier([Request, &bWorkStarted]() {
        Request->Apply([&bWorkStarted]() {
            bWorkStarted.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            FBlueprintSetVariableResult Result;
            Result.bSuccess = true;
            Result.Value = TEXT("1");
            return Result;
        });
    });

    while (!bWorkStarted.load())
    {
        std::this_thread::yield();
    }

    const FBlueprintSetVariableResult WaitResult = Request->Wait(FTimespan::Zero());
    TestTrue(TEXT("a claimed write that outlives the grace reports an unknown outcome"), WaitResult.bOutcomeUnknown);
    TestFalse(TEXT("an unknown outcome is not a success"), WaitResult.bSuccess);
    TestTrue(TEXT("the unknown message asks for a retry"), WaitResult.Error.Contains(TEXT("retry")));

    Applier.join();
    TestEqual(TEXT("the game thread completes the request once"), Request->GetCompletionCount(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPauseWriteHandshakeTest, "BlueprintInspector.Debugger.PauseWriteHandshake", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPauseWriteHandshakeTest::RunTest(const FString& Parameters)
{
    // The object under test is the one the subsystem uses: the paused flag, the lock BreakpointRequestLock
    // stands in for, and the queue a DAP thread enqueues into.
    FCriticalSection Lock;
    FBlueprintPauseState PauseState(Lock);

    // A pause that ends before the write arrives rejects it outright, so the request never waits for a
    // game thread that has already stopped draining.
    PauseState.PublishPaused();
    PauseState.EndPause();
    {
        TSharedPtr<FBlueprintVariableWriteRequest> Late = MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"));
        TestTrue(TEXT("a write after the pause ends is reported as not paused"), PauseState.EnqueueIfPaused(Late) == FBlueprintPauseState::EWriteEnqueueResult::NotPaused);
        TestEqual(TEXT("a rejected write never completes"), Late->GetCompletionCount(), 0);
    }

    {
        FCriticalSection FullLock;
        FBlueprintPauseState FullState(FullLock);
        FullState.PublishPaused();
        int32 Accepted = 0;
        for (int32 Index = 0; Index < FBlueprintPauseState::MaxQueuedWrites; ++Index)
        {
            Accepted += FullState.EnqueueIfPaused(MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"))) == FBlueprintPauseState::EWriteEnqueueResult::Queued ? 1 : 0;
        }
        TestEqual(TEXT("the queue accepts exactly its bound"), Accepted, FBlueprintPauseState::MaxQueuedWrites);
        TestTrue(TEXT("a full queue is reported apart from an ended pause"), FullState.EnqueueIfPaused(MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"))) == FBlueprintPauseState::EWriteEnqueueResult::QueueFull);
    }

    // 1,000 pause/resume cycles. The first half waits for the enqueue to land, so those cycles really
    // drain the queue: even cycles apply from the parked loop, odd cycles resume and cancel. The second
    // half does not wait, so the end of the pause can beat the enqueue outright. Either way the
    // transition itself must resolve the request: one left queued for a loop that stopped draining would
    // sit in Wait() until its five-second timeout.
    int32 BadCompletions = 0;
    int32 DoubleApplies = 0;
    int32 SlowWaits = 0;
    int32 AppliedCycles = 0;
    int32 CanceledCycles = 0;
    int32 AppliedExpected = 0;
    int32 CanceledExpected = 0;
    int32 UnsynchronizedCycles = 0;

    constexpr int32 CycleCount = 1000;
    constexpr int32 SynchronizedCycles = CycleCount / 2;

    for (int32 Index = 0; Index < CycleCount; ++Index)
    {
        PauseState.PublishPaused();

        TSharedPtr<FBlueprintVariableWriteRequest> Request = MakeShared<FBlueprintVariableWriteRequest>(1, TEXT("X"), TEXT("1"));
        std::atomic<int32> WorkRuns{0};
        std::atomic<bool> bEnqueued{false};
        FBlueprintSetVariableResult WaitResult;

        std::thread Waiter([&]() {
            if (PauseState.EnqueueIfPaused(Request) != FBlueprintPauseState::EWriteEnqueueResult::Queued)
            {
                return;
            }
            bEnqueued.store(true);
            const std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
            WaitResult = Request->Wait(FTimespan::FromSeconds(5.0));
        });

        const bool bWantedSync = Index < SynchronizedCycles;
        if (bWantedSync)
        {
            // Let the enqueue land first, so this cycle covers a drain rather than a rejection.
            // Bound the spin: a rejected enqueue would otherwise hang the automation run.
            const std::chrono::steady_clock::time_point SpinStart = std::chrono::steady_clock::now();
            while (!bEnqueued.load() && std::chrono::duration<double>(std::chrono::steady_clock::now() - SpinStart).count() < 0.5)
            {
                std::this_thread::yield();
            }
        }

        const bool bSynchronized = bWantedSync && bEnqueued.load();
        if (bWantedSync && !bSynchronized)
        {
            ++UnsynchronizedCycles;
        }

        if (Index % 2 == 0)
        {
            // The parked loop: take what is queued and apply it in place.
            for (const TSharedPtr<FBlueprintVariableWriteRequest>& Queued : PauseState.TakeQueuedWrites())
            {
                Queued->Apply([&WorkRuns]() {
                    ++WorkRuns;
                    FBlueprintSetVariableResult Result;
                    Result.bSuccess = true;
                    return Result;
                });
            }
        }

        // The pause ends. Whatever the parked loop did not take is canceled here, and a write that arrives
        // after this transition is rejected by EnqueueIfPaused.
        for (const TSharedPtr<FBlueprintVariableWriteRequest>& Queued : PauseState.EndPause())
        {
            Queued->Cancel();
        }

        Waiter.join();

        if (bEnqueued.load())
        {
            BadCompletions += Request->GetCompletionCount() != 1 ? 1 : 0;
            SlowWaits += WaitResult.Error.Contains(TEXT("before the timeout")) ? 1 : 0;
        }
        else
        {
            BadCompletions += Request->GetCompletionCount() != 0 ? 1 : 0;
        }
        DoubleApplies += WorkRuns.load() > 1 ? 1 : 0;

        if (bWantedSync && !bSynchronized)
        {
            break;
        }

        if (bSynchronized)
        {
            if (Index % 2 == 0)
            {
                ++AppliedExpected;
                AppliedCycles += WorkRuns.load() == 1 && WaitResult.bSuccess ? 1 : 0;
            }
            else
            {
                ++CanceledExpected;
                CanceledCycles += WorkRuns.load() == 0 && !WaitResult.bSuccess ? 1 : 0;
            }
        }
    }

    TestEqual(TEXT("every queued write completes exactly once"), BadCompletions, 0);
    TestEqual(TEXT("no write applies twice"), DoubleApplies, 0);
    TestEqual(TEXT("no write waits for its timeout"), SlowWaits, 0);
    TestEqual(TEXT("the drained cycles apply the write"), AppliedCycles, AppliedExpected);
    TestEqual(TEXT("the resumed cycles cancel the write"), CanceledCycles, CanceledExpected);
    TestEqual(TEXT("every synchronized cycle observed its enqueue"), UnsynchronizedCycles, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPauseReferenceWrapTest, "BlueprintInspector.Debugger.VariableReferenceWrap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPauseReferenceWrapTest::RunTest(const FString& Parameters)
{
    int32 Counter = 0;
    TestEqual(TEXT("a zero counter starts at one"), FBlueprintPauseSession::TakeNextReferenceForTest(Counter, [](int32) { return false; }), 1);

    Counter = TNumericLimits<int32>::Max();
    TestEqual(TEXT("the last reference is issued before the wrap"), FBlueprintPauseSession::TakeNextReferenceForTest(Counter, [](int32) { return false; }), TNumericLimits<int32>::Max());
    TestEqual(TEXT("the counter wraps to one"), Counter, 1);

    const TSet<int32> InUse{1, 2};
    TestEqual(TEXT("a wrapped reference skips the references still in use"), FBlueprintPauseSession::TakeNextReferenceForTest(Counter, [&InUse](const int32 Candidate) { return InUse.Contains(Candidate); }), 3);
    TestEqual(TEXT("the next reference follows the one issued"), Counter, 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintEnumNumberPolicyTest, "BlueprintInspector.Debugger.EnumNumberPolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintEnumNumberPolicyTest::RunTest(const FString& Parameters)
{
    BlueprintInspectorVariableEdit::FEnumValueAcceptance Bitflags;
    Bitflags.bBitflags = true;
    Bitflags.bDeclaredEnumeratorsReadable = true;
    Bitflags.bParsed = true;
    TestTrue(TEXT("a bitflags number is accepted without being a declared enumerator"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(Bitflags));

    BlueprintInspectorVariableEdit::FEnumValueAcceptance Declared;
    Declared.bDeclaredEnumeratorsReadable = true;
    Declared.bParsed = true;
    Declared.bDeclared = true;
    TestTrue(TEXT("a declared enumerator is accepted"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(Declared));

    BlueprintInspectorVariableEdit::FEnumValueAcceptance Undeclared;
    Undeclared.bDeclaredEnumeratorsReadable = true;
    Undeclared.bParsed = true;
    TestFalse(TEXT("an undeclared number is rejected"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(Undeclared));

    BlueprintInspectorVariableEdit::FEnumValueAcceptance Unparsable;
    Unparsable.bDeclaredEnumeratorsReadable = true;
    TestFalse(TEXT("unparsable text is rejected"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(Unparsable));

    BlueprintInspectorVariableEdit::FEnumValueAcceptance NoMetadata;
    NoMetadata.bParsed = true;
    TestTrue(TEXT("without enumerator metadata the range check alone decides"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(NoMetadata));

    BlueprintInspectorVariableEdit::FEnumValueAcceptance GeneratedMax;
    GeneratedMax.bDeclaredEnumeratorsReadable = true;
    GeneratedMax.bParsed = true;
    GeneratedMax.bDeclared = true;
    GeneratedMax.bGeneratedMax = true;
    TestFalse(TEXT("the generated _MAX entry is not a writable value"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(GeneratedMax));

    BlueprintInspectorVariableEdit::FEnumValueAcceptance BitflagsMax;
    BitflagsMax.bBitflags = true;
    BitflagsMax.bDeclaredEnumeratorsReadable = true;
    BitflagsMax.bParsed = true;
    BitflagsMax.bDeclared = true;
    BitflagsMax.bGeneratedMax = true;
    TestTrue(TEXT("a bitflags enum still accepts any declared value"), BlueprintInspectorVariableEdit::AcceptsNumericEnumValue(BitflagsMax));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorVariableEditTest, "BlueprintInspector.Debugger.VariableEdit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorVariableEditTest::RunTest(const FString& Parameters)
{
    UOpcodeProbeHost* const Host = NewObject<UOpcodeProbeHost>();
    TStrongObjectPtr<UOpcodeProbeHost> HostGuard(Host);
    if (!TestNotNull(TEXT("probe host"), Host))
    {
        return false;
    }

    FString OutValue;
    FString OutError;

    const FFloatProperty* const FloatProperty = FindFProperty<FFloatProperty>(UOpcodeProbeHost::StaticClass(), TEXT("FloatVar"));
    if (!TestNotNull(TEXT("float property"), FloatProperty))
    {
        return false;
    }
    TestTrue(TEXT("a float is text-editable"), BlueprintInspectorVariableEdit::IsTextEditable(FloatProperty));
    TestTrue(TEXT("a float imports"), BlueprintInspectorVariableEdit::Apply(FloatProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("1.25"), OutValue, OutError));
    TestTrue(TEXT("the float changed"), FMath::IsNearlyEqual(Host->FloatVar, 1.25f));
    TestFalse(TEXT("a bad float is rejected"), BlueprintInspectorVariableEdit::Apply(FloatProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("not-a-number"), OutValue, OutError));
    TestFalse(TEXT("the parse error is reported"), OutError.IsEmpty());

    const FStructProperty* const CommaStructProperty = FindFProperty<FStructProperty>(UOpcodeProbeHost::StaticClass(), TEXT("CommaStructVar"));
    if (TestNotNull(TEXT("comma struct property"), CommaStructProperty))
    {
        TestTrue(TEXT("a comma inside a quoted struct member stays in the value"), BlueprintInspectorVariableEdit::Apply(CommaStructProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("(TextVar='a,b',NumberVar=1)"), OutValue, OutError));
    }

    const FUInt64Property* const UInt64Property = FindFProperty<FUInt64Property>(UOpcodeProbeHost::StaticClass(), TEXT("UInt64Var"));
    if (TestNotNull(TEXT("uint64 property"), UInt64Property))
    {
        TestTrue(TEXT("a uint64 imports"), BlueprintInspectorVariableEdit::Apply(UInt64Property, reinterpret_cast<uint8*>(Host), Host, TEXT("42"), OutValue, OutError));
        TestTrue(TEXT("the uint64 changed"), Host->UInt64Var == static_cast<uint64>(42));
    }

    const FStructProperty* const VectorProperty = FindFProperty<FStructProperty>(UOpcodeProbeHost::StaticClass(), TEXT("Vector3fVar"));
    if (TestNotNull(TEXT("vector property"), VectorProperty))
    {
        TestTrue(TEXT("a struct of scalars is text-editable"), BlueprintInspectorVariableEdit::IsTextEditable(VectorProperty));
        const bool bStructImported = BlueprintInspectorVariableEdit::Apply(VectorProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("(X=1,Y=2,Z=3)"), OutValue, OutError);
        TestTrue(TEXT("a struct imports"), bStructImported);
        if (!bStructImported)
        {
            AddError(OutError);
        }
        TestTrue(TEXT("the vector changed"), Host->Vector3fVar.Equals(FVector(1.0, 2.0, 3.0)));
    }

    // A struct member out of its range must be rejected with the member named, and the live value must
    // not move: ImportText would otherwise wrap the member before its own error path.
    const FStructProperty* const PointProperty = FindFProperty<FStructProperty>(UOpcodeProbeHost::StaticClass(), TEXT("IntPointVar"));
    if (TestNotNull(TEXT("int point property"), PointProperty))
    {
        TestTrue(TEXT("an int point is text-editable"), BlueprintInspectorVariableEdit::IsTextEditable(PointProperty));
        Host->IntPointVar = FIntPoint(7, 8);
        const bool bOutOfRangeAccepted = BlueprintInspectorVariableEdit::Apply(PointProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("(X=3000000000,Y=0)"), OutValue, OutError);
        TestFalse(TEXT("an out-of-range struct member is rejected"), bOutOfRangeAccepted);
        TestTrue(FString::Printf(TEXT("the rejection names the out-of-range member (%s)"), *OutError), OutError.Contains(TEXT("valid X")));
        TestTrue(TEXT("the rejected struct is unchanged"), Host->IntPointVar == FIntPoint(7, 8));
        const bool bInRangeImported = BlueprintInspectorVariableEdit::Apply(PointProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("(X=1,Y=2)"), OutValue, OutError);
        TestTrue(TEXT("an in-range struct member imports"), bInRangeImported);
        if (!bInRangeImported)
        {
            AddError(OutError);
        }
        TestTrue(TEXT("the int point changed"), Host->IntPointVar == FIntPoint(1, 2));
    }

    const FStructProperty* const PrimaryIdProperty = FindFProperty<FStructProperty>(UOpcodeProbeHost::StaticClass(), TEXT("PrimaryAssetIdVar"));
    if (TestNotNull(TEXT("primary asset id property"), PrimaryIdProperty))
    {
        UScriptStruct::ICppStructOps* const Ops = PrimaryIdProperty->Struct->GetCppStructOps();
        const bool bCustomImport = Ops != nullptr && Ops->HasImportTextItem();
        TestTrue(TEXT("a custom-import struct is text-editable"), BlueprintInspectorVariableEdit::IsTextEditable(PrimaryIdProperty));
        const bool bBareImported = BlueprintInspectorVariableEdit::Apply(PrimaryIdProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("TestType:TestName"), OutValue, OutError);
        TestTrue(TEXT("the validator follows the engine's registered struct import"), bBareImported == bCustomImport);
        if (bBareImported)
        {
            TestTrue(TEXT("the custom text round-trips"), OutValue.Contains(TEXT("TestType:TestName")));
        }
    }

    const FStructProperty* const GcImportProperty = FindFProperty<FStructProperty>(UOpcodeProbeHost::StaticClass(), TEXT("GcImportVar"));
    if (TestNotNull(TEXT("gc import property"), GcImportProperty))
    {
        OpcodeProbeGcImport::ResetProbe();
        const bool bGcImported = BlueprintInspectorVariableEdit::Apply(GcImportProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("gc"), OutValue, OutError);
        TestTrue(TEXT("the custom import accepts its text"), bGcImported);
        TestTrue(TEXT("the custom import runs under the GC lock"), OpcodeProbeGcImport::SawGarbageCollectionLocked());
    }

    // A struct is editable only when every member is; a map member rejects the whole struct and the
    // message names it, so the client can explain why instead of claiming the type is unsupported.
    const FStructProperty* const UnsupportedProperty = FindFProperty<FStructProperty>(UOpcodeProbeHost::StaticClass(), TEXT("UnsupportedStructVar"));
    if (TestNotNull(TEXT("unsupported struct property"), UnsupportedProperty))
    {
        FString Reason;
        TestFalse(TEXT("a struct with a map member is not text-editable"), BlueprintInspectorVariableEdit::IsTextEditable(UnsupportedProperty, Reason));
        TestTrue(TEXT("the rejection names the offending member"), Reason.Contains(TEXT("MapVar")));
        TestFalse(TEXT("the same reason comes back through Apply"), BlueprintInspectorVariableEdit::Apply(UnsupportedProperty, reinterpret_cast<uint8*>(Host), Host, TEXT("()"), OutValue, OutError));
        TestTrue(TEXT("Apply reports the member reason"), OutError.Contains(TEXT("MapVar")));
    }

    const FDelegateProperty* const DelegateProperty = FindFProperty<FDelegateProperty>(UOpcodeProbeHost::StaticClass(), TEXT("SingleDelegateVar"));
    if (TestNotNull(TEXT("delegate property"), DelegateProperty))
    {
        TestFalse(TEXT("a delegate is not text-editable"), BlueprintInspectorVariableEdit::IsTextEditable(DelegateProperty));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorDapSetVariableTest, "BlueprintInspector.Debugger.DapSetVariable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorDapSetVariableTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FUBlueprintInspectorDap> Dap = FUBlueprintInspectorDap::Create();
    FBlueprintInspectorDapCallbacks Callbacks;

    dap::InitializeRequest InitRequest;
    InitRequest.adapterID = "blueprint-inspector";
    const dap::ResponseOrError<dap::InitializeResponse> InitResponse = Dap->InitializeRequest(InitRequest);
    TestFalse(TEXT("initialize succeeds"), static_cast<bool>(InitResponse.error));
    if (!InitResponse.error)
    {
        TestTrue(TEXT("setVariable is advertised"), InitResponse.response.supportsSetVariable.has_value() && InitResponse.response.supportsSetVariable.value());
    }

    int32 SeenReference = 0;
    FString SeenName;
    FString SeenValue;
    Callbacks.SetVariable = [&SeenReference, &SeenName, &SeenValue](const int32 Reference, const FString& Name, const FString& Value) {
        SeenReference = Reference;
        SeenName = Name;
        SeenValue = Value;
        FBlueprintSetVariableResult Result;
        Result.bSuccess = true;
        Result.Value = TEXT("42");
        Result.Type = TEXT("int32");
        Result.VariablesReference = 9;
        return Result;
    };
    Dap->SetCallbacks(Callbacks);

    dap::SetVariableRequest Request;
    Request.variablesReference = 3;
    Request.name = "IntVar";
    Request.value = "42";
    const dap::ResponseOrError<dap::SetVariableResponse> Response = Dap->SetVariableRequest(Request);
    TestFalse(TEXT("setVariable succeeds"), static_cast<bool>(Response.error));
    if (!Response.error)
    {
        TestEqual(TEXT("the new value is returned"), FString(UTF8_TO_TCHAR(Response.response.value.c_str())), FString(TEXT("42")));
        TestEqual(TEXT("the type is returned"), Response.response.type.has_value() ? FString(UTF8_TO_TCHAR(Response.response.type.value().c_str())) : FString(), FString(TEXT("int32")));
        TestEqual(TEXT("the child reference is returned"), Response.response.variablesReference.has_value() ? static_cast<int32>(Response.response.variablesReference.value()) : 0, 9);
    }
    TestEqual(TEXT("the callback receives the reference"), SeenReference, 3);
    TestEqual(TEXT("the callback receives the name"), SeenName, FString(TEXT("IntVar")));
    TestEqual(TEXT("the callback receives the value"), SeenValue, FString(TEXT("42")));

    Callbacks.SetVariable = [](const int32, const FString&, const FString&) {
        FBlueprintSetVariableResult Result;
        Result.Error = TEXT("the value was rejected");
        return Result;
    };
    Dap->SetCallbacks(Callbacks);
    const dap::ResponseOrError<dap::SetVariableResponse> Failure = Dap->SetVariableRequest(Request);
    TestTrue(TEXT("a failed set is an error"), static_cast<bool>(Failure.error));
    if (Failure.error)
    {
        const FString Message = UTF8_TO_TCHAR(Failure.error.message.c_str());
        TestTrue(TEXT("the error carries the reason"), Message.Contains(TEXT("the value was rejected")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintHotCaptureMetadataTest, "BlueprintInspector.Capture.MetadataRequiresACapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintHotCaptureMetadataTest::RunTest(const FString& Parameters)
{
    FBlueprintClassRegistry Registry;
    FBlueprintHotCapture Capture(Registry);

    // The initial generation is 0 and the cache is empty; without an explicit validity flag a lookup
    // before the first capture would return the default-constructed metadata as if it were current.
    FBlueprintCaptureMetadata Metadata;
    TestFalse(TEXT("a fresh capture has no metadata"), Capture.TryCopyMetadata(Metadata));

    Capture.Start();
    Capture.AcquireMetadata();
    TestTrue(TEXT("the started generation's metadata is cached"), Capture.TryCopyMetadata(Metadata));
    TestFalse(TEXT("the cached metadata names a map or build"), Metadata.Build.IsEmpty() && Metadata.Engine.IsEmpty() && Metadata.Project.IsEmpty());

    FBlueprintCaptureMetadata Again;
    TestTrue(TEXT("the same run reads back again"), Capture.TryCopyMetadata(Again));
    TestTrue(TEXT("the capture time is stamped"), !Metadata.CapturedAt.IsEmpty());
    TestEqual(TEXT("one run reports one capture time"), Metadata.CapturedAt, Again.CapturedAt);

    Capture.Reset();
    TestFalse(TEXT("a reset invalidates the cached metadata"), Capture.TryCopyMetadata(Metadata));

    Capture.Start();
    Capture.TestDuringMetadataBuild = [&Capture]() { Capture.Reset(); };
    const FBlueprintCaptureMetadata Raced = Capture.AcquireMetadata();
    TestTrue(TEXT("a metadata build raced by a reset is discarded"), Raced.Build.IsEmpty());
    FBlueprintCaptureMetadata NotCached;
    TestFalse(TEXT("a discarded metadata build is not cached"), Capture.TryCopyMetadata(NotCached));
    Capture.TestDuringMetadataBuild = nullptr;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPauseCallerAddressTest, "BlueprintInspector.Debugger.CallerFrameAddressIsBounded", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPauseCallerAddressTest::RunTest(const FString& Parameters)
{
    UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestCallerAddress"), BlueprintInspectorTests::MakeReturnValueScript(1));
    UOpcodeProbeHost* const Host = NewObject<UOpcodeProbeHost>();
    if (!TestNotNull(TEXT("function"), Function) || !TestNotNull(TEXT("host"), Host))
    {
        return false;
    }

    FFrame Frame(Host, Function, nullptr);
    Frame.Code = Function->Script.GetData() + 2;
    TestEqual(TEXT("an address inside the script is reported"), FBlueprintPauseSession::GetCallerFrameAddressForTest(Frame), static_cast<int64>(2));

    Frame.Code = Function->Script.GetData() + Function->Script.Num();
    TestEqual(TEXT("the end of the script is a valid address"), FBlueprintPauseSession::GetCallerFrameAddressForTest(Frame), static_cast<int64>(Function->Script.Num()));

    TArray<uint8> OtherBuffer;
    OtherBuffer.Add(0);
    Frame.Code = OtherBuffer.GetData();
    TestEqual(TEXT("an address outside the script is refused"), FBlueprintPauseSession::GetCallerFrameAddressForTest(Frame), static_cast<int64>(INDEX_NONE));

    Frame.Code = nullptr;
    TestEqual(TEXT("a null code pointer is refused"), FBlueprintPauseSession::GetCallerFrameAddressForTest(Frame), static_cast<int64>(INDEX_NONE));

    Frame.Code = Function->Script.GetData();
    Frame.Node = nullptr;
    TestEqual(TEXT("a frame without a function is refused"), FBlueprintPauseSession::GetCallerFrameAddressForTest(Frame), static_cast<int64>(INDEX_NONE));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPausePublishClearRaceTest, "BlueprintInspector.Debugger.PublishFrameDiscardsClearDuringBuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPausePublishClearRaceTest::RunTest(const FString& Parameters)
{
    FBlueprintClassRegistry Registry;
    FBlueprintPauseSession Session(Registry);

    UOpcodeProbeHost* const Host = NewObject<UOpcodeProbeHost>();
    UFunction* const Function = NewObject<UFunction>(UOpcodeProbeHost::StaticClass(), TEXT("BlueprintInspectorTestPublishClearRace"), RF_Transient);
    if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("function"), Function))
    {
        return false;
    }
    TStrongObjectPtr<UOpcodeProbeHost> HostGuard(Host);
    TStrongObjectPtr<UFunction> FunctionGuard(Function);
    Function->Script = BlueprintInspectorTests::MakeReturnValueScript(1);

    FFrame Frame(Host, Function, nullptr);
    Session.PublishFrame(Frame, 0, 0);
    TestEqual(TEXT("the frame is published"), Session.AcquireStackFrames().Num(), 1);

    Session.TestBeforePublish = [&Session]() {
        Session.ClearFrame();
    };
    Session.PublishFrame(Frame, 0, 0);
    TestEqual(TEXT("a clear during the build discards the publish"), Session.AcquireStackFrames().Num(), 0);
    Session.TestBeforePublish = nullptr;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPauseAwareMarshalTest, "BlueprintInspector.Debugger.PauseAwareMarshalRechecksPause", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPauseAwareMarshalTest::RunTest(const FString& Parameters)
{
    UGameInstance* const GameInstance = NewObject<UGameInstance>();
    TStrongObjectPtr<UGameInstance> GameInstanceGuard(GameInstance);
    UBlueprintInspectorSubsystem* const Subsystem = GameInstance ? NewObject<UBlueprintInspectorSubsystem>(GameInstance) : nullptr;
    TStrongObjectPtr<UBlueprintInspectorSubsystem> SubsystemGuard(Subsystem);
    if (!TestNotNull(TEXT("subsystem"), Subsystem))
    {
        return false;
    }

    int32 Value = 0;
    TestFalse(TEXT("staying paused refuses the request"), Subsystem->TestMarshalUnlessPaused(false, Value));
    TestEqual(TEXT("refused work does not run"), Value, 0);

    TestTrue(TEXT("a resume during the cache lookup marshals the request"), Subsystem->TestMarshalUnlessPaused(true, Value));
    TestEqual(TEXT("the marshalled work ran"), Value, 42);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintHotCaptureGenerationTest, "BlueprintInspector.Capture.GenerationMatchesCounters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintHotCaptureGenerationTest::RunTest(const FString& Parameters)
{
    FBlueprintClassRegistry Registry;
    FBlueprintHotCapture Capture(Registry);
    const UFunction* const Function = BlueprintInspectorTests::MakeFunction(TEXT("BlueprintInspectorTestHotCaptureGeneration"), BlueprintInspectorTests::MakeReturnValueScript(1));
    if (!TestNotNull(TEXT("capture function"), Function))
    {
        return false;
    }

    constexpr int32 Iterations = 20000;
    constexpr int32 HitsPerRun = 1;

    std::atomic<bool> bWriterDone{false};
    std::atomic<int32> Violations{0};
    std::atomic<uint32> PublishedGeneration{0};

    std::thread Writer([&]() {
        for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
        {
            const uint32 Generation = Capture.Start().Generation;
            PublishedGeneration.store(Generation);
            for (int32 Hit = 0; Hit < HitsPerRun; ++Hit)
            {
                Capture.CountStep(Function, Hit);
            }
            Capture.Reset();
        }
        bWriterDone.store(true);
    });

    int32 Reports = 0;
    const std::chrono::steady_clock::time_point ReadStart = std::chrono::steady_clock::now();
    while (!bWriterDone.load() && std::chrono::duration<double>(std::chrono::steady_clock::now() - ReadStart).count() < 30.0)
    {
        std::this_thread::yield();
        const uint32 Before = PublishedGeneration.load();
        const FBlueprintHotLinesResult Result = Capture.AcquireHotLines(16);
        const uint32 After = PublishedGeneration.load();
        ++Reports;
        if (Result.Status.TotalHits != 0 && Result.Status.Generation > Before && Result.Status.Generation > After)
        {
            ++Violations;
        }
    }
    Writer.join();

    AddInfo(FString::Printf(TEXT("The reader reported %d time(s)."), Reports));
    TestTrue(TEXT("the writer finished within the reader's deadline"), bWriterDone.load());
    TestEqual(TEXT("no report pairs a newer generation with the previous counters"), Violations.load(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorDapBreakpointAlignmentTest, "BlueprintInspector.Debugger.DapBreakpointResponseAlignment", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorDapBreakpointAlignmentTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FUBlueprintInspectorDap> Dap = FUBlueprintInspectorDap::Create();
    FBlueprintInspectorDapCallbacks Callbacks;
    Callbacks.ApplyBreakpoints = [](const FBlueprintSourceIdentity&, const TArray<FBlueprintBreakpointRequest>& Requests) {
        TGameThreadCallResult<TArray<FBlueprintBreakpointResolution>> Result;
        Result.bTimedOut = false;
        Result.Value.Reserve(Requests.Num());
        for (const FBlueprintBreakpointRequest& Entry : Requests)
        {
            FBlueprintBreakpointResolution Resolution;
            Resolution.RequestedLine = Entry.Line;
            Resolution.Line = Entry.Line;
            Result.Value.Add(Resolution);
        }
        return Result;
    };
    Dap->SetCallbacks(Callbacks);

    dap::SetBreakpointsRequest Request;
    Request.source.path = "/Game/Tests/BP_Alignment";
    dap::SourceBreakpoint First;
    First.line = 10;
    dap::SourceBreakpoint Unsupported;
    Unsupported.line = static_cast<dap::integer>(MAX_int32) + 1;
    dap::SourceBreakpoint Third;
    Third.line = 20;
    Request.breakpoints = dap::array<dap::SourceBreakpoint>{First, Unsupported, Third};

    const dap::ResponseOrError<dap::SetBreakpointsResponse> Response = Dap->SetBreakpointsRequest(Request);
    TestFalse(TEXT("setBreakpoints succeeds"), static_cast<bool>(Response.error));
    if (!Response.error)
    {
        TestEqual(TEXT("the response keeps one entry per requested breakpoint"), static_cast<int32>(Response.response.breakpoints.size()), 3);
        if (Response.response.breakpoints.size() == 3)
        {
            TestTrue(TEXT("the first breakpoint is verified"), Response.response.breakpoints[0].verified);
            TestEqual(TEXT("the first breakpoint keeps its line"), static_cast<int32>(Response.response.breakpoints[0].line.value()), 10);
            TestFalse(TEXT("the unsupported line is not verified"), Response.response.breakpoints[1].verified);
            TestEqual(TEXT("the unsupported line keeps its place and value"), static_cast<dap::integer>(Response.response.breakpoints[1].line.value()), static_cast<dap::integer>(MAX_int32) + 1);
            TestTrue(TEXT("the breakpoint after the rejected one stays aligned"), Response.response.breakpoints[2].verified);
            TestEqual(TEXT("the trailing breakpoint keeps its line"), static_cast<int32>(Response.response.breakpoints[2].line.value()), 20);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintUbergraphRenderingTest, "BlueprintInspector.Rendering.UbergraphEntries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintUbergraphRenderingTest::RunTest(const FString& Parameters)
{
    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    if (!TestNotNull(TEXT("the fixture main class"), Class))
    {
        return false;
    }

    FBlueprintClassRegistry Registry;
    const int32 Handle = Registry.EnsureClassHandle(Class);
    const TOptional<FBlueprintRenderedSource> Rendered = Registry.EnsureClassSource(Handle);
    if (!TestTrue(TEXT("the fixture class renders"), Rendered.IsSet()))
    {
        return false;
    }

    // The event stubs call ExecuteUbergraph_BP_OpcodeDemo_All(76) and (40). Each entry has to render as
    // its own section with the offset in the signature, not as one bare ubergraph from offset 0.
    TestTrue(TEXT("the first ubergraph entry is rendered"), Rendered->Text.Contains(TEXT("ExecuteUbergraph_BP_OpcodeDemo_All_76(")));
    TestTrue(TEXT("the second ubergraph entry is rendered"), Rendered->Text.Contains(TEXT("ExecuteUbergraph_BP_OpcodeDemo_All_40(")));
    TestFalse(TEXT("the bare ubergraph signature is not rendered"), Rendered->Text.Contains(TEXT("ExecuteUbergraph_BP_OpcodeDemo_All(int32 EntryPoint)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintStepDecisionTest, "BlueprintInspector.Debugger.StepDecision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintStepDecisionTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("an operand address never stops a step"), ShouldStopForStep(EDebuggingMode::StepInto, 1, 0, false));

    TestTrue(TEXT("step into stops in the same frame"), ShouldStopForStep(EDebuggingMode::StepInto, 0, 0, true));
    TestTrue(TEXT("step into stops in a callee"), ShouldStopForStep(EDebuggingMode::StepInto, 1, 0, true));

    TestTrue(TEXT("step over stops at the same depth"), ShouldStopForStep(EDebuggingMode::StepOver, 1, 1, true));
    TestFalse(TEXT("step over skips a callee"), ShouldStopForStep(EDebuggingMode::StepOver, 2, 1, true));
    TestTrue(TEXT("step over stops after returning to the caller"), ShouldStopForStep(EDebuggingMode::StepOver, 0, 1, true));

    TestTrue(TEXT("step out stops in the caller"), ShouldStopForStep(EDebuggingMode::StepOut, 0, 1, true));
    TestFalse(TEXT("step out skips the same frame"), ShouldStopForStep(EDebuggingMode::StepOut, 1, 1, true));
    TestFalse(TEXT("step out skips a deeper callee"), ShouldStopForStep(EDebuggingMode::StepOut, 2, 1, true));

    TestFalse(TEXT("continue never stops on a statement"), ShouldStopForStep(EDebuggingMode::ContinueExecution, 0, 0, true));

    TestTrue(TEXT("a breakpoint at the step start is suppressed"), IsBreakpointAtStepStart(EDebuggingMode::StepOver, true, true, true));
    TestFalse(TEXT("continue keeps the breakpoint"), IsBreakpointAtStepStart(EDebuggingMode::ContinueExecution, true, true, true));
    TestFalse(TEXT("a different address keeps the breakpoint"), IsBreakpointAtStepStart(EDebuggingMode::StepOver, true, true, false));
    TestFalse(TEXT("a different function keeps the breakpoint"), IsBreakpointAtStepStart(EDebuggingMode::StepOver, true, false, true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintLevelScriptUbergraphTest, "BlueprintInspector.Debugger.LevelScriptUbergraphRenders", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintLevelScriptUbergraphTest::RunTest(const FString& Parameters)
{
    UWorld* const World = LoadObject<UWorld>(nullptr, *GetBlueprintTestMapObjectPath());
    if (World == nullptr || World->PersistentLevel == nullptr)
    {
        AddError(FString::Printf(TEXT("Could not load %s (override with -BlueprintInspectorTestMap=)."), *GetBlueprintTestMapPath()));
        return false;
    }

    ULevelScriptBlueprint* const LevelBlueprint = World->PersistentLevel->GetLevelScriptBlueprint(true);
    UClass* const Class = LevelBlueprint ? LevelBlueprint->GeneratedClass.Get() : nullptr;
    if (Class == nullptr)
    {
        AddError(TEXT("The level blueprint has no generated class."));
        return false;
    }

    AddInfo(FString::Printf(TEXT("Level script class: %s"), *Class->GetName()));
    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        UFunction* const Function = *It;
        AddInfo(FString::Printf(TEXT("%s flags=0x%x ubergraph=%d script=%d"), *Function->GetName(), static_cast<int32>(Function->FunctionFlags), Function->HasAllFunctionFlags(FUNC_UbergraphFunction) ? 1 : 0, Function->Script.Num()));

        if (Function->Script.Num() > 0)
        {
            const FBlueprintRenderedSource Rendered = BlueprintInspectorTests::RenderFunctionWithMap(Function);
            const FString FirstLine = Rendered.Text.Left(180).Replace(TEXT("\n"), TEXT(" | "));
            int32 StatementLines = 0;
            for (const FBlueprintLineInfo& Info : Rendered.Map.Lines)
            {
                StatementLines += Info.Kind == EBlueprintLineKind::Statement ? 1 : 0;
            }
            AddInfo(FString::Printf(TEXT("  lines=%d statements=%d map=%d first=%s"), CountRenderedLines(Rendered.Text), StatementLines, Rendered.Map.Lines.Num(), *FirstLine));
        }
    }

    UFunction* const BeginPlay = Class->FindFunctionByName(TEXT("ReceiveBeginPlay"));
    UFunction* Ubergraph = nullptr;
    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (It->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            Ubergraph = *It;
            break;
        }
    }
    if (BeginPlay == nullptr || Ubergraph == nullptr)
    {
        AddError(TEXT("The level script is missing ReceiveBeginPlay or an ExecuteUbergraph function."));
        return false;
    }

    const FString StubText = BlueprintInspectorTests::RenderFunction(BeginPlay);
    const FString Marker = Ubergraph->GetName() + TEXT("(");
    const int32 MarkerPosition = StubText.Find(Marker);
    if (MarkerPosition == INDEX_NONE)
    {
        AddError(FString::Printf(TEXT("ReceiveBeginPlay does not call %s with an entry offset."), *Ubergraph->GetName()));
        return false;
    }

    int32 NumberStart = MarkerPosition + Marker.Len();
    int32 NumberEnd = NumberStart;
    while (NumberEnd < StubText.Len() && FChar::IsDigit(StubText[NumberEnd]))
    {
        ++NumberEnd;
    }
    const int64 EntryOffset = FCString::Atoi64(*StubText.Mid(NumberStart, NumberEnd - NumberStart));

    TestTrue(TEXT("ReceiveBeginPlay names a ubergraph entry"), EntryOffset > 0);

    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Ubergraph, EntryOffset);

    FString UbergraphText;
    FBlueprintSourceMap UbergraphMap;
    FBlueprintScriptVisualizer_Pseudo Visualizer;
    Visualizer.Visualize(Ubergraph, Disassembler, UbergraphText, &UbergraphMap);
    AddInfo(UbergraphText);

    int32 UbergraphStatements = 0;
    for (const FBlueprintLineInfo& Info : UbergraphMap.Lines)
    {
        UbergraphStatements += Info.Kind == EBlueprintLineKind::Statement ? 1 : 0;
    }

    // Any non-empty BeginPlay body is a valid host map: the test verifies that the inspector renders
    // the level-script ubergraph, not one particular node call. The bootstrap's generated body is one
    // possible shape.
    TestTrue(TEXT("the ubergraph entry body renders statements"), UbergraphStatements > 0);

    UFunction* const DoThing = Class->FindFunctionByName(TEXT("DoThing"));
    if (DoThing != nullptr)
    {
        const FBlueprintRenderedSource DoThingRendered = BlueprintInspectorTests::RenderFunctionWithMap(DoThing);
        TArray<FString> DoThingLines;
        DoThingRendered.Text.ParseIntoArrayLines(DoThingLines, false);

        int32 SwitchLine = 0;
        int64 SwitchAddress = INDEX_NONE;
        for (int32 Index = 0; Index < DoThingRendered.Map.Lines.Num(); ++Index)
        {
            const FBlueprintLineInfo& Info = DoThingRendered.Map.Lines[Index];
            if (Info.Kind != EBlueprintLineKind::Statement || !DoThingLines.IsValidIndex(Index))
            {
                continue;
            }

            if (DoThingLines[Index].Contains(TEXT("flow_switchvalue")))
            {
                SwitchLine = Index + 1;
                SwitchAddress = Info.ScriptIndex;
                break;
            }
        }

        if (SwitchLine == 0)
        {
            AddError(TEXT("DoThing no longer renders a flow_switchvalue call line."));
            return false;
        }

        if (!TestEqual(TEXT("the flow_switchvalue line resolves from its own address"), DoThingRendered.Map.FindLine(DoThing, SwitchAddress), SwitchLine))
        {
            return false;
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintTransientFixtureTest, "BlueprintInspector.Fixture.TransientBuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintTransientFixtureTest::RunTest(const FString& Parameters)
{
    UBlueprint* const Main = BlueprintFixtureSource::GetMain();
    UBlueprint* const Structs = BlueprintFixtureSource::GetStructs();
    if (Main == nullptr || Main->GeneratedClass == nullptr)
    {
        AddError(TEXT("The transient main fixture did not build."));
        return false;
    }

    AddInfo(FString::Printf(TEXT("Transient main fixture: graphs=%d status=%d."), Main->FunctionGraphs.Num(), static_cast<int32>(Main->Status)));
    AddInfo(FString::Printf(TEXT("Transient struct fixture: graphs=%d status=%d."), Structs ? Structs->FunctionGraphs.Num() : 0, Structs ? static_cast<int32>(Structs->Status) : -1));

    TestTrue(TEXT("the transient main fixture compiled"), Main->Status != BS_Error);

    for (const FString& Graph : GetBlueprintStructGraphs())
    {
        TestNull(FString::Printf(TEXT("main fixture does not carry %s"), *Graph), Main->GeneratedClass->FindFunctionByName(FName(*Graph)));
    }

    const auto Render = [this](const UFunction* const Function, const TCHAR* const Name, FString& OutText) {
        if (Function == nullptr)
        {
            AddError(FString::Printf(TEXT("The split fixture has no %s function."), Name));
            return;
        }
        OutText = BlueprintInspectorTests::RenderFunctionWithMap(Function).Text;
        AddInfo(FString::Printf(TEXT("%s:\n%s"), Name, *OutText));
    };

    const UFunction* const ConstStruct = Structs && Structs->GeneratedClass ? Structs->GeneratedClass->FindFunctionByName(TEXT("const_struct")) : nullptr;
    FString ConstStructText;
    Render(ConstStruct, TEXT("const_struct"), ConstStructText);

    const UFunction* const Switch = BlueprintFixtureSource::FindFunction(TEXT("flow_switchvalue"));
    FString SwitchText;
    Render(Switch, TEXT("flow_switchvalue"), SwitchText);
    TestTrue(TEXT("flow_switchvalue keeps its select option defaults"), SwitchText.Contains(TEXT("Temp_int_Variable_1 = 2;")) && SwitchText.Contains(TEXT("Temp_int_Variable_2 = 1;")));

    // The struct defaults are the reason for the split. 4.x cannot import the archive's struct
    // literals (the bootstrap skips them), so on those engines the defaults must be absent instead.
    const bool bContainerDefaults = ConstStructText.Contains(TEXT("TArray<int32>{1, 2, 3}"));
    AddInfo(FString::Printf(TEXT("const_struct container defaults present: %s"), bContainerDefaults ? TEXT("yes") : TEXT("no")));
#if BI_ENGINE_AT_LEAST_5
    TestTrue(TEXT("const_struct keeps its container defaults"), bContainerDefaults);
#else
    TestFalse(TEXT("4.x skips the archive's struct literals, so no container defaults"), bContainerDefaults);
#endif
    return true;
}

// The bytecode reader carries no package version of its own, so the width it reads for a constant has to
// match the engine's contract rather than an archive default. Write the operand with the engine's own
// writer (which carries the package file version) and require our reader to consume the same bytes: a
// disagreement here is exactly the desync that crashed GC_Collect_Effect_C.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectorConstantWidthTest, "BlueprintInspector.Disassembler.ConstantWidthMatchesEngineWriter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintInspectorConstantWidthTest::RunTest(const FString& Parameters)
{
    // 4.x exposes the file version as an int32 global; 5.x moved it into FPackageFileVersion.
#if BI_ENGINE_AT_LEAST_5
    const int32 PackageFileVersion = GPackageFileUEVersion.ToValue();
#else
    const int32 PackageFileVersion = GPackageFileUE4Version;
#endif
    AddInfo(FString::Printf(TEXT("package file version=%d sizeof(FTransform)=%d sizeof(FVector)=%d"), PackageFileVersion, static_cast<int32>(sizeof(FTransform)), static_cast<int32>(sizeof(FVector))));

    {
        FTransform Value(FQuat(0.0, 0.0, 0.0, 1.0), FVector(100.0, 200.0, 300.0), FVector(1.0, 1.0, 1.0));
        TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        uint8 Opcode = static_cast<uint8>(EX_TransformConst);
        Writer << Opcode;
        const int32 WrittenStart = Bytes.Num();
        Writer << Value;
        const int32 Written = Bytes.Num() - WrittenStart;

        FBlueprintScriptArchiveReader Reader(Bytes);
        uint8 ReadOpcode = 0;
        Reader << ReadOpcode;
        const int64 ReadStart = Reader.Tell();
        FTransform Parsed;
        Reader << Parsed;
        const int64 Read = Reader.Tell() - ReadStart;

        AddInfo(FString::Printf(TEXT("EX_TransformConst: engine writes %d, reader consumes %lld"), Written, Read));
        TestEqual(TEXT("transform: the reader consumes what the engine writes"), static_cast<int32>(Read), Written);
        TestEqual(TEXT("transform: the opcode round-trips"), static_cast<int32>(ReadOpcode), static_cast<int32>(EX_TransformConst));
        TestTrue(TEXT("transform: the value round-trips"), Parsed.Equals(Value, 1.e-4));
    }

    {
        FVector Value(10.0, 20.0, 30.0);
        TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        uint8 Opcode = static_cast<uint8>(EX_VectorConst);
        Writer << Opcode;
        const int32 WrittenStart = Bytes.Num();
        Writer << Value;
        const int32 Written = Bytes.Num() - WrittenStart;

        FBlueprintScriptArchiveReader Reader(Bytes);
        uint8 ReadOpcode = 0;
        Reader << ReadOpcode;
        const int64 ReadStart = Reader.Tell();
        FVector Parsed;
        Reader << Parsed;
        const int64 Read = Reader.Tell() - ReadStart;

        AddInfo(FString::Printf(TEXT("EX_VectorConst: engine writes %d, reader consumes %lld"), Written, Read));
        TestEqual(TEXT("vector: the reader consumes what the engine writes"), static_cast<int32>(Read), Written);
        TestTrue(TEXT("vector: the value round-trips"), Parsed.Equals(Value, 1.e-4));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArchiveReaderRebindTest, "BlueprintInspector.Disassembly.ArchiveReaderRebind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FArchiveReaderRebindTest::RunTest(const FString& Parameters)
{
    TArray<uint8> First;
    First.Init(0, 4);
    FBlueprintScriptArchiveReader Reader(First);

    uint8 Byte = 0;
    TestTrue(TEXT("the first script reads"), Reader.ReadByte(Byte));
    Reader.SetError();

    TArray<uint8> Second;
    Second.Init(0, 2);
    Reader.Rebind(Second);

    TestEqual(TEXT("the offset resets"), static_cast<int32>(Reader.Tell()), 0);
    TestFalse(TEXT("the error clears"), Reader.IsError());
    TestEqual(TEXT("the length follows the new script"), static_cast<int32>(Reader.Length()), 2);
    TestTrue(TEXT("the new script reads"), Reader.ReadByte(Byte));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructDefaultStringsTest, "BlueprintInspector.Fixture.StructDefaultStrings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStructDefaultStringsTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("a GUID-suffixed member name loses its suffix"), BlueprintStructDefaults::MemberBaseName(TEXT("Inner_3_1A2B3C4D5E6F708192A3B4C5D6E7F809")), FString(TEXT("Inner")));
    TestEqual(TEXT("a plain name ending in digits is kept"), BlueprintStructDefaults::MemberBaseName(TEXT("Slot1")), FString(TEXT("Slot1")));
    TestEqual(TEXT("a non-GUID suffix is kept"), BlueprintStructDefaults::MemberBaseName(TEXT("Inner_3_NoGuidHere")), FString(TEXT("Inner_3_NoGuidHere")));
    TestEqual(TEXT("a name without a suffix is kept"), BlueprintStructDefaults::MemberBaseName(TEXT("Value")), FString(TEXT("Value")));

    TArray<FString> Members;
    TestTrue(TEXT("a two-member literal splits"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=1,B=2)"), Members));
    TestEqual(TEXT("both members are returned"), Members.Num(), 2);
    TestEqual(TEXT("the first member keeps its text"), Members[0], FString(TEXT("A=1")));
    TestEqual(TEXT("the second member keeps its text"), Members[1], FString(TEXT("B=2")));

    TestTrue(TEXT("a nested struct stays one member"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=(B=1,C=2),D=3)"), Members));
    TestEqual(TEXT("the nested literal is not split"), Members.Num(), 2);
    TestEqual(TEXT("the nested member is returned whole"), Members[0], FString(TEXT("A=(B=1,C=2)")));

    TestTrue(TEXT("a comma inside a quoted member is not a separator"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=\"x,y\",B=2)"), Members));
    TestEqual(TEXT("the quoted member stays one member"), Members.Num(), 2);
    TestEqual(TEXT("the quoted member keeps its quotes and comma"), Members[0], FString(TEXT("A=\"x,y\"")));

    TestTrue(TEXT("an escaped quote does not end the member"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=\"x\\\"y\",B=2)"), Members));
    TestEqual(TEXT("the escaped member stays one member"), Members.Num(), 2);
    TestEqual(TEXT("the escaped quote is kept"), Members[0], FString(TEXT("A=\"x\\\"y\"")));

    TestTrue(TEXT("an empty literal splits into one empty member"), BlueprintStructDefaults::SplitStructMembers(TEXT("()"), Members));
    TestEqual(TEXT("the empty literal yields one member"), Members.Num(), 1);
    TestTrue(TEXT("the member is empty"), Members[0].IsEmpty());

    Members.Add(TEXT("stale"));
    TestFalse(TEXT("a stray closing parenthesis is refused"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=1))"), Members));
    TestTrue(TEXT("a refused literal leaves no members"), Members.Num() == 0);

    TestFalse(TEXT("an unterminated quote is refused"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=\"x)"), Members));
    TestTrue(TEXT("the unterminated literal leaves no members"), Members.Num() == 0);

    TestFalse(TEXT("an unbalanced literal is refused"), BlueprintStructDefaults::SplitStructMembers(TEXT("(A=(B=1)"), Members));
    TestFalse(TEXT("a literal without parentheses is refused"), BlueprintStructDefaults::SplitStructMembers(TEXT("A=1"), Members));
    TestFalse(TEXT("an empty string is refused"), BlueprintStructDefaults::SplitStructMembers(FString(), Members));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructDefaultMapKeyTest, "BlueprintInspector.Fixture.StructDefaultMapKey", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStructDefaultMapKeyTest::RunTest(const FString& Parameters)
{
    const FString Archive = TEXT("(Entries=(((X_0_1A2B3C4D5E6F708192A3B4C5D6E7F809=1.0,Y_0_1A2B3C4D5E6F708192A3B4C5D6E7F809=2.0,Z_0_1A2B3C4D5E6F708192A3B4C5D6E7F809=3.0),5)))");
    const FString Rewritten = BlueprintStructDefaults::RewriteStructDefault(FBlueprintStructDefaultsMapHolder::StaticStruct(), Archive);
    TestFalse(TEXT("the struct key's archive names are rewritten"), Rewritten.Contains(TEXT("_0_")));
    TestTrue(TEXT("the key struct's X member survives"), Rewritten.Contains(TEXT("X=")));
    TestTrue(TEXT("the map value survives"), Rewritten.Contains(TEXT("5")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
