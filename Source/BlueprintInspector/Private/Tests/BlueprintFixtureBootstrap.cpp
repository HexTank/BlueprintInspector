// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintFixtureBootstrap.h"
#include "BlueprintFixtureSource.h"

#if WITH_EDITOR

#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"
#include "OpcodeProbeHost.h"

#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "Factories.h"
#include "FileHelpers.h"
#if BI_ENGINE_AT_LEAST_5_5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Materials/Material.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

#include "BlueprintOpcodeFixtureBuilder.h"
#include "BlueprintStructDefaults.h"
#include "BlueprintTestConfiguration.h"

namespace BlueprintFixtureBootstrap
{
namespace
{
const TCHAR* const CanonicalFixtureRoot = TEXT("/BlueprintInspector/Tests/OpcodeShowcase");
FString FixtureRoot = CanonicalFixtureRoot;
bool bSaveAssets = true;
bool bInProgress = false;

FString AssetPath(const TCHAR* const Name)
{
    return FString::Printf(TEXT("%s/%s"), *FixtureRoot, Name);
}

FString ObjectPath(const TCHAR* const Name, const TCHAR* const ClassName)
{
    return FString::Printf(TEXT("%s/%s.%s"), *FixtureRoot, Name, ClassName);
}

FString FixtureContentDir()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BlueprintInspector"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: the BlueprintInspector plugin is not registered; the fixture content directory is unknown."));
        return FString();
    }
    return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content/Tests/OpcodeShowcase"));
}

FString ArchiveDir()
{
    return FPaths::Combine(FixtureContentDir(), TEXT("Graphs"));
}

UPackage* GetOrCreatePackage(const FString& PackagePath)
{
    if (UPackage* const Existing = FindPackage(nullptr, *PackagePath))
    {
        return Existing;
    }
#if BI_ENGINE_AT_LEAST_5
    return CreatePackage(*PackagePath);
#else
    return CreatePackage(nullptr, *PackagePath);
#endif
}

bool SaveAsset(UObject* const Asset)
{
    if (!bSaveAssets)
    {
        return Asset != nullptr;
    }

    UPackage* const Package = Asset ? Asset->GetOutermost() : nullptr;
    if (Package == nullptr)
    {
        return false;
    }

    Package->MarkPackageDirty();
    const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

#if BI_ENGINE_AT_LEAST_5
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, Asset, *FileName, SaveArgs);
#else
    return UPackage::SavePackage(Package, Asset, RF_Public | RF_Standalone, *FileName, GError, nullptr, false, true, SAVE_NoError);
#endif
}

// Compiles a fixture and reports a BS_Error. Callers abort instead of saving an asset whose
// GeneratedClass the suite would then use.
bool CompileBlueprintChecked(UBlueprint* const Blueprint, const TCHAR* const What)
{
    if (Blueprint == nullptr)
    {
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %s failed to compile."), What);
        return false;
    }
    return true;
}

FEdGraphPinType PinType(const FName Category, UObject* const SubObject = nullptr)
{
    FEdGraphPinType Type;
    Type.PinCategory = Category;
    Type.PinSubCategoryObject = SubObject;
    return Type;
}

FEdGraphPinType RealType()
{
#if BI_ENGINE_AT_LEAST_5
    // PC_Real carries its width in PinSubCategory; the Kismet compiler rejects an empty one.
    FEdGraphPinType Type = PinType(UEdGraphSchema_K2::PC_Real);
    Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    return Type;
#else
    return PinType(UEdGraphSchema_K2::PC_Float);
#endif
}

FEdGraphPinType StructType(UScriptStruct* const Struct)
{
    return PinType(UEdGraphSchema_K2::PC_Struct, Struct);
}

FEdGraphPinType ObjectType(UClass* const Class)
{
    return PinType(UEdGraphSchema_K2::PC_Object, Class);
}

FEdGraphPinType WeakObjectType(UClass* const Class)
{
    FEdGraphPinType Type = ObjectType(Class);
    Type.bIsWeakPointer = true;
    return Type;
}

FEdGraphPinType ContainerType(const FEdGraphPinType& Element, const EPinContainerType Container)
{
    FEdGraphPinType Type = Element;
    Type.ContainerType = Container;
    return Type;
}

FEdGraphPinType MapType(const FEdGraphPinType& Key, const FEdGraphPinType& Value)
{
    FEdGraphPinType Type = Key;
    Type.ContainerType = EPinContainerType::Map;
    Type.PinValueType.TerminalCategory = Value.PinCategory;
    Type.PinValueType.TerminalSubCategory = Value.PinSubCategory;
    Type.PinValueType.TerminalSubCategoryObject = Value.PinSubCategoryObject;
    return Type;
}

UK2Node_FunctionEntry* FindEntry(UEdGraph* const Graph)
{
    if (Graph == nullptr)
    {
        return nullptr;
    }
    for (UEdGraphNode* const Node : Graph->Nodes)
    {
        if (UK2Node_FunctionEntry* const Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry;
        }
    }
    return nullptr;
}

UK2Node_FunctionResult* FindResult(UEdGraph* const Graph)
{
    if (Graph == nullptr)
    {
        return nullptr;
    }
    for (UEdGraphNode* const Node : Graph->Nodes)
    {
        if (UK2Node_FunctionResult* const Result = Cast<UK2Node_FunctionResult>(Node))
        {
            return Result;
        }
    }
    return nullptr;
}

struct FPinSpec
{
    FPinSpec(const TCHAR* const InName, const FEdGraphPinType& InType) : Name(InName), Type(InType) {}
    FPinSpec(const FName& InName, const FEdGraphPinType& InType) : Name(InName), Type(InType) {}

    FName Name;
    FEdGraphPinType Type;
};

UEdGraph* AddFunction(UBlueprint* const Blueprint, const TCHAR* const Name, const std::initializer_list<FPinSpec>& Inputs, const std::initializer_list<FPinSpec>& Outputs)
{
    if (Blueprint == nullptr)
    {
        return nullptr;
    }

    UEdGraph* const Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (Graph == nullptr)
    {
        return nullptr;
    }
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);

    UK2Node_FunctionEntry* const Entry = FindEntry(Graph);
    if (Entry == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %s has no function entry."), Name);
        FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
        return nullptr;
    }
    for (const FPinSpec& Input : Inputs)
    {
        if (Entry->CreateUserDefinedPin(Input.Name, Input.Type, EGPD_Output) == nullptr)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %s could not add input %s."), Name, *Input.Name.ToString());
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
            return nullptr;
        }
    }

    UK2Node_FunctionResult* Result = FindResult(Graph);
    if (Outputs.size() > 0 && Result == nullptr)
    {
        Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
    }
    if (Outputs.size() > 0 && Result == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %s has no function result."), Name);
        FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
        return nullptr;
    }
    for (const FPinSpec& Output : Outputs)
    {
        if (Result == nullptr || Result->CreateUserDefinedPin(Output.Name, Output.Type, EGPD_Input) == nullptr)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %s could not add output %s."), Name, *Output.Name.ToString());
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
            return nullptr;
        }
    }

    const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
    if (Entry != nullptr && Result != nullptr && Schema != nullptr)
    {
        Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then), Result->FindPin(UEdGraphSchema_K2::PN_Execute));
    }
    return Graph;
}

bool AddReturnFunction(UBlueprint* const Blueprint, const TCHAR* const Name, const TCHAR* const DefaultValue)
{
    UEdGraph* const Graph = AddFunction(Blueprint, Name, {}, {{UEdGraphSchema_K2::PN_ReturnValue, PinType(UEdGraphSchema_K2::PC_Int)}});
    UK2Node_FunctionResult* const Result = FindResult(Graph);
    UEdGraphPin* const ReturnPin = Result ? Result->FindPin(UEdGraphSchema_K2::PN_ReturnValue) : nullptr;
    if (ReturnPin == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %s has no return pin."), Name);
        return false;
    }
    ReturnPin->DefaultValue = DefaultValue;
    return true;
}

