#pragma once

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "BlueprintInspectorPort.h"

// Expectations that depend on the engine are scoped by this tag, "<major>.<minor>": the fixture plan
// carries per-engine override rows next to the base CSV, and the golden test reads
// "<name>.<tag>.golden" when that file exists.
inline FString GetBlueprintEngineTag()
{
    return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
}

// flow_switchvalue now compiles from the transient fixture. No graph is sourced from a saved engine
// asset, so every engine can build the full set in memory.
inline TArray<FString> GetBlueprintStructGraphs()
{
    return {TEXT("const_struct"), TEXT("structconst_containers"), TEXT("let_struct")};
}

// Graphs the fixture bootstrap cannot author on the running engine. The bootstrap skips exactly this
// list, so a fixture it generates lacks these functions and the token plan must not read the absence
// as a regression. UE 4.25-5.2 cannot resolve the multicast dispatcher signature in the archive export
// and also reject the container pin default and the delegate parameter, so the whole set is skipped
// there; 5.3+ imports all of them from the same archive (see the 5.3+ golden files).
inline TArray<FString> GetBlueprintGraphsTheBootstrapSkips()
{
    TArray<FString> Graphs;
#if !BI_ENGINE_AT_LEAST_5_3
    // The four delegate rows stay skipped even where the bootstrap authors the dispatcher.
    Graphs.Add(TEXT("delegate_addelegate"));
    Graphs.Add(TEXT("delegate_callmulticast"));
    Graphs.Add(TEXT("delegate_cleardelegate"));
    Graphs.Add(TEXT("delegate_removedelegate"));
    Graphs.Add(TEXT("arrayconst_paramdefault"));
    Graphs.Add(TEXT("delegate_instance"));
#endif
    return Graphs;
}

// Fixtures that cannot exist on the running engine, as the CSV graph names the token test compares.
// Pinning the set makes an unexpected absence fail there.
inline TArray<FString> GetBlueprintKnownMissingFixtures()
{
    // These four are engine-independent: the fixture set never authors those rows (they are absent from
    // the layout guide/archive on every supported engine), so their absence is expected everywhere. Only
    // the bootstrap-skipped set below is 4.25-specific.
    TArray<FString> Missing = {
        TEXT("F_let_multicast"),
        TEXT("F_setconst_paramdefault"),
        TEXT("F_mapconst_paramdefault"),
        TEXT("F_delegate_assigndelegateset"),
    };
#if !BI_ENGINE_AT_LEAST_5_3
    // A fixture built on these engines is bootstrap-generated, so everything the bootstrap skips is
    // absent as well.
    for (const FString& Graph : GetBlueprintGraphsTheBootstrapSkips())
    {
        Missing.Add(FString::Printf(TEXT("F_%s"), *Graph));
    }
#endif
    return Missing;
}

// Kismet.bCanSuppressAccessViolation is read once per process, at the first context emission, so the
// command line has to carry it; setting it from a test is too late. The fixture bytecode records
// EX_Context_FailSilent only when the compiler sees the flag.
inline bool CanSuppressAccessViolation()
{
    static const bool bConfigured = []() {
        bool bValue = false;
        GConfig->GetBool(TEXT("Kismet"), TEXT("bCanSuppressAccessViolation"), bValue, GEngineIni);
        return bValue;
    }();
    return bConfigured;
}

inline const TCHAR* FailSilentCommandLineSwitch()
{
    return TEXT("-ini:Engine:[Kismet]:bCanSuppressAccessViolation=true");
}

// The map the level-script and runtime tests drive. It defaults to the project's NewMap; pass
// -BlueprintInspectorTestMap=/Game/Other/Map to run the tests against another map with a level script.
inline FString GetBlueprintTestMapPath()
{
    FString Path;
    if (FParse::Value(FCommandLine::Get(), TEXT("BlueprintInspectorTestMap="), Path) && !Path.IsEmpty())
    {
        // Normalise once so every consumer sees the same package form: drop an object suffix and any
        // trailing separator. The object-path helper then only appends the object name.
        int32 DotIndex = INDEX_NONE;
        if (Path.FindChar(TEXT('.'), DotIndex))
        {
            Path.LeftInline(DotIndex);
        }
        while (Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        if (!Path.IsEmpty())
        {
            return Path;
        }
    }
    return TEXT("/Game/NewMap");
}

inline FString GetBlueprintTestMapObjectPath()
{
    // Accept the documented package-path form regardless of a trailing separator or an object suffix:
    // '/Game/Other/Map/', '/Game/Other/Map.Map' and '/Game/Other/Map' all name the same package.
    const FString PackagePath = GetBlueprintTestMapPath();
    return FString::Printf(TEXT("%s.%s"), *PackagePath, *FPaths::GetBaseFilename(PackagePath));
}
