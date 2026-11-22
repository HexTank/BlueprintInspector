// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintFixtureSource.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "BlueprintFixtureBootstrap.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintTestConfiguration.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace BlueprintFixtureSource
{
namespace
{
const TCHAR* const TransientRoot = TEXT("/Temp/BlueprintInspectorFixture");

TStrongObjectPtr<UBlueprint> GMain;
TStrongObjectPtr<UBlueprint> GStructs;
std::atomic<bool> bBuilt{false};
std::atomic<bool> bBuilding{false};
// Set by Release(). A fixture released at shutdown must not be rebuilt, which would create UObjects
// during teardown.
std::atomic<bool> bReleased{false};

void EnsureBuilt()
{
    // Building creates assets, so it must run on the game thread; the state is published only after a
    // successful build, and a failed attempt leaves bBuilt false so a later call can retry.
    check(IsInGameThread());
    if (bBuilt.load() || bBuilding.load() || bReleased.load())
    {
        return;
    }
    bBuilding.store(true);

    UBlueprint* Main = nullptr;
    UBlueprint* Structs = nullptr;
    const bool bOk = BlueprintFixtureBootstrap::BuildTransientFixtures(Main, Structs);

    if (bReleased.load())
    {
        bBuilding.store(false);
        if (Main != nullptr)
        {
            Main->ConditionalBeginDestroy();
        }
        if (Structs != nullptr)
        {
            Structs->ConditionalBeginDestroy();
        }
        return;
    }

    if (!bOk)
    {
        bBuilding.store(false);
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture source: the transient fixtures did not build."));
        return;
    }

    // Validate the contract before publishing: a violation must leave bBuilt false so a later call can
    // retry instead of caching a null fixture for the rest of the session.
    if (Main == nullptr)
    {
        bBuilding.store(false);
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture source: the fixture builder reported success without a main fixture."));
        return;
    }

    GMain.Reset(Main);
    GStructs.Reset(Structs);
    bBuilding.store(false);
    bBuilt.store(true);

    if (Structs == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Fixture source: the transient struct fixture is missing; its three rows will be absent."));
    }
}

void AddClassFunctions(const UClass* const Class, TSet<FName>& OutSeen, TArray<UFunction*>& OutFunctions)
{
    if (Class == nullptr)
    {
        return;
    }

    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        UFunction* const Function = *It;
        if (Function == nullptr)
        {
            continue;
        }
        if (!OutSeen.Contains(Function->GetFName()))
        {
            OutSeen.Add(Function->GetFName());
            OutFunctions.Add(Function);
        }
    }
}
} // namespace

const TCHAR* GetTransientRoot()
{
    return TransientRoot;
}

UBlueprint* GetMain()
{
    EnsureBuilt();
    return GMain.Get();
}

UBlueprint* GetStructs()
{
    EnsureBuilt();
    return GStructs.Get();
}

void Release()
{
    // The plugin module calls this on shutdown. Dropping the fixtures here destroys them while the
    // UObject system is still alive, instead of leaving them to a static destructor at process exit.
    // If this ever ran off the game thread, touching the objects would be worse than leaking them, so
    // skip and report rather than assert during shutdown.
    if (!IsInGameThread())
    {
        bReleased.store(true);
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture source: release skipped because it ran off the game thread."));
        return;
    }

    GMain.Reset();
    GStructs.Reset();
    bBuilt.store(false);
    bReleased.store(true);
}

UClass* GetMainClass()
{
    UBlueprint* const Main = GetMain();
    return Main ? Main->GeneratedClass.Get() : nullptr;
}

UClass* GetPrerequisiteClass(const TCHAR* const Name)
{
    EnsureBuilt();
    if (Name == nullptr || Name[0] == TEXT('\0'))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture source: GetPrerequisiteClass was called without a name."));
        return nullptr;
    }

    const FString Path = FString::Printf(TEXT("%s/%s.%s_C"), TransientRoot, Name, Name);
    UClass* const Found = FindObject<UClass>(nullptr, *Path);
    if (Found == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Fixture source: prerequisite class %s was not built."), *Path);
    }
    return Found;
}

TArray<UFunction*> GetAllFunctions()
{
    EnsureBuilt();

    TArray<UFunction*> Functions;

    // One Seen set per class: the struct fixture's generated class derives from the main one, so a
    // same-named function there is an override and must not be dropped as a duplicate.
    TSet<FName> MainSeen;
    TSet<FName> StructSeen;
    AddClassFunctions(GMain ? GMain->GeneratedClass.Get() : nullptr, MainSeen, Functions);
    AddClassFunctions(GStructs ? GStructs->GeneratedClass.Get() : nullptr, StructSeen, Functions);
    return Functions;
}

UFunction* FindFunction(const FName Name)
{
    // The struct fixture must exist before the override-first lookup below can work.
    EnsureBuilt();

    // Resolve the struct fixture's own function first: it derives from the main fixture and overrides a
    // same-named function there.
    if (GStructs && GStructs->GeneratedClass)
    {
        for (TFieldIterator<UFunction> It(GStructs->GeneratedClass.Get(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            if (It->GetFName() == Name)
            {
                return *It;
            }
        }
    }

    if (GMain && GMain->GeneratedClass)
    {
        for (TFieldIterator<UFunction> It(GMain->GeneratedClass.Get(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            if (It->GetFName() == Name)
            {
                return *It;
            }
        }
    }
    return nullptr;
}
} // namespace BlueprintFixtureSource

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