// A Blueprint event dispatcher: the four delegate graph rows call, add to, remove from and clear a
// multicast delegate owned by the fixture blueprint. The graph text names it, but the dispatcher and
// its signature are blueprint state, not graph text, so the bootstrap has to author them first.
bool EnsureMulticastDispatcher(UBlueprint* const Blueprint, const TCHAR* const Name)
{
    if (Blueprint == nullptr)
    {
        return false;
    }
    UEdGraph* Graph = nullptr;
    for (UEdGraph* const SignatureGraph : Blueprint->DelegateSignatureGraphs)
    {
        if (SignatureGraph != nullptr && SignatureGraph->GetFName() == FName(Name))
        {
            Graph = SignatureGraph;
            break;
        }
    }
    const bool bHasSignatureGraph = Graph != nullptr;

    bool bAddedVariable = false;
    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(Name)) == INDEX_NONE)
    {
        FEdGraphPinType DelegateType;
        DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
        if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(Name), DelegateType))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not add %s."), Name);
            return false;
        }
        bAddedVariable = true;
    }
    else if (bHasSignatureGraph)
    {
        return true;
    }

    if (Graph == nullptr)
    {
        Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    }
    const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
    if (Graph == nullptr || Schema == nullptr)
    {
        if (Graph != nullptr)
        {
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
        }
        if (bAddedVariable)
        {
            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(Name));
        }
        return false;
    }

    Graph->bEditable = false;
    if (Graph->Nodes.Num() == 0)
    {
        Schema->CreateDefaultNodesForGraph(*Graph);
        Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
    }
    Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
    Schema->MarkFunctionEntryAsEditable(Graph, true);

    UK2Node_FunctionEntry* const Entry = FindEntry(Graph);
    if (Entry == nullptr || (Entry->FindPin(TEXT("Value"), EGPD_Output) == nullptr && Entry->CreateUserDefinedPin(TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Int), EGPD_Output) == nullptr))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not author the %s signature."), Name);
        FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
        if (bAddedVariable)
        {
            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(Name));
        }
        return false;
    }
    if (UK2Node_FunctionResult* const Result = FindResult(Graph))
    {
        Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then), Result->FindPin(UEdGraphSchema_K2::PN_Execute));
    }

    if (!Blueprint->DelegateSignatureGraphs.Contains(Graph))
    {
        Blueprint->DelegateSignatureGraphs.Add(Graph);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    return true;
}

UBlueprint* GetOrCreateBlueprint(const TCHAR* const Name, UClass* const Parent, const EBlueprintType Type)
{
    if (UBlueprint* const Existing = LoadObject<UBlueprint>(nullptr, *ObjectPath(Name, Name), nullptr, LOAD_NoWarn))
    {
        return Existing;
    }

    UPackage* const Package = GetOrCreatePackage(AssetPath(Name));
    if (Package == nullptr)
    {
        return nullptr;
    }

    UBlueprint* const Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(Name), Type, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (Blueprint == nullptr)
    {
        return nullptr;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    if (!CompileBlueprintChecked(Blueprint, Name))
    {
        return nullptr;
    }
    if (!SaveAsset(Blueprint))
    {
        return nullptr;
    }
    return Blueprint;
}

bool AddVariable(UBlueprint* const Blueprint, const TCHAR* const Name, const FEdGraphPinType& Type)
{
    if (Blueprint == nullptr)
    {
        return false;
    }
    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(Name)) != INDEX_NONE)
    {
        return true;
    }
    return FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(Name), Type);
}

bool AddStructMember(UUserDefinedStruct* const Struct, const TCHAR* const Name, const FEdGraphPinType& Type)
{
    if (Struct == nullptr || Name == nullptr)
    {
        return false;
    }

    // Re-running the bootstrap must not append a second copy of the member. Match on the base name: a
    // user-defined-struct member carries an engine-generated _<index>_<guid> suffix.
    TArray<FGuid> ExistingGuids;
    for (const FStructVariableDescription& Variable : FStructureEditorUtils::GetVarDesc(Struct))
    {
        if (BlueprintStructDefaults::MemberBaseName(Variable.VarName.ToString()) == Name)
        {
            return true;
        }
        ExistingGuids.Add(Variable.VarGuid);
    }

    if (!FStructureEditorUtils::AddVariable(Struct, Type))
    {
        return false;
    }

    FGuid AddedGuid;
    for (const FStructVariableDescription& Variable : FStructureEditorUtils::GetVarDesc(Struct))
    {
        if (!ExistingGuids.Contains(Variable.VarGuid))
        {
            AddedGuid = Variable.VarGuid;
            break;
        }
    }
    if (!AddedGuid.IsValid())
    {
        return false;
    }
    if (!FStructureEditorUtils::RenameVariable(Struct, AddedGuid, Name))
    {
        FStructureEditorUtils::RemoveVariable(Struct, AddedGuid);
        return false;
    }
    return true;
}

UUserDefinedStruct* CreateStruct(const TCHAR* const Name)
{
    if (UUserDefinedStruct* const Existing = LoadObject<UUserDefinedStruct>(nullptr, *ObjectPath(Name, Name), nullptr, LOAD_NoWarn))
    {
        return Existing;
    }
    UPackage* const Package = GetOrCreatePackage(AssetPath(Name));
    return Package ? FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(Name), RF_Public | RF_Standalone) : nullptr;
}

UUserDefinedEnum* CreateEnum(const TCHAR* const Name)
{
    if (UUserDefinedEnum* const Existing = LoadObject<UUserDefinedEnum>(nullptr, *ObjectPath(Name, Name), nullptr, LOAD_NoWarn))
    {
        return Existing;
    }
    UPackage* const Package = GetOrCreatePackage(AssetPath(Name));
    return Package ? Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(Name), RF_Public | RF_Standalone)) : nullptr;
}

UMaterial* EnsureMaterial(const TCHAR* const Name)
{
    if (UMaterial* const Existing = LoadObject<UMaterial>(nullptr, *ObjectPath(Name, Name), nullptr, LOAD_NoWarn))
    {
        return Existing;
    }
    UPackage* const Package = GetOrCreatePackage(AssetPath(Name));
    if (Package == nullptr)
    {
        return nullptr;
    }

    UMaterial* const Material = NewObject<UMaterial>(Package, FName(Name), RF_Public | RF_Standalone);
    // A dropped save leaves the fixture blueprints referencing an asset that was never written, so a
    // failed save is a failed fixture, not a warning.
    if (Material == nullptr || !SaveAsset(Material))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not save %s."), Name);
        return nullptr;
    }
    return Material;
}

