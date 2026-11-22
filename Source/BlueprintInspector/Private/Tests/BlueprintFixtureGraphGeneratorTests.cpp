
#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "BlueprintInspectorPort.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionTerminator.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "BlueprintFixtureRepair.h"
#include "BlueprintOpcodeFixtureBuilder.h"
#include "BlueprintTestConfiguration.h"

namespace BlueprintFixtureGraphGenerator
{
const TCHAR* NonFixtureGraphName = TEXT("UserConstructionScript");

FString GetArchiveDir()
{
    return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("BlueprintInspector/Content/Tests/OpcodeShowcase/Graphs"));
}

bool AreToolsEnabled()
{
    return FParse::Param(FCommandLine::Get(), TEXT("BlueprintFixtureTools"));
}

UBlueprint* LoadFixture()
{
    return BlueprintFixtureRepair::LoadRepairedFixture();
}

UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FName GraphName)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == GraphName)
        {
            return Graph;
        }
    }
    return nullptr;
}

// The exported text has one "Begin Object" per node, so the archive's own node count is what separates
// a complete import from a truncated one (a partial import still reports a non-zero imported set).
int32 CountArchivedNodes(const FString& Text)
{
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);
    int32 Count = 0;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("Begin Object Class=")))
        {
            ++Count;
        }
    }
    return Count;
}

void ClearGraph(UBlueprint* Blueprint, UEdGraph* Graph);

// Imports the archive into a throwaway graph so the outcome can be checked before the cached fixture
// is touched. Returns the imported node count, or INDEX_NONE when the text cannot be imported at all.
// The probe is outside the blueprint's graph lists, so nothing that enumerates or saves the asset can
// see it. Its nodes are removed and the graph is renamed out of the blueprint before this returns.
int32 StageImport(UBlueprint* Blueprint, const FString& Text)
{
    if (!Blueprint)
    {
        return INDEX_NONE;
    }

    UEdGraph* const Staging = FBlueprintEditorUtils::CreateNewGraph(Blueprint, NAME_None, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (Staging == nullptr)
    {
        return INDEX_NONE;
    }

    int32 ImportedCount = INDEX_NONE;
    if (FEdGraphUtilities::CanImportNodesFromText(Staging, Text))
    {
        TSet<UEdGraphNode*> Imported;
        FEdGraphUtilities::ImportNodesFromText(Staging, Text, Imported);
        ImportedCount = Imported.Num();
    }

    ClearGraph(Blueprint, Staging);
    FBlueprintEditorUtils::RemoveGraph(Blueprint, Staging, EGraphRemoveFlags::None);

    return ImportedCount;
}

// Removes every node from Graph, leaving the graph object itself in place.
void ClearGraph(UBlueprint* Blueprint, UEdGraph* Graph)
{
    if (!Graph)
    {
        return;
    }

    TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
    for (UEdGraphNode* const Node : ExistingNodes)
    {
        if (Node != nullptr)
        {
            FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
        }
    }
}

bool IsFunctionTerminator(const UEdGraphNode* Node)
{
    return Node && Node->IsA<UK2Node_FunctionTerminator>();
}

void CollectGraphNodes(UEdGraph* Graph, TSet<UObject*>& OutNodes)
{
    OutNodes.Reset();
    if (!Graph)
    {
        return;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            OutNodes.Add(Node);
        }
    }
}

int32 CountExportedNodes(const FString& Text)
{
    return CountArchivedNodes(Text);
}

bool SaveBlueprint(UBlueprint* Blueprint, FString& OutError)
{
    UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
    if (!Package)
    {
        OutError = TEXT("Blueprint has no package.");
        return false;
    }

    Package->MarkPackageDirty();
    const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

#if BI_ENGINE_AT_LEAST_5
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;

    if (!UPackage::SavePackage(Package, Blueprint, *FileName, SaveArgs))
#else
    // FSavePackageArgs arrived after 4.25; the older SavePackage takes the flags directly.
    if (!UPackage::SavePackage(Package, Blueprint, RF_Public | RF_Standalone, *FileName, GError, nullptr, false, true, SAVE_NoError))
#endif
    {
        OutError = FString::Printf(TEXT("SavePackage failed for %s."), *FileName);
        return false;
    }
    return true;
}

int32 CountFunctions(const UClass* Class)
{
    int32 Count = 0;
    if (Class)
    {
        for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            ++Count;
        }
    }
    return Count;
}

int32 CountTerminators(const UEdGraph* Graph)
{
    int32 Count = 0;
    if (Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (IsFunctionTerminator(Node))
            {
                ++Count;
            }
        }
    }
    return Count;
}

FString ValidateScalarDefault(const UEdGraphPin& Pin)
{
    if (Pin.DefaultValue.IsEmpty())
    {
        return FString();
    }

    const FName& Category = Pin.PinType.PinCategory;
    if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64)
    {
        if (!FDefaultValueHelper::IsStringValidInteger(Pin.DefaultValue))
        {
            return FString::Printf(TEXT("'%s' is not a valid integer"), *Pin.DefaultValue);
        }
    }
#if BI_ENGINE_AT_LEAST_5
    else if (Category == UEdGraphSchema_K2::PC_Real)
#else
    else if (Category == UEdGraphSchema_K2::PC_Float)
#endif
    {
        if (!FDefaultValueHelper::IsStringValidFloat(Pin.DefaultValue))
        {
            return FString::Printf(TEXT("'%s' is not a valid float"), *Pin.DefaultValue);
        }
    }

    return FString();
}