bool EnsureVariables(UBlueprint* const Blueprint, UEnum* const Enum, UScriptStruct* const Nested, UScriptStruct* const Struct, UScriptStruct* const Containers, UClass* const Interface)
{
    if (Blueprint == nullptr)
    {
        return false;
    }

    const FDelegateProperty* const DelegateProperty = FindFProperty<FDelegateProperty>(UOpcodeProbeHost::StaticClass(), TEXT("SingleDelegateVar"));
    const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
    FEdGraphPinType DelegateType;
    const bool bDelegateType = DelegateProperty != nullptr && Schema != nullptr && Schema->ConvertPropertyToPinType(DelegateProperty, DelegateType);

    TArray<FPinSpec> Variables;
    Variables.Emplace(TEXT("IntVar"), PinType(UEdGraphSchema_K2::PC_Int));
    Variables.Emplace(TEXT("Int64Var"), PinType(UEdGraphSchema_K2::PC_Int64));
    Variables.Emplace(TEXT("ByteVar"), PinType(UEdGraphSchema_K2::PC_Byte));
    Variables.Emplace(TEXT("EnumVar"), PinType(UEdGraphSchema_K2::PC_Byte, Enum));
    Variables.Emplace(TEXT("BoolVar"), PinType(UEdGraphSchema_K2::PC_Boolean));
    Variables.Emplace(TEXT("RealVar"), RealType());
    Variables.Emplace(TEXT("StringVar"), PinType(UEdGraphSchema_K2::PC_String));
    Variables.Emplace(TEXT("NameVar"), PinType(UEdGraphSchema_K2::PC_Name));
    Variables.Emplace(TEXT("TextVar"), PinType(UEdGraphSchema_K2::PC_Text));
    Variables.Emplace(TEXT("ObjectVar"), ObjectType(UObject::StaticClass()));
    Variables.Emplace(TEXT("ActorVar"), ObjectType(AActor::StaticClass()));
    Variables.Emplace(TEXT("ClassVar"), PinType(UEdGraphSchema_K2::PC_Class, AActor::StaticClass()));
    Variables.Emplace(TEXT("SoftObjVar"), PinType(UEdGraphSchema_K2::PC_SoftObject, UObject::StaticClass()));
    Variables.Emplace(TEXT("WeakObjVar"), WeakObjectType(AActor::StaticClass()));
    Variables.Emplace(TEXT("InterfaceVar"), PinType(UEdGraphSchema_K2::PC_Interface, Interface));
    {
        UClass* const InterfaceBClass = LoadObject<UClass>(nullptr, *ObjectPath(TEXT("BPI_ProbeB"), TEXT("BPI_ProbeB_C")));
        UClass* const CastTargetClass = LoadObject<UClass>(nullptr, *ObjectPath(TEXT("BP_ProbeCastTarget"), TEXT("BP_ProbeCastTarget_C")));
        if (InterfaceBClass != nullptr)
        {
            Variables.Emplace(TEXT("InterfaceBVar"), PinType(UEdGraphSchema_K2::PC_Interface, InterfaceBClass));
        }
        if (CastTargetClass != nullptr)
        {
            Variables.Emplace(TEXT("CastTargetVar"), ObjectType(CastTargetClass));
        }
    }
    Variables.Emplace(TEXT("StructVar"), StructType(Struct));
    Variables.Emplace(TEXT("ContainerStructVar"), StructType(Containers));
    Variables.Emplace(TEXT("ArrayVar"), ContainerType(PinType(UEdGraphSchema_K2::PC_Int), EPinContainerType::Array));
    Variables.Emplace(TEXT("StringArrayVar"), ContainerType(PinType(UEdGraphSchema_K2::PC_String), EPinContainerType::Array));
    Variables.Emplace(TEXT("SetVar"), ContainerType(PinType(UEdGraphSchema_K2::PC_Name), EPinContainerType::Set));
    Variables.Emplace(TEXT("MapVar"), MapType(PinType(UEdGraphSchema_K2::PC_Int), PinType(UEdGraphSchema_K2::PC_Name)));
    Variables.Emplace(TEXT("VectorVar"), StructType(TBaseStructure<FVector>::Get()));
    Variables.Emplace(TEXT("RotatorVar"), StructType(TBaseStructure<FRotator>::Get()));
    Variables.Emplace(TEXT("TransformVar"), StructType(TBaseStructure<FTransform>::Get()));
    Variables.Emplace(TEXT("NestedVar"), StructType(Nested));
    Variables.Emplace(TEXT("LoopFlag"), PinType(UEdGraphSchema_K2::PC_Boolean));

    for (const FPinSpec& Variable : Variables)
    {
        if (!AddVariable(Blueprint, *Variable.Name.ToString(), Variable.Type))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not add variable %s."), *Variable.Name.ToString());
            return false;
        }
    }

    if (bDelegateType && !AddVariable(Blueprint, TEXT("SingleDelegateVar"), DelegateType))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not add SingleDelegateVar."));
        return false;
    }

    const int32 WeakIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("WeakObjVar"));
    if (WeakIndex != INDEX_NONE)
    {
        Blueprint->NewVariables[WeakIndex].VarType.bIsWeakPointer = true;
    }
    return true;
}

bool CreatePrerequisiteAssets(UEnum*& OutEnum, UScriptStruct*& OutNested, UScriptStruct*& OutStruct, UScriptStruct*& OutContainers, UClass*& OutInterface, UBlueprint*& OutParent)
{
    UUserDefinedEnum* const Enum = CreateEnum(TEXT("E_OpcodeDemoEnum"));
    if (Enum == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: E_OpcodeDemoEnum failed."));
        return false;
    }
    // NumEnums() includes the hidden _MAX entry, so four entries are the three values plus _MAX.
    // Bound the add attempts: the engine API returns void, and a failed add would otherwise spin this
    // loop forever in the command ticker.
    int32 AddAttempts = 0;
    while (Enum->NumEnums() < 4 && AddAttempts < 4)
    {
        FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
        ++AddAttempts;
    }
    if (Enum->NumEnums() < 4)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: E_OpcodeDemoEnum could not be extended to 4 entries."));
        return false;
    }
    FEnumEditorUtils::SetEnumeratorDisplayName(Enum, 0, FText::FromString(TEXT("One")));
    FEnumEditorUtils::SetEnumeratorDisplayName(Enum, 1, FText::FromString(TEXT("Two")));
    FEnumEditorUtils::SetEnumeratorDisplayName(Enum, 2, FText::FromString(TEXT("Three")));
    if (!SaveAsset(Enum))
    {
        return false;
    }
    OutEnum = Enum;

    UUserDefinedStruct* const Nested = CreateStruct(TEXT("S_ProbeNested"));
    if (Nested == nullptr || !AddStructMember(Nested, TEXT("Flag"), PinType(UEdGraphSchema_K2::PC_Boolean)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: S_ProbeNested failed."));
        return false;
    }
    FStructureEditorUtils::CompileStructure(Nested);
    if (!SaveAsset(Nested))
    {
        return false;
    }
    OutNested = Nested;

    UUserDefinedStruct* const Struct = CreateStruct(TEXT("S_ProbeStruct"));
    if (Struct == nullptr || !AddStructMember(Struct, TEXT("Int"), PinType(UEdGraphSchema_K2::PC_Int)) || !AddStructMember(Struct, TEXT("Real"), RealType()) || !AddStructMember(Struct, TEXT("Str"), PinType(UEdGraphSchema_K2::PC_String)) || !AddStructMember(Struct, TEXT("Obj"), ObjectType(UObject::StaticClass())) || !AddStructMember(Struct, TEXT("Nested"), StructType(Nested)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: S_ProbeStruct failed."));
        return false;
    }
    FStructureEditorUtils::CompileStructure(Struct);
    if (!SaveAsset(Struct))
    {
        return false;
    }
    OutStruct = Struct;

    UUserDefinedStruct* const Containers = CreateStruct(TEXT("S_ProbeContainers"));
    if (Containers == nullptr || !AddStructMember(Containers, TEXT("Ints"), ContainerType(PinType(UEdGraphSchema_K2::PC_Int), EPinContainerType::Array)) || !AddStructMember(Containers, TEXT("Names"), ContainerType(PinType(UEdGraphSchema_K2::PC_Name), EPinContainerType::Set)) || !AddStructMember(Containers, TEXT("Lookup"), MapType(PinType(UEdGraphSchema_K2::PC_Int), PinType(UEdGraphSchema_K2::PC_Name))))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: S_ProbeContainers failed."));
        return false;
    }
    FStructureEditorUtils::CompileStructure(Containers);
    if (!SaveAsset(Containers))
    {
        return false;
    }
    OutContainers = Containers;

    UBlueprint* const Interface = GetOrCreateBlueprint(TEXT("BPI_ProbeA"), UInterface::StaticClass(), BPTYPE_Interface);
    // The message-call row needs a void interface function: the VM backend marks a call with any
    // return/out parameter as unsafe to skip, so an int return would emit EX_Context instead of the
    // planned EX_Context_FailSilent.
    if (Interface == nullptr || AddFunction(Interface, TEXT("IProbeFunc"),
                                            {{TEXT("Input"), PinType(UEdGraphSchema_K2::PC_Int)}}, {}) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BPI_ProbeA failed."));
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Interface);
    if (!CompileBlueprintChecked(Interface, TEXT("BPI_ProbeA")) || !SaveAsset(Interface))
    {
        return false;
    }
    OutInterface = Interface->GeneratedClass.Get();

    UBlueprint* const InterfaceB = GetOrCreateBlueprint(TEXT("BPI_ProbeB"), UInterface::StaticClass(), BPTYPE_Interface);
    if (InterfaceB == nullptr || AddFunction(InterfaceB, TEXT("IProbeOther"),
                                             {{TEXT("Input"), PinType(UEdGraphSchema_K2::PC_Int)}},
                                             {{UEdGraphSchema_K2::PN_ReturnValue, PinType(UEdGraphSchema_K2::PC_Int)}}) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BPI_ProbeB failed."));
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(InterfaceB);
    if (!CompileBlueprintChecked(InterfaceB, TEXT("BPI_ProbeB")) || !SaveAsset(InterfaceB))
    {
        return false;
    }

    UBlueprint* const Parent = GetOrCreateBlueprint(TEXT("BP_ProbeParent"), AActor::StaticClass(), BPTYPE_Normal);
    if (Parent == nullptr || AddFunction(Parent, TEXT("F_ParentTarget"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Int)}}, {}) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BP_ProbeParent failed."));
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Parent);
    if (!CompileBlueprintChecked(Parent, TEXT("BP_ProbeParent")) || !SaveAsset(Parent))
    {
        return false;
    }
    OutParent = Parent;

    UBlueprint* const CastTarget = GetOrCreateBlueprint(TEXT("BP_ProbeCastTarget"), AActor::StaticClass(), BPTYPE_Normal);
    if (CastTarget == nullptr || !AddVariable(CastTarget, TEXT("ProbeInt"), PinType(UEdGraphSchema_K2::PC_Int)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BP_ProbeCastTarget failed."));
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(CastTarget);
    if (!CompileBlueprintChecked(CastTarget, TEXT("BP_ProbeCastTarget")) || !SaveAsset(CastTarget))
    {
        return false;
    }

    if (EnsureMaterial(TEXT("M_Probe")) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: M_Probe failed."));
        return false;
    }

    UBlueprint* const Library = GetOrCreateBlueprint(TEXT("BFL_ProbeLibrary"), UBlueprintFunctionLibrary::StaticClass(), BPTYPE_FunctionLibrary);
    if (Library == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BFL_ProbeLibrary failed."));
        return false;
    }

    const FDelegateProperty* const DelegateProperty = FindFProperty<FDelegateProperty>(UOpcodeProbeHost::StaticClass(), TEXT("SingleDelegateVar"));
    FEdGraphPinType DelegateType;
    const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
    const bool bDelegateType = DelegateProperty != nullptr && Schema != nullptr && Schema->ConvertPropertyToPinType(DelegateProperty, DelegateType);

    if (AddFunction(Library, TEXT("DefaultArray"),
                    {{TEXT("Ints"), ContainerType(PinType(UEdGraphSchema_K2::PC_Int), EPinContainerType::Array)}}, {}) == nullptr ||
        AddFunction(Library, TEXT("DefaultStruct"), {{TEXT("Value"), StructType(Containers)}}, {}) == nullptr || AddFunction(Library, TEXT("SoftRef"), {{TEXT("Asset"), PinType(UEdGraphSchema_K2::PC_SoftObject, UObject::StaticClass())}}, {}) == nullptr || (bDelegateType && AddFunction(Library, TEXT("SingleDelegate"), {{TEXT("Del"), DelegateType}}, {}) == nullptr) || AddFunction(Library, TEXT("Echo_Byte"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Byte)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Double"), {{TEXT("Value"), RealType()}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Int"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Int)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Int64"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Int64)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Name"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Name)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Rotator"), {{TEXT("Value"), StructType(TBaseStructure<FRotator>::Get())}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_String"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_String)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_UnicodeString"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_String)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Text"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Text)}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Transform"), {{TEXT("Value"), StructType(TBaseStructure<FTransform>::Get())}}, {}) == nullptr || AddFunction(Library, TEXT("Echo_Vector"), {{TEXT("Value"), StructType(TBaseStructure<FVector>::Get())}}, {}) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BFL_ProbeLibrary functions failed."));
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Library);
    if (!CompileBlueprintChecked(Library, TEXT("BFL_ProbeLibrary")) || !SaveAsset(Library))
    {
        return false;
    }

    return true;
}

FString NormalizeGraphText(const FString& Text)
{
    // The archive paths name the canonical fixture package. A transient build puts the same assets
    // under a different root, so rewrite the package references before importing the graph text.
    FString RootedText = Text.Replace(CanonicalFixtureRoot, *FixtureRoot);

#if BI_ENGINE_AT_LEAST_5
    return RootedText;
#else
    // The archive was exported by UE 5.8. 4.25's pin serializer does not know
    // bIsUObjectWrapper, and every pin carrying it fails to parse, which loses the function
    // entry/result nodes. 4.25 also spells the real category "float" with an empty subcategory.
    FString Out = RootedText;
    Out = Out.Replace(TEXT("PinType.bIsUObjectWrapper=False,"), TEXT(""));
    Out = Out.Replace(TEXT("PinType.bIsUObjectWrapper=True,"), TEXT(""));
    Out = Out.Replace(TEXT("PinType.bSerializeAsSinglePrecisionFloat=False,"), TEXT(""));
    Out = Out.Replace(TEXT("PinType.bSerializeAsSinglePrecisionFloat=True,"), TEXT(""));
    Out = Out.Replace(TEXT("PinType.PinCategory=\"real\",PinType.PinSubCategory=\"double\""), TEXT("PinType.PinCategory=\"float\",PinType.PinSubCategory=\"\""));
    Out = Out.Replace(TEXT("PinType.PinCategory=\"real\",PinType.PinSubCategory=\"float\""), TEXT("PinType.PinCategory=\"float\",PinType.PinSubCategory=\"\""));
    Out = Out.Replace(TEXT("MemberName=\"Add_DoubleDouble\""), TEXT("MemberName=\"Add_FloatFloat\""));
    return Out;
#endif
}

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

/**
 * The engine's paste factory refuses function entry/result nodes (K2Node_FunctionTerminator
 * overrides CanDuplicateNode to false), which is why FEdGraphUtilities::ImportNodesFromText cannot
 * build a new function graph from the archive. This factory allows them and adds every created node
 * to the destination graph, then lets the engine resolve the pin links.
 */
class FFullGraphTextFactory final : public FCustomizableTextObjectFactory
{
public:
    explicit FFullGraphTextFactory(UEdGraph* const InDestinationGraph) : FCustomizableTextObjectFactory(GWarn), DestinationGraph(InDestinationGraph) {}

    TSet<UEdGraphNode*> SpawnedNodes;

protected:
    bool CanCreateClass(UClass* const ObjectClass, bool& bOmitSubObjs) const override
    {
        if (ObjectClass != nullptr && ObjectClass->IsChildOf(UK2Node_FunctionTerminator::StaticClass()))
        {
            bOmitSubObjs = false;
            return true;
        }

        if (const UEdGraphNode* const DefaultNode = Cast<UEdGraphNode>(ObjectClass ? ObjectClass->GetDefaultObject() : nullptr))
        {
            bOmitSubObjs = true;
            return DefaultNode->CanDuplicateNode() && DestinationGraph != nullptr && DefaultNode->CanCreateUnderSpecifiedSchema(DestinationGraph->GetSchema());
        }
        return false;
    }

    void ProcessConstructedObject(UObject* const CreatedObject) override
    {
        if (UEdGraphNode* const Node = Cast<UEdGraphNode>(CreatedObject))
        {
            SpawnedNodes.Add(Node);
            if (Node->GetGraph() == nullptr)
            {
                Node->Rename(nullptr, DestinationGraph);
            }
            if (Node->GetGraph() == DestinationGraph)
            {
                DestinationGraph->Nodes.AddUnique(Node);
            }
        }
    }

    void PostProcessConstructedObjects() override
    {
        UEdGraphPin::ResolveAllPinReferences();
    }

private:
    UEdGraph* DestinationGraph = nullptr;
};

FString ExtractTextValue(const FString& Line, const TCHAR* const Key, const bool bRequireTokenStart = false)
{
    int32 Start = Line.Find(Key, ESearchCase::CaseSensitive);

    // "DefaultValue=" is a substring of "AutogeneratedDefaultValue=". When the caller asks for a token
    // start, only accept a match whose preceding character opens a property list or follows a comma.
    if (bRequireTokenStart && Start > 0)
    {
        while (Start != INDEX_NONE && Line[Start - 1] != TEXT(',') && Line[Start - 1] != TEXT('('))
        {
            Start = Line.Find(Key, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 1);
        }
    }
    if (Start == INDEX_NONE)
    {
        return FString();
    }
    Start += FCString::Strlen(Key);
    if (Start >= Line.Len())
    {
        return FString();
    }

    if (Line[Start] == TEXT('"'))
    {
        // DefaultValue carries nested struct and container literals, so its quoted text can contain
        // escaped quotes ("(\"Alpha\",\"Beta\")"). Stop at the first unescaped quote and unescape.
        FString Value;
        bool bEscaped = false;
        for (int32 Index = Start + 1; Index < Line.Len(); ++Index)
        {
            const TCHAR Character = Line[Index];
            if (bEscaped)
            {
                Value.AppendChar(Character);
                bEscaped = false;
            }
            else if (Character == TEXT('\\'))
            {
                bEscaped = true;
            }
            else if (Character == TEXT('"'))
            {
                return Value;
            }
            else
            {
                Value.AppendChar(Character);
            }
        }
        return FString();
    }

    int32 End = Line.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
    if (End == INDEX_NONE)
    {
        End = Line.Len();
    }
    return Line.Mid(Start, End - Start);
}