bool ValidatePinDefaults(UBlueprint* Blueprint, TArray<FString>& OutErrors)
{
    OutErrors.Reset();
    if (!Blueprint)
    {
        OutErrors.Add(TEXT("No Blueprint loaded."));
        return false;
    }

    const auto ValidateGraph = [&OutErrors](UEdGraph* const Graph) {
        if (!Graph || Graph->GetFName() == NonFixtureGraphName)
        {
            return;
        }

        const UEdGraphSchema_K2* const Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (!Schema)
        {
            OutErrors.Add(FString::Printf(TEXT("%s: graph has no K2 schema"), *Graph->GetName()));
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->bHidden || Pin->bDefaultValueIsIgnored || Pin->LinkedTo.Num() > 0 || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->PinType.IsArray() || Pin->PinType.IsSet() || Pin->PinType.IsMap() || Pin->PinType.bIsReference)
                {
                    continue;
                }

                const FString Error = Schema->IsPinDefaultValid(Pin, Pin->DefaultValue, Pin->DefaultObject, Pin->DefaultTextValue);
                const FString ScalarError = Error.IsEmpty() ? ValidateScalarDefault(*Pin) : FString();
                const FString& EffectiveError = Error.IsEmpty() ? ScalarError : Error;
                if (!EffectiveError.IsEmpty())
                {
                    OutErrors.Add(FString::Printf(TEXT("%s: %s.%s: %s"), *Graph->GetName(), *Node->GetName(), *Pin->PinName.ToString(), *EffectiveError));
                }
            }
        }
    };

    // Authored graphs live in both containers: the flow fixtures are function graphs, but the EventGraph
    // rows are ubergraph pages. Validating only FunctionGraphs let an invalid ubergraph default reach
    // the compiler and be saved.
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        ValidateGraph(Graph);
    }
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        ValidateGraph(Graph);
    }

    return OutErrors.Num() == 0;
}
} // namespace BlueprintFixtureGraphGenerator

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFixtureGraphExportTest, "BlueprintInspector.Fixture.ExportGraphArchive", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintFixtureGraphExportTest::RunTest(const FString& Parameters)
{
    if (!BlueprintFixtureGraphGenerator::AreToolsEnabled())
    {
        AddInfo(TEXT("Fixture graph tools skipped; pass -BlueprintFixtureTools to run them."));
        return true;
    }

    UBlueprint* const Blueprint = BlueprintFixtureGraphGenerator::LoadFixture();
    if (!Blueprint)
    {
        AddError(TEXT("Fixture blueprint BP_OpcodeDemo_All was not loaded."));
        return false;
    }

    const FString ArchiveDir = BlueprintFixtureGraphGenerator::GetArchiveDir();
    IFileManager::Get().MakeDirectory(*ArchiveDir, true);

    FBlueprintOpcodeFixtureBuilder Builder(*Blueprint);

    struct FStagedGraph
    {
        FString FileName;
        FString Text;
    };
    TArray<FStagedGraph> Staged;
    TSet<FString> PreservedFiles;

    int32 Exported = 0;
    int32 Empty = 0;
    int32 TotalNodes = 0;
    int32 Terminators = 0;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph || Graph->GetFName() == BlueprintFixtureGraphGenerator::NonFixtureGraphName)
        {
            continue;
        }

        int32 BodyNodeCount = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && !BlueprintFixtureGraphGenerator::IsFunctionTerminator(Node))
            {
                ++BodyNodeCount;
            }
        }
        if (BodyNodeCount == 0)
        {
            ++Empty;
            PreservedFiles.Add(FString::Printf(TEXT("%s.txt"), *Graph->GetName()));
            continue;
        }

        TotalNodes += Graph->Nodes.Num();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (BlueprintFixtureGraphGenerator::IsFunctionTerminator(Node))
            {
                ++Terminators;
            }
        }

        Builder.CompactLayout(*Graph);

        TSet<UObject*> Nodes;
        BlueprintFixtureGraphGenerator::CollectGraphNodes(Graph, Nodes);

        FString Text;
        FEdGraphUtilities::ExportNodesToText(Nodes, Text);
        if (Text.IsEmpty() || !FEdGraphUtilities::CanImportNodesFromText(Graph, Text) || BlueprintFixtureGraphGenerator::CountExportedNodes(Text) < Graph->Nodes.Num())
        {
            AddError(FString::Printf(TEXT("%s did not export a complete graph; the existing archive was left untouched."), *Graph->GetName()));
            return false;
        }

        Staged.Add({FString::Printf(TEXT("%s.txt"), *Graph->GetName()), Text});
        ++Exported;
    }

    if (Exported == 0)
    {
        AddError(TEXT("No function graph was exported; the existing archive was left untouched."));
        return false;
    }

    TArray<FString> TempPaths;
    for (const FStagedGraph& Entry : Staged)
    {
        const FString TempPath = FPaths::Combine(ArchiveDir, Entry.FileName + TEXT(".tmp"));
        if (!FFileHelper::SaveStringToFile(Entry.Text, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8))
        {
            for (const FString& StaleTemp : TempPaths)
            {
                IFileManager::Get().Delete(*StaleTemp);
            }
            AddError(FString::Printf(TEXT("Could not stage %s; the existing archive was left untouched."), *Entry.FileName));
            return false;
        }
        TempPaths.Add(TempPath);
    }

    // Replace each staged file in place, moving the old file aside first. A failure part-way then
    // restores every destination from its backup instead of leaving a mixed archive; the whole
    // archive is never removed, so there is always an old copy to fall back on.
    struct FArchiveEntry
    {
        FString Destination;
        FString Backup;
        bool bHadBackup = false;
    };
    TArray<FArchiveEntry> Replaced;
    TSet<FString> NewFiles;

    const auto RollBack = [&Replaced, &TempPaths]() {
        for (int32 Index = Replaced.Num() - 1; Index >= 0; --Index)
        {
            const FArchiveEntry& Entry = Replaced[Index];
            IFileManager::Get().Delete(*Entry.Destination);
            if (Entry.bHadBackup)
            {
                IFileManager::Get().Move(*Entry.Destination, *Entry.Backup, true);
            }
        }
        for (const FString& StaleTemp : TempPaths)
        {
            IFileManager::Get().Delete(*StaleTemp);
        }
    };

    for (int32 Index = 0; Index < Staged.Num(); ++Index)
    {
        FArchiveEntry Entry;
        Entry.Destination = FPaths::Combine(ArchiveDir, Staged[Index].FileName);
        Entry.Backup = Entry.Destination + TEXT(".bak");
        NewFiles.Add(Staged[Index].FileName);

        if (IFileManager::Get().FileExists(*Entry.Destination))
        {
            if (!IFileManager::Get().Move(*Entry.Backup, *Entry.Destination, true))
            {
                AddError(FString::Printf(TEXT("Could not back up %s; the previous archive was restored."), *Staged[Index].FileName));
                RollBack();
                return false;
            }
            Entry.bHadBackup = true;
        }

        Replaced.Add(Entry);
        if (!IFileManager::Get().Move(*Entry.Destination, *TempPaths[Index], true))
        {
            AddError(FString::Printf(TEXT("Could not move %s into the archive; the previous archive was restored."), *Staged[Index].FileName));
            RollBack();
            return false;
        }
    }

    for (const FArchiveEntry& Entry : Replaced)
    {
        if (Entry.bHadBackup)
        {
            IFileManager::Get().Delete(*Entry.Backup);
        }
    }

    // Only once every new file is in place are the graphs that no longer exist removed.
    TArray<FString> ExistingFiles;
    IFileManager::Get().FindFiles(ExistingFiles, *FPaths::Combine(ArchiveDir, TEXT("*.txt")), true, false);
    for (const FString& ExistingFile : ExistingFiles)
    {
        if (!NewFiles.Contains(ExistingFile) && !PreservedFiles.Contains(ExistingFile) && !IFileManager::Get().Delete(*FPaths::Combine(ArchiveDir, ExistingFile)))
        {
            AddError(FString::Printf(TEXT("Could not delete the stale archive file %s; the next regeneration would recreate the graph."), *ExistingFile));
        }
    }

    AddInfo(FString::Printf(TEXT("Exported %d function graph(s) to %s; %d empty graph(s); %d node(s), %d function terminator(s)."), Exported, *ArchiveDir, Empty, TotalNodes, Terminators));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFixtureGraphRegenerateTest, "BlueprintInspector.Fixture.RegenerateFromGraphArchive", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintFixtureGraphRegenerateTest::RunTest(const FString& Parameters)
{
    if (!BlueprintFixtureGraphGenerator::AreToolsEnabled())
    {
        AddInfo(TEXT("Fixture graph tools skipped; pass -BlueprintFixtureTools to run them."));
        return true;
    }

    UBlueprint* const Blueprint = BlueprintFixtureGraphGenerator::LoadFixture();
    if (!Blueprint)
    {
        AddError(TEXT("Fixture blueprint BP_OpcodeDemo_All was not loaded."));
        return false;
    }

    if (!CanSuppressAccessViolation())
    {
        AddError(FString::Printf(TEXT("Kismet.bCanSuppressAccessViolation is false; pass %s. The asset was not compiled or saved."), FailSilentCommandLineSwitch()));
        return false;
    }

    const FString ArchiveDir = BlueprintFixtureGraphGenerator::GetArchiveDir();
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(ArchiveDir, TEXT("*.txt")), true, false);
    if (Files.Num() == 0)
    {
        AddError(FString::Printf(TEXT("No graph archive found under %s."), *ArchiveDir));
        return false;
    }

    FBlueprintOpcodeFixtureBuilder Builder(*Blueprint);
    int32 Rebuilt = 0;
    int32 Failed = 0;

    struct FRebuiltGraph
    {
        UEdGraph* Graph = nullptr;
        FString Backup;
        TSet<UObject*> BackupNodes;
        bool bCreated = false;
    };
    TArray<FRebuiltGraph> RebuiltGraphs;

    const int32 FunctionsBefore = BlueprintFixtureGraphGenerator::CountFunctions(Blueprint->GeneratedClass);
    AddInfo(FString::Printf(TEXT("Before regeneration: graphs=%d functions=%d archive files=%d."), Blueprint->FunctionGraphs.Num(), FunctionsBefore, Files.Num()));
    for (const FString& File : Files)
    {
        const FName GraphName(*FPaths::GetBaseFilename(File));
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(ArchiveDir, File)))
        {
            AddError(FString::Printf(TEXT("Could not read %s."), *File));
            ++Failed;
            continue;
        }

        // A partial import (terminators only, body lost) still reports a non-zero imported set, so the
        // archive's own node count decides whether the import is complete.
        const int32 ExpectedNodes = BlueprintFixtureGraphGenerator::CountArchivedNodes(Text);

        // Stage the import in a throwaway graph first: the cached fixture must not be left half-rebuilt
        // by an archive this engine cannot load, because the tests that follow use it.
        const int32 StagedNodes = BlueprintFixtureGraphGenerator::StageImport(Blueprint, Text);
        if (StagedNodes == INDEX_NONE)
        {
            AddError(FString::Printf(TEXT("%s cannot be imported into %s."), *File, *GraphName.ToString()));
            ++Failed;
            continue;
        }
        if (StagedNodes == 0 || StagedNodes < ExpectedNodes)
        {
            AddError(FString::Printf(TEXT("%s imported %d of %d node(s) into %s; the graph was left unchanged."), *File, StagedNodes, ExpectedNodes, *GraphName.ToString()));
            ++Failed;
            continue;
        }

        // The archive imports completely, so rebuild the live graph from it. The graph's old contents
        // are kept as text so a second import that disagrees with the staged one can be rolled back.
        UEdGraph* Graph = BlueprintFixtureGraphGenerator::FindFunctionGraph(Blueprint, GraphName);
        const bool bCreatedGraph = Graph == nullptr;
        if (bCreatedGraph)
        {
            Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, GraphName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (Graph == nullptr)
            {
                AddError(FString::Printf(TEXT("%s graph could not be created; the archive file was skipped."), *GraphName.ToString()));
                ++Failed;
                continue;
            }
            Blueprint->FunctionGraphs.Add(Graph);
        }

        FString Backup;
        TSet<UObject*> BackupNodes;
        for (UEdGraphNode* const Node : Graph->Nodes)
        {
            if (Node != nullptr)
            {
                BackupNodes.Add(Node);
            }
        }
        if (BackupNodes.Num() > 0)
        {
            FEdGraphUtilities::ExportNodesToText(BackupNodes, Backup);
        }

        FRebuiltGraph Record;
        Record.Graph = Graph;
        Record.Backup = Backup;
        Record.BackupNodes = BackupNodes;
        Record.bCreated = bCreatedGraph;
        RebuiltGraphs.Add(MoveTemp(Record));

        BlueprintFixtureGraphGenerator::ClearGraph(Blueprint, Graph);

        TSet<UEdGraphNode*> Imported;
        FEdGraphUtilities::ImportNodesFromText(Graph, Text, Imported);
        if (Imported.Num() < ExpectedNodes)
        {
            AddError(FString::Printf(TEXT("%s imported %d of %d node(s) into %s."), *File, Imported.Num(), ExpectedNodes, *GraphName.ToString()));
            ++Failed;
            continue;
        }

        Builder.CompactLayout(*Graph);
        ++Rebuilt;
    }

    bool bRebuildCommitted = false;
    ON_SCOPE_EXIT
    {
        if (bRebuildCommitted)
        {
            return;
        }
        for (int32 Index = RebuiltGraphs.Num() - 1; Index >= 0; --Index)
        {
            const FRebuiltGraph& Entry = RebuiltGraphs[Index];
            if (Entry.Graph == nullptr)
            {
                continue;
            }

            BlueprintFixtureGraphGenerator::ClearGraph(Blueprint, Entry.Graph);
            if (Entry.bCreated)
            {
                FBlueprintEditorUtils::RemoveGraph(Blueprint, Entry.Graph, EGraphRemoveFlags::Default);
                continue;
            }

            if (Entry.BackupNodes.Num() == 0)
            {
                continue;
            }

            if (Entry.Backup.IsEmpty())
            {
                AddError(FString::Printf(TEXT("%s could not be backed up before its rebuild; it was left cleared."), *Entry.Graph->GetName()));
                continue;
            }

            TSet<UEdGraphNode*> Restored;
            FEdGraphUtilities::ImportNodesFromText(Entry.Graph, Entry.Backup, Restored);
            if (Restored.Num() < Entry.BackupNodes.Num())
            {
                AddError(FString::Printf(TEXT("%s restored only %d of %d node(s) from its backup after a failed rebuild."), *Entry.Graph->GetName(), Restored.Num(), Entry.BackupNodes.Num()));
            }
        }
    };

    if (Failed > 0)
    {
        AddError(TEXT("Graph regeneration failed; the asset was not compiled or saved."));
        return false;
    }

    // The archive is the source of truth: a function graph with no archive file would otherwise be
    // compiled and saved as part of the "regenerated" fixture. The construction script is not an
    // archived fixture graph, so it stays.
    TSet<FName> ArchivedNames;
    for (const FString& File : Files)
    {
        ArchivedNames.Add(FName(*FPaths::GetBaseFilename(File)));
    }

    TArray<UEdGraph*> StaleGraphs;
    for (UEdGraph* const Graph : Blueprint->FunctionGraphs)
    {
        if (Graph != nullptr && Graph->GetFName() != BlueprintFixtureGraphGenerator::NonFixtureGraphName && !ArchivedNames.Contains(Graph->GetFName()))
        {
            StaleGraphs.Add(Graph);
        }
    }

    struct FStaleGraphBackup
    {
        FName Name;
        FString Text;
        int32 NodeCount = 0;
    };
    TArray<FStaleGraphBackup> StaleBackups;
    for (UEdGraph* const Graph : StaleGraphs)
    {
        FStaleGraphBackup Backup;
        Backup.Name = Graph->GetFName();

        TSet<UObject*> Nodes;
        BlueprintFixtureGraphGenerator::CollectGraphNodes(Graph, Nodes);
        Backup.NodeCount = Nodes.Num();
        FEdGraphUtilities::ExportNodesToText(Nodes, Backup.Text);
        StaleBackups.Add(MoveTemp(Backup));
    }

    // A stale graph is removed only for a build that commits. Any later failure restores it, so a
    // half-finished regeneration cannot leave the cached fixture missing graphs the next test needs.
    bool bStaleRemovalCommitted = false;
    ON_SCOPE_EXIT
    {
        if (bStaleRemovalCommitted)
        {
            return;
        }
        for (const FStaleGraphBackup& Backup : StaleBackups)
        {
            UEdGraph* const RestoredGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, Backup.Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (RestoredGraph == nullptr)
            {
                AddError(FString::Printf(TEXT("%s could not be recreated after a failed regeneration."), *Backup.Name.ToString()));
                continue;
            }
            Blueprint->FunctionGraphs.Add(RestoredGraph);
            if (Backup.Text.IsEmpty())
            {
                continue;
            }

            TSet<UEdGraphNode*> Restored;
            FEdGraphUtilities::ImportNodesFromText(RestoredGraph, Backup.Text, Restored);
            if (Restored.Num() < Backup.NodeCount)
            {
                AddError(FString::Printf(TEXT("%s restored only %d of %d node(s) after a failed regeneration."), *Backup.Name.ToString(), Restored.Num(), Backup.NodeCount));
            }
        }
    };

    for (UEdGraph* const Graph : StaleGraphs)
    {
        AddInfo(FString::Printf(TEXT("Removing stale function graph %s: no archive file."), *Graph->GetName()));
        FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
    }

    if (!Builder.EnsureWeakVariable())
    {
        AddError(TEXT("The WeakObjVar variable could not be made a weak object pointer; the asset was not compiled or saved."));
        return false;
    }

    TArray<FString> PinErrors;
    if (!BlueprintFixtureGraphGenerator::ValidatePinDefaults(Blueprint, PinErrors))
    {
        for (const FString& Error : PinErrors)
        {
            AddError(FString::Printf(TEXT("Invalid pin default: %s"), *Error));
        }
        AddError(TEXT("Graph regeneration aborted before compile; the asset was not saved."));
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    const int32 FunctionsAfter = BlueprintFixtureGraphGenerator::CountFunctions(Blueprint->GeneratedClass);
    AddInfo(FString::Printf(TEXT("After regeneration: status=%d graphs=%d functions=%d."), static_cast<int32>(Blueprint->Status), Blueprint->FunctionGraphs.Num(), FunctionsAfter));

    if (Blueprint->Status == BS_Error)
    {
        AddError(TEXT("Blueprint failed to compile after graph regeneration; the asset was not saved."));
        return false;
    }

    const int32 ExpectedFunctionsAfter = FMath::Max(0, FunctionsBefore - StaleGraphs.Num());
    if (FunctionsAfter < ExpectedFunctionsAfter)
    {
        AddError(FString::Printf(TEXT("Regeneration dropped functions (%d -> %d, %d stale graph(s) intentionally removed); the asset was not saved."), FunctionsBefore, FunctionsAfter, StaleGraphs.Num()));
        return false;
    }

    int32 ShortArchivedScripts = 0;
    for (const FString& File : Files)
    {
        const FName FunctionName(*FPaths::GetBaseFilename(File));
        UFunction* const Function = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->FindFunctionByName(FunctionName) : nullptr;
        if (!Function || Function->Script.Num() < 8)
        {
            AddError(FString::Printf(TEXT("%s compiled to %d script byte(s); the asset was not saved."), *FunctionName.ToString(), Function ? Function->Script.Num() : 0));
            ++ShortArchivedScripts;
        }
    }
    if (ShortArchivedScripts > 0)
    {
        return false;
    }

    FString SaveError;
    if (!BlueprintFixtureGraphGenerator::SaveBlueprint(Blueprint, SaveError))
    {
        AddError(SaveError);
        return false;
    }

    bStaleRemovalCommitted = true;
    bRebuildCommitted = true;

    AddInfo(FString::Printf(TEXT("Regenerated %d function graph(s); %d failed; read %d archive file(s)."), Rebuilt, Failed, Files.Num()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFixtureGraphDiagnosticTest, "BlueprintInspector.Fixture.RegenerateOneGraphDiagnostic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintFixtureGraphDiagnosticTest::RunTest(const FString& Parameters)
{
    if (!BlueprintFixtureGraphGenerator::AreToolsEnabled())
    {
        AddInfo(TEXT("Fixture graph tools skipped; pass -BlueprintFixtureTools to run them."));
        return true;
    }

    UBlueprint* const Blueprint = BlueprintFixtureGraphGenerator::LoadFixture();
    if (!Blueprint)
    {
        AddError(TEXT("Fixture blueprint BP_OpcodeDemo_All was not loaded."));
        return false;
    }

    // This test compiles the shared in-memory fixture, so it needs the same suppression guard as the
    // other compiling tests before it can hit the compiler's access-violation path.
    if (!CanSuppressAccessViolation())
    {
        AddError(FString::Printf(TEXT("Kismet.bCanSuppressAccessViolation is false; pass %s."), FailSilentCommandLineSwitch()));
        return false;
    }

    const FName GraphName(TEXT("let_int"));
    UEdGraph* const Graph = BlueprintFixtureGraphGenerator::FindFunctionGraph(Blueprint, GraphName);
    if (!Graph)
    {
        AddError(TEXT("let_int graph was not found."));
        return false;
    }

    TSet<UObject*> AllNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            AllNodes.Add(Node);
        }
    }

    FString Text;
    FEdGraphUtilities::ExportNodesToText(AllNodes, Text);
    AddInfo(FString::Printf(TEXT("Export: nodes=%d bytes=%d entryInText=%s resultInText=%s."), AllNodes.Num(), Text.Len(), Text.Contains(TEXT("K2Node_FunctionEntry")) ? TEXT("yes") : TEXT("no"), Text.Contains(TEXT("K2Node_FunctionResult")) ? TEXT("yes") : TEXT("no")));

    if (Text.IsEmpty() || !FEdGraphUtilities::CanImportNodesFromText(Graph, Text) || BlueprintFixtureGraphGenerator::CountExportedNodes(Text) < AllNodes.Num())
    {
        AddError(TEXT("let_int did not export a complete graph; the shared fixture was left untouched."));
        return false;
    }

    const auto RestoreOriginal = [Blueprint, Graph, &Text]() {
        TArray<UEdGraphNode*> BrokenNodes = Graph->Nodes;
        for (UEdGraphNode* Node : BrokenNodes)
        {
            if (Node)
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            }
        }
        TSet<UEdGraphNode*> Restored;
        FEdGraphUtilities::ImportNodesFromText(Graph, Text, Restored);
    };

    AddInfo(FString::Printf(TEXT("Before: graphs=%d functions=%d nodes=%d terminators=%d."), Blueprint->FunctionGraphs.Num(), BlueprintFixtureGraphGenerator::CountFunctions(Blueprint->GeneratedClass), Graph->Nodes.Num(), BlueprintFixtureGraphGenerator::CountTerminators(Graph)));

    TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
    for (UEdGraphNode* Node : ExistingNodes)
    {
        if (Node)
        {
            FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
        }
    }

    AddInfo(FString::Printf(TEXT("After removal: nodes=%d terminators=%d."), Graph->Nodes.Num(), BlueprintFixtureGraphGenerator::CountTerminators(Graph)));

    AddInfo(FString::Printf(TEXT("Can import: %s."), FEdGraphUtilities::CanImportNodesFromText(Graph, Text) ? TEXT("yes") : TEXT("no")));

    TSet<UEdGraphNode*> Imported;
    FEdGraphUtilities::ImportNodesFromText(Graph, Text, Imported);
    AddInfo(FString::Printf(TEXT("After import: nodes=%d terminators=%d imported=%d."), Graph->Nodes.Num(), BlueprintFixtureGraphGenerator::CountTerminators(Graph), Imported.Num()));

    if (Imported.Num() != AllNodes.Num())
    {
        RestoreOriginal();
        AddError(FString::Printf(TEXT("let_int imported %d of %d nodes; the shared fixture was restored."), Imported.Num(), AllNodes.Num()));
        return false;
    }

    TArray<FString> PinErrors;
    if (!BlueprintFixtureGraphGenerator::ValidatePinDefaults(Blueprint, PinErrors))
    {
        for (const FString& Error : PinErrors)
        {
            AddError(FString::Printf(TEXT("Invalid pin default: %s"), *Error));
        }
        AddError(TEXT("Graph diagnostic aborted before compile; the asset was not saved."));
        RestoreOriginal();
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    AddInfo(FString::Printf(TEXT("After compile: status=%d graphs=%d functions=%d nodes=%d terminators=%d let_int=%s script=%d."), static_cast<int32>(Blueprint->Status), Blueprint->FunctionGraphs.Num(), BlueprintFixtureGraphGenerator::CountFunctions(Blueprint->GeneratedClass), Graph->Nodes.Num(), BlueprintFixtureGraphGenerator::CountTerminators(Graph), Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindFunctionByName(GraphName) ? TEXT("yes") : TEXT("no"), Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindFunctionByName(GraphName) ? Blueprint->GeneratedClass->FindFunctionByName(GraphName)->Script.Num() : 0));

    const UFunction* const CompiledFunction = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->FindFunctionByName(GraphName) : nullptr;
    TestTrue(TEXT("the diagnostic graph compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
    TestNotNull(TEXT("the diagnostic function exists after compile"), CompiledFunction);
    if (CompiledFunction != nullptr)
    {
        TestTrue(TEXT("the diagnostic function has script"), CompiledFunction->Script.Num() > 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFixturePinDefaultsTest, "BlueprintInspector.Fixture.PinDefaultsValidate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintFixturePinDefaultsTest::RunTest(const FString& Parameters)
{
    if (!BlueprintFixtureGraphGenerator::AreToolsEnabled())
    {
        AddInfo(TEXT("Pin default mutation check skipped; pass -BlueprintFixtureTools to run it."));
        return true;
    }

    UBlueprint* const Blueprint = BlueprintFixtureGraphGenerator::LoadFixture();
    if (!Blueprint)
    {
        AddError(TEXT("Fixture blueprint BP_OpcodeDemo_All was not loaded."));
        return false;
    }

    TArray<FString> Errors;
    const bool bFixtureValid = BlueprintFixtureGraphGenerator::ValidatePinDefaults(Blueprint, Errors);
    for (const FString& Error : Errors)
    {
        AddInfo(Error);
    }
    TestTrue(TEXT("the fixture's unlinked pin defaults validate"), bFixtureValid);

    const auto FindLinkedIntPin = [](UEdGraph* Graph) -> UEdGraphPin* {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0 && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int && !Pin->bHidden && !Pin->PinType.IsArray() && !Pin->PinType.IsSet() && !Pin->PinType.IsMap())
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    };

    UEdGraphPin* IntPin = FindLinkedIntPin(BlueprintFixtureGraphGenerator::FindFunctionGraph(Blueprint, TEXT("flow_branch")));
    if (!IntPin)
    {
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            IntPin = FindLinkedIntPin(Graph);
            if (IntPin)
            {
                break;
            }
        }
    }

    if (!IntPin)
    {
        AddError(TEXT("No linked integer input pin was found to corrupt."));
        return false;
    }

    struct FPinRestore
    {
        UEdGraphPin* Pin = nullptr;
        TArray<UEdGraphPin*> Links;
        FString Value;

        ~FPinRestore()
        {
            if (Pin)
            {
                Pin->DefaultValue = Value;
                Pin->LinkedTo = Links;
                for (UEdGraphPin* const Peer : Links)
                {
                    if (Peer != nullptr)
                    {
                        Peer->LinkedTo.AddUnique(Pin);
                    }
                }
            }
        }
    };
    const FPinRestore Restore{IntPin, IntPin->LinkedTo, IntPin->DefaultValue};

    IntPin->BreakAllPinLinks();
    IntPin->DefaultValue = TEXT("not-an-int");

    const UEdGraphNode* const OwningNode = IntPin->GetOwningNodeUnchecked();
    AddInfo(FString::Printf(TEXT("corrupt pin graph=%s node=%s pin=%s category=%s default=%s links=%d scalar='%s'"), OwningNode && OwningNode->GetGraph() ? *OwningNode->GetGraph()->GetName() : TEXT("<none>"), OwningNode ? *OwningNode->GetName() : TEXT("<none>"), *IntPin->PinName.ToString(), *IntPin->PinType.PinCategory.ToString(), *IntPin->DefaultValue, IntPin->LinkedTo.Num(), *BlueprintFixtureGraphGenerator::ValidateScalarDefault(*IntPin)));

    Errors.Reset();
    const bool bCorruptValid = BlueprintFixtureGraphGenerator::ValidatePinDefaults(Blueprint, Errors);

    TestFalse(TEXT("a bad pin default is rejected before compile"), bCorruptValid);
    // Do not assume Errors[0]: other pre-existing fixture errors may sort first. Assert that some error
    // names this pin in its graph.
    const FString PinGraphName = OwningNode && OwningNode->GetGraph() ? OwningNode->GetGraph()->GetName() : FString();
    const bool bPinNamedInErrors = Errors.ContainsByPredicate([&IntPin, &PinGraphName](const FString& Error) {
        return Error.Contains(IntPin->PinName.ToString()) && Error.Contains(PinGraphName);
    });
    TestTrue(TEXT("an error names the graph and pin"), bPinNamedInErrors);
    for (const FString& Error : Errors)
    {
        AddInfo(Error);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFixtureGraphInspectTest, "BlueprintInspector.Fixture.InspectRegeneratedAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintFixtureGraphInspectTest::RunTest(const FString& Parameters)
{
    if (!BlueprintFixtureGraphGenerator::AreToolsEnabled())
    {
        AddInfo(TEXT("Fixture graph tools skipped; pass -BlueprintFixtureTools to run them."));
        return true;
    }

    UBlueprint* const Blueprint = BlueprintFixtureGraphGenerator::LoadFixture();
    if (!Blueprint)
    {
        AddError(TEXT("Fixture blueprint BP_OpcodeDemo_All was not loaded."));
        return false;
    }

    int32 TotalNodes = 0;
    int32 TotalTerminators = 0;
    TArray<FString> GraphsWithoutTerminators;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph)
        {
            continue;
        }

        const int32 Terminators = BlueprintFixtureGraphGenerator::CountTerminators(Graph);
        TotalNodes += Graph->Nodes.Num();
        TotalTerminators += Terminators;
        if (Terminators == 0)
        {
            GraphsWithoutTerminators.Add(Graph->GetName());
        }
    }

    AddInfo(FString::Printf(TEXT("Fixture state: graphs=%d functions=%d nodes=%d terminators=%d graphsWithoutTerminators=%d."), Blueprint->FunctionGraphs.Num(), BlueprintFixtureGraphGenerator::CountFunctions(Blueprint->GeneratedClass), TotalNodes, TotalTerminators, GraphsWithoutTerminators.Num()));

    TMap<FName, int32> FunctionNameCounts;
    TArray<FString> FunctionNames;
    TArray<FString> ShortScripts;
    if (Blueprint->GeneratedClass)
    {
        for (TFieldIterator<UFunction> It(Blueprint->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            if (UFunction* const Function = *It)
            {
                FunctionNameCounts.FindOrAdd(Function->GetFName())++;
                FunctionNames.Add(Function->GetName());
                if (Function->Script.Num() < 8)
                {
                    ShortScripts.Add(FString::Printf(TEXT("%s:%d"), *Function->GetName(), Function->Script.Num()));
                }
            }
        }
    }

    TArray<FString> DuplicateNames;
    for (const TPair<FName, int32>& Pair : FunctionNameCounts)
    {
        if (Pair.Value > 1)
        {
            DuplicateNames.Add(FString::Printf(TEXT("%s x%d"), *Pair.Key.ToString(), Pair.Value));
        }
    }
    AddInfo(FString::Printf(TEXT("Distinct function names=%d; duplicates=%s."), FunctionNameCounts.Num(), DuplicateNames.Num() > 0 ? *FString::Join(DuplicateNames, TEXT(", ")) : TEXT("none")));
    AddInfo(FString::Printf(TEXT("Function names: %s"), *FString::Join(FunctionNames, TEXT(", "))));
    AddInfo(FString::Printf(TEXT("Short scripts=%d; %s"), ShortScripts.Num(), *FString::Join(ShortScripts, TEXT(", "))));
    if (GraphsWithoutTerminators.Num() > 0)
    {
        AddInfo(FString::Printf(TEXT("Graphs without terminators: %s"), *FString::Join(GraphsWithoutTerminators, TEXT(", "))));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFlowFixtureAuthoringTest, "BlueprintInspector.Fixture.AuthorFiniteFlowFixtures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintFlowFixtureAuthoringTest::RunTest(const FString& Parameters)
{
    if (!BlueprintFixtureGraphGenerator::AreToolsEnabled())
    {
        AddInfo(TEXT("Flow fixture authoring skipped; pass -BlueprintFixtureTools to run it."));
        return true;
    }

    UBlueprint* const Blueprint = BlueprintFixtureGraphGenerator::LoadFixture();
    if (!Blueprint)
    {
        AddError(TEXT("Fixture blueprint BP_OpcodeDemo_All was not loaded."));
        return false;
    }

    if (!CanSuppressAccessViolation())
    {
        AddError(FString::Printf(TEXT("Kismet.bCanSuppressAccessViolation is false; pass %s. The asset was not compiled or saved."), FailSilentCommandLineSwitch()));
        return false;
    }

    FBlueprintOpcodeFixtureBuilder Builder(*Blueprint);

    struct FAuthoring
    {
        const TCHAR* GraphName = nullptr;
        bool (FBlueprintOpcodeFixtureBuilder::*Body)(UEdGraph&) = nullptr;
        bool bLoop = false;
    };

    const TArray<FAuthoring> Fixtures = {
        {TEXT("flow_branch"), &FBlueprintOpcodeFixtureBuilder::AuthorBranch},
        {TEXT("flow_while"), &FBlueprintOpcodeFixtureBuilder::AuthorWhile, true},
        {TEXT("flow_do_while"), &FBlueprintOpcodeFixtureBuilder::AuthorDoWhile, true},
        {TEXT("flow_continue"), &FBlueprintOpcodeFixtureBuilder::AuthorContinue, true},
        {TEXT("flow_break"), &FBlueprintOpcodeFixtureBuilder::AuthorBreak, true},
        {TEXT("flow_nested_loops"), &FBlueprintOpcodeFixtureBuilder::AuthorNestedLoops, true},
        {TEXT("flow_switch"), &FBlueprintOpcodeFixtureBuilder::AuthorSwitch},
        {TEXT("call_selfarg"), &FBlueprintOpcodeFixtureBuilder::AuthorCallerArgument},
        {TEXT("context_failsilent"), &FBlueprintOpcodeFixtureBuilder::AuthorContextFailSilent},
        {TEXT("let_delegate"), &FBlueprintOpcodeFixtureBuilder::AuthorLetDelegate},
        {TEXT("delegate_createdelegate"), &FBlueprintOpcodeFixtureBuilder::AuthorDelegateCreateDelegate},
        {TEXT("let_enum"), &FBlueprintOpcodeFixtureBuilder::AuthorLetEnum},
        {TEXT("delegate_instance"), &FBlueprintOpcodeFixtureBuilder::AuthorDelegateInstance},
    };

    if (!Builder.EnsureLoopFlag())
    {
        AddError(TEXT("The LoopFlag member could not be added to BP_OpcodeDemo_All."));
        return false;
    }

    if (!Builder.EnsureDelegateVariable())
    {
        AddError(TEXT("The native SingleDelegateVar variable could not be installed on BP_OpcodeDemo_All."));
        return false;
    }

    if (!Builder.EnsureEnumAsset())
    {
        AddError(TEXT("E_OpcodeDemoEnum could not be brought up to Three values."));
        return false;
    }

    if (!Builder.EnsureEnumVariable())
    {
        AddError(TEXT("The EnumVar variable could not be installed on BP_OpcodeDemo_All."));
        return false;
    }

    if (!Builder.EnsureWeakVariable())
    {
        AddError(TEXT("The WeakObjVar variable could not be made a weak object pointer."));
        return false;
    }

    if (!Builder.EnsureDelegateLibraryFunction())
    {
        AddError(TEXT("The SingleDelegate function could not be added to BFL_ProbeLibrary."));
        return false;
    }

    int32 Authored = 0;
    for (const FAuthoring& Fixture : Fixtures)
    {
        UEdGraph* const Graph = Builder.FindOrCreateGraph(Fixture.GraphName);
        if (Graph == nullptr)
        {
            // Continuing would compile and save a fixture that is missing this graph. Stop before the
            // compile/save below so the incomplete asset is never persisted.
            AddError(FString::Printf(TEXT("%s graph could not be created. The asset was not compiled or saved."), Fixture.GraphName));
            return false;
        }

        if (!(Builder.*Fixture.Body)(*Graph))
        {
            AddError(FString::Printf(TEXT("%s could not be laid out: a node or pin is missing. The asset was not compiled or saved."), Fixture.GraphName));
            return false;
        }

        if (Fixture.bLoop)
        {
            bool bTwoPredecessors = false;
            for (const UEdGraphNode* const Node : Graph->Nodes)
            {
                if (Node == nullptr)
                {
                    continue;
                }
                for (const UEdGraphPin* const Pin : Node->Pins)
                {
                    if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->LinkedTo.Num() > 1)
                    {
                        bTwoPredecessors = true;
                    }
                }
            }
            if (!bTwoPredecessors)
            {
                AddError(FString::Printf(TEXT("%s has no exec input with two predecessors; the back edge did not survive authoring. The asset was not compiled or saved."), Fixture.GraphName));
                return false;
            }
        }
        Builder.CompactLayout(*Graph);
        ++Authored;
    }

    if (!Builder.AuthorEventGraphFixtures())
    {
        AddError(TEXT("The EventGraph fixtures could not be laid out: a node or pin is missing. The asset was not compiled or saved."));
        return false;
    }

    TArray<FString> PinErrors;
    if (!BlueprintFixtureGraphGenerator::ValidatePinDefaults(Blueprint, PinErrors))
    {
        for (const FString& Error : PinErrors)
        {
            AddError(FString::Printf(TEXT("Invalid pin default: %s"), *Error));
        }
        AddError(TEXT("Flow fixture authoring aborted before compile; the asset was not saved."));
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error)
    {
        AddError(TEXT("Blueprint failed to compile after the flow fixtures were laid out; the asset was not saved."));
        return false;
    }

    for (const FAuthoring& Fixture : Fixtures)
    {
        UFunction* const Function = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->FindFunctionByName(Fixture.GraphName) : nullptr;
        if (Function == nullptr || Function->Script.Num() < 8)
        {
            AddError(FString::Printf(TEXT("%s compiled to %d script byte(s); the asset was not saved."), Fixture.GraphName, Function ? Function->Script.Num() : 0));
            return false;
        }
    }

    FString SaveError;
    if (!BlueprintFixtureGraphGenerator::SaveBlueprint(Blueprint, SaveError))
    {
        AddError(SaveError);
        return false;
    }

    AddInfo(FString::Printf(TEXT("Laid out %d of %d flow fixture(s); graphs=%d functions=%d."), Authored, Fixtures.Num(), Blueprint->FunctionGraphs.Num(), BlueprintFixtureGraphGenerator::CountFunctions(Blueprint->GeneratedClass)));
    TestEqual(TEXT("flow fixtures laid out"), Authored, Fixtures.Num());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