struct FTextPinSpec
{
    FString NodeName;
    FString PinName;
    FString PinId;
    FString DefaultValue;
    FString AutogeneratedDefaultValue;
    FString DefaultObjectPath;
    TArray<FString> LinkedPinIds;
};

void ParseGraphText(const FString& Text, TArray<FTextPinSpec>& OutPins, TMap<FString, FString>& OutPinIdToNode, TMap<FString, FString>& OutPinIdToName, TMap<FString, FString>& OutNodeTargetTypes)
{
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);
    FString CurrentNode;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("Begin Object ")) && Line.Contains(TEXT("Name=")))
        {
            CurrentNode = ExtractTextValue(Line, TEXT("Name="));
            continue;
        }

        if (!CurrentNode.IsEmpty())
        {
            FString Trimmed = Line;
            Trimmed.TrimStartInline();
            if (Trimmed.StartsWith(TEXT("TargetType=")))
            {
                OutNodeTargetTypes.Add(CurrentNode, ExtractTextValue(Trimmed, TEXT("TargetType=")));
            }
        }

        if (!Line.Contains(TEXT("CustomProperties Pin (")) || CurrentNode.IsEmpty())
        {
            continue;
        }

        FTextPinSpec Spec;
        Spec.NodeName = CurrentNode;
        Spec.PinName = ExtractTextValue(Line, TEXT("PinName="));
        Spec.PinId = ExtractTextValue(Line, TEXT("PinId="));
        Spec.DefaultValue = ExtractTextValue(Line, TEXT("DefaultValue="), /*bRequireTokenStart=*/true);
        Spec.AutogeneratedDefaultValue = ExtractTextValue(Line, TEXT("AutogeneratedDefaultValue="));
        Spec.DefaultObjectPath = ExtractTextValue(Line, TEXT("DefaultObject="));

        const int32 Start = Line.Find(TEXT("LinkedTo=("), ESearchCase::CaseSensitive);
        if (Start != INDEX_NONE)
        {
            const int32 ContentStart = Start + FCString::Strlen(TEXT("LinkedTo=("));
            const int32 End = Line.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ContentStart);
            if (End != INDEX_NONE)
            {
                TArray<FString> Items;
                Line.Mid(ContentStart, End - ContentStart).ParseIntoArray(Items, TEXT(","), true);
                for (FString Item : Items)
                {
                    Item.TrimStartAndEndInline();
                    int32 SpaceIndex = INDEX_NONE;
                    if (Item.FindLastChar(TEXT(' '), SpaceIndex))
                    {
                        Spec.LinkedPinIds.Add(Item.Mid(SpaceIndex + 1).TrimStartAndEnd());
                    }
                }
            }
        }

        if (!Spec.PinId.IsEmpty())
        {
            OutPinIdToNode.Add(Spec.PinId, Spec.NodeName);
            OutPinIdToName.Add(Spec.PinId, Spec.PinName);
        }
        OutPins.Add(MoveTemp(Spec));
    }
}

void RestoreGraphData(UEdGraph* const Graph, const FString& Text)
{
    TArray<FTextPinSpec> Specs;
    TMap<FString, FString> PinIdToNode;
    TMap<FString, FString> PinIdToName;
    TMap<FString, FString> NodeTargetTypes;
    ParseGraphText(Text, Specs, PinIdToNode, PinIdToName, NodeTargetTypes);
    if (Specs.Num() == 0)
    {
        return;
    }

    TMap<FString, UEdGraphPin*> PinsByKey;
    for (UEdGraphNode* const Node : Graph->Nodes)
    {
        if (Node == nullptr)
        {
            continue;
        }
        for (UEdGraphPin* const Pin : Node->Pins)
        {
            if (Pin != nullptr)
            {
                PinsByKey.Add(Node->GetName() + TEXT("|") + Pin->PinName.ToString(), Pin);
                Pin->LinkedTo.Reset();
            }
        }
        if (UK2Node_DynamicCast* const CastNode = Cast<UK2Node_DynamicCast>(Node))
        {
            if (const FString* const TargetPath = NodeTargetTypes.Find(Node->GetName()))
            {
                if (UClass* const TargetClass = LoadObject<UClass>(nullptr, **TargetPath))
                {
                    CastNode->TargetType = TargetClass;
                }
            }
        }
    }

    for (const FTextPinSpec& Spec : Specs)
    {
        UEdGraphPin* const Pin = PinsByKey.FindRef(Spec.NodeName + TEXT("|") + Spec.PinName);
        if (Pin == nullptr)
        {
            continue;
        }

        if (!Spec.DefaultValue.IsEmpty())
        {
            const bool bStructLiteral = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && Spec.DefaultValue.StartsWith(TEXT("("));
#if !BI_ENGINE_AT_LEAST_5
            // 4.25 cannot import a 5.8 struct literal: the member names carry GUIDs it does not know,
            // and applying the rewritten default makes the compiler reject it. A simple literal such as
            // FVector(1,2,3) does import correctly, so the imported pin default is kept as-is.
            if (!bStructLiteral)
            {
                Pin->DefaultValue = Spec.DefaultValue;
                Pin->AutogeneratedDefaultValue = Spec.AutogeneratedDefaultValue;
            }
#else
            if (bStructLiteral)
            {
                // The archive's member names carry the exporting engine's struct GUIDs. Rewrite them to
                // the target struct's own names, or the compiler clears the default.
                const UScriptStruct* const TargetStruct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
                Pin->DefaultValue = BlueprintStructDefaults::RewriteStructDefault(TargetStruct, Spec.DefaultValue);
                Pin->AutogeneratedDefaultValue = Pin->DefaultValue;
            }
            else
            {
                Pin->DefaultValue = Spec.DefaultValue;
                Pin->AutogeneratedDefaultValue = Spec.AutogeneratedDefaultValue;
            }
#endif
        }

        if (Pin->DefaultObject == nullptr && !Spec.DefaultObjectPath.IsEmpty())
        {
            Pin->DefaultObject = LoadObject<UObject>(nullptr, *Spec.DefaultObjectPath);
        }

        for (const FString& TargetPinId : Spec.LinkedPinIds)
        {
            const FString* const TargetNodeName = PinIdToNode.Find(TargetPinId);
            const FString* const TargetPinName = PinIdToName.Find(TargetPinId);
            if (TargetNodeName == nullptr || TargetPinName == nullptr)
            {
                continue;
            }
            if (UEdGraphPin* const Target = PinsByKey.FindRef(*TargetNodeName + TEXT("|") + *TargetPinName))
            {
                if (Target != Pin)
                {
                    Pin->MakeLinkTo(Target);
                }
            }
        }
    }
}

bool ImportGraphArchive(UBlueprint* const Fixture, TArray<FString>& OutSkipped, TArray<FString>& OutFailed, const TArray<FString>& Only = {}, const TArray<FString>& Excluded = {})
{
    const FString Dir = ArchiveDir();
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.txt")), true, false);
    if (Files.Num() == 0)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: no graph archive under %s."), *Dir);
        return false;
    }

    for (const FString& File : Files)
    {
        const FName GraphName(*FPaths::GetBaseFilename(File));

        // A split build imports a whitelist or leaves a blacklist to the other blueprint.
        if (Only.Num() > 0 && !Only.Contains(GraphName.ToString()))
        {
            continue;
        }
        if (Excluded.Contains(GraphName.ToString()))
        {
            continue;
        }

        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, File)))
        {
            OutFailed.Add(File);
            continue;
        }

        // Graphs this engine cannot author. The list is shared with the token test, which treats these
        // functions as known-missing instead of as regressions.
        if (GetBlueprintGraphsTheBootstrapSkips().Contains(GraphName.ToString()))
        {
            OutSkipped.Add(File);
            continue;
        }

        const FString ImportText = NormalizeGraphText(Text);

        UEdGraph* Graph = nullptr;
        for (UEdGraph* const Existing : Fixture->FunctionGraphs)
        {
            if (Existing != nullptr && Existing->GetFName() == GraphName)
            {
                Graph = Existing;
                break;
            }
        }
        if (Graph == nullptr)
        {
            Graph = FBlueprintEditorUtils::CreateNewGraph(Fixture, GraphName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (Graph != nullptr)
            {
                Fixture->FunctionGraphs.Add(Graph);
            }
        }

        if (Graph == nullptr)
        {
            OutFailed.Add(File);
            continue;
        }

        // The archive is the source of truth. Re-importing onto a graph that already has nodes would
        // append a second copy of every node (and conflict on the pins), so start from an empty graph.
        if (Graph->Nodes.Num() > 0)
        {
            TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
            for (UEdGraphNode* const Node : ExistingNodes)
            {
                if (Node != nullptr)
                {
                    FBlueprintEditorUtils::RemoveNode(Fixture, Node, true);
                }
            }
        }

        FFullGraphTextFactory Factory(Graph);
        Factory.ProcessBuffer(Graph, RF_Transactional, ImportText);
        const int32 ArchivedNodes = CountArchivedNodes(ImportText);
        if (ArchivedNodes == 0 || Factory.SpawnedNodes.Num() != ArchivedNodes || Graph->Nodes.Num() == 0)
        {
            const TArray<UEdGraphNode*> PartialNodes = Factory.SpawnedNodes.Array();
            for (UEdGraphNode* const PartialNode : PartialNodes)
            {
                if (PartialNode != nullptr)
                {
                    FBlueprintEditorUtils::RemoveNode(Fixture, PartialNode, true);
                }
            }
            OutFailed.Add(File);
            continue;
        }

        // 5.8 serializes bDefaultsToPureFunc, which 4.25 does not know. Re-apply the function flags
        // from the resolved UFunction so pure calls do not look like impure nodes without exec pins.
        for (UEdGraphNode* const Node : Graph->Nodes)
        {
            if (UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node))
            {
                if (UFunction* const Function = Call->GetTargetFunction())
                {
                    Call->SetFromFunction(Function);
                }
            }
        }

        RestoreGraphData(Graph, ImportText);
    }

    return OutFailed.Num() == 0;
}

// The level-script and runtime tests load /Game/NewMap. A host project that was copied without a map
// has neither, so the bootstrap creates one. The level-script test needs a level blueprint whose
// ReceiveBeginPlay stub calls a non-empty ubergraph, so the map is authored with a BeginPlay body.
const TCHAR* const TestMapPackage = TEXT("/Game/NewMap");

// Bump when the authored body changes. The marker next to the map records this value so a map the
// bootstrap generated can be refreshed instead of being mistaken for host content.
constexpr int32 TestMapAuthoringVersion = 1;

FString TestMapMarkerFile(const FString& MapFile)
{
    return MapFile + TEXT(".version");
}

bool AuthorLevelScriptBeginPlay(ULevel* const Level)
{
    if (Level == nullptr)
    {
        return false;
    }

    ULevelScriptBlueprint* const LevelBlueprint = Level->GetLevelScriptBlueprint(false);
    if (LevelBlueprint == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not create the level script blueprint."));
        return false;
    }

    UEdGraph* Graph = nullptr;
    for (UEdGraph* const Page : LevelBlueprint->UbergraphPages)
    {
        if (Page != nullptr)
        {
            Graph = Page;
            break;
        }
    }
    if (Graph == nullptr)
    {
        Graph = FBlueprintEditorUtils::CreateNewGraph(LevelBlueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        if (Graph != nullptr)
        {
            LevelBlueprint->UbergraphPages.Add(Graph);
        }
    }
    if (Graph == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: the level script has no ubergraph."));
        return false;
    }

    // Rebuild the body, so a second run produces the same map.
    const TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
    for (UEdGraphNode* const Node : ExistingNodes)
    {
        if (Node != nullptr)
        {
            Graph->RemoveNode(Node, true);
        }
    }

    if (FBlueprintEditorUtils::FindNewVariableIndex(LevelBlueprint, TEXT("BootstrapTicks")) == INDEX_NONE && !FBlueprintEditorUtils::AddMemberVariable(LevelBlueprint, TEXT("BootstrapTicks"), PinType(UEdGraphSchema_K2::PC_Int)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not add a level script variable."));
        return false;
    }
    if (FBlueprintEditorUtils::FindNewVariableIndex(LevelBlueprint, TEXT("SpawnTransform")) == INDEX_NONE && !FBlueprintEditorUtils::AddMemberVariable(LevelBlueprint, TEXT("SpawnTransform"), StructType(TBaseStructure<FTransform>::Get())))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not add the level script transform variable."));
        return false;
    }

    UK2Node_Event* const BeginPlay = NewObject<UK2Node_Event>(Graph);
    BeginPlay->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
    BeginPlay->bOverrideFunction = true;
    Graph->AddNode(BeginPlay, true, false);
    BeginPlay->CreateNewGuid();
    BeginPlay->AllocateDefaultPins();
    BeginPlay->NodePosX = 0;
    BeginPlay->NodePosY = 0;

    UK2Node_VariableSet* const Store = NewObject<UK2Node_VariableSet>(Graph);
    Store->VariableReference.SetSelfMember(TEXT("BootstrapTicks"));
    Graph->AddNode(Store, true, false);
    Store->CreateNewGuid();
    Store->AllocateDefaultPins();
    Store->NodePosX = 300;
    Store->NodePosY = 0;

    if (UEdGraphPin* const Value = Store->FindPin(TEXT("BootstrapTicks"), EGPD_Input))
    {
        Value->DefaultValue = TEXT("1");
    }

    const UEdGraphSchema_K2* const Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
    UEdGraphPin* const Then = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* const Exec = Store->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    if (Schema == nullptr || Then == nullptr || Exec == nullptr || !Schema->TryCreateConnection(Then, Exec))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not wire the level script BeginPlay body."));
        return false;
    }

    // The level-script test asserts the entry body renders the BeginPlay spawn call. Author it here
    // rather than shipping a level asset: a .umap is engine-version fragile and the plugin does not
    // otherwise need host content. The map is never played, so the call's default inputs are enough.
    UFunction* const FinishSpawning = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.GameplayStatics:FinishSpawningActor"));
    UEdGraphPin* const StoreThen = Store->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    if (FinishSpawning == nullptr || StoreThen == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: GameplayStatics::FinishSpawningActor was not found; the level script cannot be authored."));
        return false;
    }

    UK2Node_CallFunction* const SpawnCall = NewObject<UK2Node_CallFunction>(Graph);
    SpawnCall->SetFromFunction(FinishSpawning);
    Graph->AddNode(SpawnCall, true, false);
    SpawnCall->CreateNewGuid();
    SpawnCall->AllocateDefaultPins();
    SpawnCall->NodePosX = 600;
    SpawnCall->NodePosY = 0;

    // The transform parameter is a const reference, so a pin default is not enough. Feed it from a
    // level-script variable; the map is never played, so its identity default is fine.
    UK2Node_VariableGet* const TransformGet = NewObject<UK2Node_VariableGet>(Graph);
    TransformGet->VariableReference.SetSelfMember(TEXT("SpawnTransform"));
    Graph->AddNode(TransformGet, true, false);
    TransformGet->CreateNewGuid();
    TransformGet->AllocateDefaultPins();
    TransformGet->NodePosX = 420;
    TransformGet->NodePosY = 120;

    UEdGraphPin* const TransformOut = TransformGet->FindPin(TEXT("SpawnTransform"), EGPD_Output);
    UEdGraphPin* const TransformIn = SpawnCall->FindPin(TEXT("SpawnTransform"), EGPD_Input);
    if (TransformOut == nullptr || TransformIn == nullptr || !Schema->TryCreateConnection(TransformOut, TransformIn))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not wire the level script spawn transform."));
        return false;
    }

    UEdGraphPin* const SpawnExec = SpawnCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    if (SpawnExec == nullptr || !Schema->TryCreateConnection(StoreThen, SpawnExec))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not wire the level script spawn call."));
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(LevelBlueprint);
    FKismetEditorUtilities::CompileBlueprint(LevelBlueprint);
    if (LevelBlueprint->Status == BS_Error)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: the level script failed to compile."));
        return false;
    }
    return true;
}

bool EnsureTestMap()
{
    // An explicit test map means the host owns the map; never write one next to it.
    FString Override;
    if (FParse::Value(FCommandLine::Get(), TEXT("BlueprintInspectorTestMap="), Override) && !Override.IsEmpty())
    {
        UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: leaving the test map to -BlueprintInspectorTestMap=%s."), *Override);
        return true;
    }

    const FString MapFile = FPackageName::LongPackageNameToFilename(TestMapPackage, FPackageName::GetMapPackageExtension());
    const FString MarkerFile = TestMapMarkerFile(MapFile);

    bool bRegenerate = false;
    if (IFileManager::Get().FileExists(*MapFile))
    {
        FString Marker;
        int32 MarkerVersion = INDEX_NONE;
        FString MarkerEngine;
        const bool bHasMarker = FFileHelper::LoadFileToString(Marker, *MarkerFile) && FParse::Value(*Marker, TEXT("version="), MarkerVersion);
        if (bHasMarker)
        {
            // The marker means the bootstrap generated this map. Refresh it when the authored shape or
            // the engine changed. Do not load the package: a map saved by another engine cannot be
            // loaded, and loading it keeps a file handle that blocks the rewrite. A marker without an
            // engine line is from the first version of this marker and is refreshed too.
            FParse::Value(*Marker, TEXT("engine="), MarkerEngine);
            bRegenerate = MarkerVersion != TestMapAuthoringVersion || MarkerEngine != GetBlueprintEngineTag();
        }
        else
        {
            // Maps generated before the marker existed carry BootstrapTicks in their level script.
            // Anything else is host content and is left untouched.
            UWorld* const Existing = LoadObject<UWorld>(nullptr, *GetBlueprintTestMapObjectPath());
            const ULevelScriptBlueprint* const LevelBlueprint = Existing != nullptr && Existing->PersistentLevel != nullptr ? Existing->PersistentLevel->GetLevelScriptBlueprint(true) : nullptr;
            const UClass* const LevelClass = LevelBlueprint ? LevelBlueprint->GeneratedClass.Get() : nullptr;
            bRegenerate = LevelClass != nullptr && LevelClass->FindPropertyByName(TEXT("BootstrapTicks")) != nullptr;

            if (!bRegenerate)
            {
                if (Existing == nullptr)
                {
                    UE_LOG(LogBlueprintInspector, Warning, TEXT("Bootstrap: %s exists but cannot be loaded by this engine; leaving it untouched. "
                                                                "Delete it or pass -BlueprintInspectorTestMap=."),
                           TestMapPackage);
                }
                else
                {
                    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: %s exists (host map); leaving it untouched."), TestMapPackage);
                }
                return true;
            }
        }
    }

    if (IFileManager::Get().FileExists(*MapFile) && !bRegenerate)
    {
        // A matching marker means the generated map is already current; keep it.
        return true;
    }

    UWorld* const World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (World == nullptr || World->PersistentLevel == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not create %s."), TestMapPackage);
        return false;
    }

    if (bRegenerate)
    {
        TArray<UPackage*> Packages;
        if (UPackage* const ExistingPackage = FindPackage(nullptr, TestMapPackage))
        {
            Packages.Add(ExistingPackage);
        }
        if (Packages.Num() > 0 && !UPackageTools::UnloadPackages(Packages))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not unload the stale %s; delete it and retry."), TestMapPackage);
            return false;
        }
        IFileManager::Get().Delete(*MapFile, false, true, true);
        if (IFileManager::Get().FileExists(*MapFile))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not delete the stale %s; the regenerated map would not match its marker."), *MapFile);
            return false;
        }
        IFileManager::Get().Delete(*MarkerFile, false, true, true);
        if (IFileManager::Get().FileExists(*MarkerFile))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not delete the stale marker %s."), *MarkerFile);
            return false;
        }
        UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: refreshing the generated %s."), TestMapPackage);
    }

    if (!AuthorLevelScriptBeginPlay(World->PersistentLevel))
    {
        return false;
    }

    if (!UEditorLoadingAndSavingUtils::SaveMap(World, TestMapPackage))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not save %s."), TestMapPackage);
        return false;
    }

    const FString Marker = FString::Printf(TEXT("version=%d\nengine=%s\nmap=%s\n"), TestMapAuthoringVersion, *GetBlueprintEngineTag(), TestMapPackage);
    if (!FFileHelper::SaveStringToFile(Marker, *MarkerFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Bootstrap: could not write the test map marker %s; the map will be treated as host content."), *MarkerFile);
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: created %s with a level script BeginPlay body."), TestMapPackage);
    return true;
}
} // namespace

bool Run()
{
    if (!GIsEditor)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: only runs in the editor."));
        return false;
    }

    if (bInProgress)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: Run is already in progress."));
        return false;
    }
    bInProgress = true;
    ON_SCOPE_EXIT
    {
        bInProgress = false;
    };

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: starting."));

    // Clear a partially generated set so a retry starts from a known state. The graph archive is
    // separate and is never touched. Use the filesystem path here: FixtureRoot is a package mount path.
    const FString ContentDir = FixtureContentDir();
    if (ContentDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*ContentDir))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: the fixture content directory %s does not exist."), *ContentDir);
        return false;
    }
    TArray<FString> Existing;
    IFileManager::Get().FindFiles(Existing, *FPaths::Combine(ContentDir, TEXT("*.uasset")), true, false);

    // A previous run's assets can still be loaded (the startup map may reference the fixture), and
    // deleting the files does not unload them. Unload first, or CreateStruct/CreateBlueprint finds
    // the old assets and the bootstrap fails on variables that already exist.
    TArray<UPackage*> LoadedPackages;
    for (const FString& File : Existing)
    {
        const FString PackagePath = FString::Printf(TEXT("%s/%s"), *FixtureRoot, *FPaths::GetBaseFilename(File));
        if (UPackage* const Package = FindPackage(nullptr, *PackagePath))
        {
            LoadedPackages.Add(Package);
        }
    }
    if (LoadedPackages.Num() > 0 && !UPackageTools::UnloadPackages(LoadedPackages))
    {
        // Deleting the files while their packages are still loaded would leave the editor using the
        // old assets and the bootstrap failing on variables that already exist.
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: could not unload the existing fixture assets; nothing was deleted."));
        return false;
    }
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    for (const FString& File : Existing)
    {
        IFileManager::Get().Delete(*FPaths::Combine(ContentDir, File), false, true, true);
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: cleared %d existing asset(s)."), Existing.Num());

    // The suite's level-script and runtime tests need a map. Creating one switches the editor's world,
    // so do it before any fixture asset pointer is live. A missing map is not fatal for fixture
    // generation, so log the two tests it blocks instead of aborting.
    if (!EnsureTestMap())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: no test map. BlueprintInspector.Debugger.LevelScriptUbergraphRenders and "
                                                  "BlueprintInspector.Runtime.StatementMapMatchesVMStep will fail until /Game/NewMap has a "
                                                  "level blueprint with a BeginPlay body, or -BlueprintInspectorTestMap= names another map."));
    }

    UEnum* Enum = nullptr;
    UScriptStruct* Nested = nullptr;
    UScriptStruct* Struct = nullptr;
    UScriptStruct* Containers = nullptr;
    UClass* Interface = nullptr;
    UBlueprint* Parent = nullptr;
    if (!CreatePrerequisiteAssets(Enum, Nested, Struct, Containers, Interface, Parent))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: prerequisite assets failed."));
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: prerequisites created; creating fixture."));

    UBlueprint* const Fixture = GetOrCreateBlueprint(TEXT("BP_OpcodeDemo_All"), Parent ? Parent->GeneratedClass.Get() : AActor::StaticClass(), BPTYPE_Normal);
    if (Fixture == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: fixture blueprint failed."));
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: fixture blueprint created; adding variables."));

    if (!EnsureVariables(Fixture, Enum, Nested, Struct, Containers, Interface))
    {
        return false;
    }

    if (!EnsureMulticastDispatcher(Fixture, TEXT("OnProbeMulticast")))
    {
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: variables added; adding support functions."));

    if (AddFunction(Fixture, TEXT("F_ProbeLocalFunc"), {}, {}) == nullptr ||
        AddFunction(Fixture, TEXT("F_ProbeOverridable"), {}, {}) == nullptr ||
        AddFunction(Fixture, TEXT("F_ProbeDelegateHandler"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Int)}}, {}) == nullptr ||
        !AddReturnFunction(Fixture, TEXT("flow_return_value"), TEXT("7")) ||
        !AddReturnFunction(Fixture, TEXT("var_localout"), TEXT("0")) ||
        AddFunction(Fixture, TEXT("flow_return_void"), {}, {}) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: a support function could not be created."));
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: importing graph archive."));

    TArray<FString> Skipped;
    TArray<FString> Failed;
    if (!ImportGraphArchive(Fixture, Skipped, Failed))
    {
        for (const FString& File : Failed)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: failed graph %s."), *File);
        }
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: %d graph(s) failed to import; the fixture was not compiled or saved."), Failed.Num());
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: graph archive imported; authoring event graph."));

    FBlueprintOpcodeFixtureBuilder Builder(*Fixture);
    Builder.AuthorEventGraphFixtures();

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: compiling fixture."));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture);
    if (!CompileBlueprintChecked(Fixture, TEXT("BP_OpcodeDemo_All")))
    {
        return false;
    }

    if (!SaveAsset(Fixture))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: fixture save failed."));
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: %d graph(s) imported, %d skipped (multicast), %d failed, status=%d."), Fixture->FunctionGraphs.Num(), Skipped.Num(), Failed.Num(), static_cast<int32>(Fixture->Status));
    return Fixture->Status != BS_Error;
}

bool BuildTransientFixtures(UBlueprint*& OutMain, UBlueprint*& OutStructs)
{
    OutMain = nullptr;
    OutStructs = nullptr;

    if (bInProgress)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: BuildTransientFixtures is already in progress."));
        return false;
    }
    bInProgress = true;
    ON_SCOPE_EXIT
    {
        bInProgress = false;
    };

    // Build the split fixture set in a throwaway package root, without saving. The main blueprint
    // carries every archive graph except the struct rows; the struct rows live in
    // their own blueprint because a valid struct default re-instances the user structs and would reset
    // the main fixture's pins.
    const FString SavedRoot = FixtureRoot;
    const bool bSavedAssets = bSaveAssets;
    FixtureRoot = BlueprintFixtureSource::GetTransientRoot();
    bSaveAssets = false;
    ON_SCOPE_EXIT
    {
        FixtureRoot = SavedRoot;
        bSaveAssets = bSavedAssets;
    };

    UEnum* Enum = nullptr;
    UScriptStruct* Nested = nullptr;
    UScriptStruct* Struct = nullptr;
    UScriptStruct* Containers = nullptr;
    UClass* Interface = nullptr;
    UBlueprint* Parent = nullptr;
    if (!CreatePrerequisiteAssets(Enum, Nested, Struct, Containers, Interface, Parent))
    {
        return false;
    }

    UBlueprint* const Fixture = GetOrCreateBlueprint(TEXT("BP_OpcodeDemo_All"), Parent ? Parent->GeneratedClass.Get() : AActor::StaticClass(), BPTYPE_Normal);
    if (Fixture == nullptr || !EnsureVariables(Fixture, Enum, Nested, Struct, Containers, Interface) || !EnsureMulticastDispatcher(Fixture, TEXT("OnProbeMulticast")))
    {
        return false;
    }

    if (AddFunction(Fixture, TEXT("F_ProbeLocalFunc"), {}, {}) == nullptr ||
        AddFunction(Fixture, TEXT("F_ProbeOverridable"), {}, {}) == nullptr ||
        AddFunction(Fixture, TEXT("F_ProbeDelegateHandler"), {{TEXT("Value"), PinType(UEdGraphSchema_K2::PC_Int)}}, {}) == nullptr ||
        !AddReturnFunction(Fixture, TEXT("flow_return_value"), TEXT("7")) ||
        !AddReturnFunction(Fixture, TEXT("var_localout"), TEXT("0")) ||
        AddFunction(Fixture, TEXT("flow_return_void"), {}, {}) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: a support function could not be created."));
        return false;
    }

    TArray<FString> Excluded = GetBlueprintStructGraphs();

    TArray<FString> Skipped;
    TArray<FString> Failed;
    if (!ImportGraphArchive(Fixture, Skipped, Failed, {}, Excluded))
    {
        for (const FString& File : Failed)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: transient import failed for %s."), *File);
        }
        return false;
    }

    FBlueprintOpcodeFixtureBuilder Builder(*Fixture);
    Builder.AuthorEventGraphFixtures();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture);
    if (!CompileBlueprintChecked(Fixture, TEXT("the transient main fixture")))
    {
        return false;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: transient main fixture built (%d graph(s), %d skipped, %d failed, status=%d)."), Fixture->FunctionGraphs.Num(), Skipped.Num(), Failed.Num(), static_cast<int32>(Fixture->Status));

    // Derive the struct fixture from the main one. The archive's self-context variable nodes carry the
    // main blueprint's class, and a child satisfies that check while still compiling its three graphs
    // in isolation; StructVar is inherited.
    UBlueprint* const StructFixture = GetOrCreateBlueprint(TEXT("BP_OpcodeDemo_Structs"), Fixture->GeneratedClass.Get(), BPTYPE_Normal);
    OutMain = Fixture;
    if (StructFixture == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Bootstrap: transient struct fixture could not be created; its three rows will be absent."));
        return true;
    }

    TArray<FString> StructSkipped;
    TArray<FString> StructFailed;
    if (!ImportGraphArchive(StructFixture, StructSkipped, StructFailed, GetBlueprintStructGraphs()))
    {
        for (const FString& File : StructFailed)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Bootstrap: struct row failed to import: %s."), *File);
        }
        // The struct rows are optional in the split build: keep the main fixture and leave OutStructs
        // null, as documented.
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(StructFixture);
    if (!CompileBlueprintChecked(StructFixture, TEXT("the transient struct fixture")))
    {
        return true;
    }

    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: transient struct fixture built (%d graph(s), %d skipped, %d failed, status=%d)."), StructFixture->FunctionGraphs.Num(), StructSkipped.Num(), StructFailed.Num(), static_cast<int32>(StructFixture->Status));

    OutStructs = StructFixture;
    return true;
}

namespace
{
void RunCommand()
{
    UE_LOG(LogBlueprintInspector, Display, TEXT("Bootstrap: command received; scheduling."));

    // -ExecCmds runs very early in editor startup. Asset creation needs the editor and asset registry
    // to be up, so defer the work to the next core tick and let this command own the exit.
    auto Bootstrap = [](float) {
        const bool bOk = Run();
        UE_LOG(LogBlueprintInspector, Display, TEXT("BlueprintInspector.BootstrapFixture %s."), bOk ? TEXT("succeeded") : TEXT("failed"));
        // Carry the result into the process exit code so a CI/-ExecCmds run can tell a failed bootstrap
        // from a successful one.
        FPlatformMisc::RequestExitWithStatus(false, bOk ? 0 : 1);
        return false;
    };

#if BI_ENGINE_AT_LEAST_5
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(Bootstrap), 2.0f);
#else
    FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(Bootstrap), 2.0f);
#endif
}

struct FBootstrapCommandRegistration
{
    FBootstrapCommandRegistration()
    {
        if (FParse::Param(FCommandLine::Get(), TEXT("BlueprintFixtureTools")))
        {
            Command = MakeUnique<FAutoConsoleCommand>(TEXT("BlueprintInspector.BootstrapFixture"), TEXT("Create or refresh the opcode fixture assets for the running engine."), FConsoleCommandDelegate::CreateStatic(&RunCommand));
        }
    }

    TUniquePtr<FAutoConsoleCommand> Command;
};

FBootstrapCommandRegistration GBootstrapFixtureCommand;
} // namespace
} // namespace BlueprintFixtureBootstrap
#endif // WITH_EDITOR
