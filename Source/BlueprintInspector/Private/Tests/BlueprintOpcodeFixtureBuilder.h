#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

class UBlueprint;
class UEdGraph;

// Lays out the finite flow fixtures in BP_OpcodeDemo_All with variable-driven conditions, so both arms
// of every branch and every loop iteration execute at runtime. Every Author* call clears the target
// graph first, so running the fixture tool twice leaves the same asset. Holds a raw reference to the
// blueprint; the caller keeps it alive for the builder's lifetime.
class FBlueprintOpcodeFixtureBuilder
{
public:
    explicit FBlueprintOpcodeFixtureBuilder(UBlueprint& InBlueprint);

    // Non-owning, identity-bearing helper: a copy would silently act on the same asset with its own
    // cached state, so copying/moving is deleted.
    FBlueprintOpcodeFixtureBuilder(const FBlueprintOpcodeFixtureBuilder&) = delete;
    FBlueprintOpcodeFixtureBuilder& operator=(const FBlueprintOpcodeFixtureBuilder&) = delete;
    FBlueprintOpcodeFixtureBuilder(FBlueprintOpcodeFixtureBuilder&&) = delete;
    FBlueprintOpcodeFixtureBuilder& operator=(FBlueprintOpcodeFixtureBuilder&&) = delete;

    // The fixture's function graph, created (and registered in the Blueprint's FunctionGraphs) when the
    // asset does not have it yet. The result is a non-owning pointer into the Blueprint, or null when the
    // graph cannot be created.
    UEdGraph* FindOrCreateGraph(const TCHAR* GraphName);

    // Pulls the graph to the origin and caps the gap between neighbouring columns and rows, so a
    // generated fixture stays inside a window instead of linking across the old hand-layout canvas.
    void CompactLayout(UEdGraph& Graph);

    // The loops need a second boolean so the nested fixture can drive an inner and an outer flag.
    bool EnsureLoopFlag();

    // Replaces the fixture's single-cast delegate variable with one typed by the native signature on
    // UOpcodeProbeHost; a Blueprint-authored signature does not survive the asset being saved.
    bool EnsureDelegateVariable();

    // Installs EnumVar as PC_Byte plus the E_OpcodeDemoEnum object. The schema converts its UI "enum"
    // category to PC_Byte; storing PC_Enum directly is what blanked the sibling bodies.
    bool EnsureEnumVariable();

    // Restores bIsWeakPointer on WeakObjVar and on the destination pin in let_weakobj. AddMemberVariable
    // clears the flag for user-created variables, and the exported graph text carries the pin flag only.
    // Restore-only: returns false (with an error log) when the bootstrap has not authored WeakObjVar; a
    // missing let_weakobj graph is logged but does not fail.
    bool EnsureWeakVariable();

    // Brings E_OpcodeDemoEnum up to the layout guide's Three values and saves it. The shared asset is only
    // dirty and saved when a display name actually changed.
    bool EnsureEnumAsset();

    // Adds SingleDelegate(Del) to BFL_ProbeLibrary and saves it. A delegate parameter default is the
    // only producer of EX_InstanceDelegate.
    bool EnsureDelegateLibraryFunction();

    // Each Author* call clears the target graph and rebuilds it, so a second run leaves the same asset.
    // The matching Ensure* setup must have run first: EnsureLoopFlag before the loop rows,
    // EnsureDelegateVariable before AuthorLetDelegate and AuthorDelegateCreateDelegate,
    // EnsureEnumVariable plus EnsureEnumAsset before AuthorLetEnum, and the bootstrap's IntVar, BoolVar
    // and ActorVar must already exist. Without them the authoring fails late on a missing pin.
    bool AuthorBranch(UEdGraph& Graph);
    bool AuthorWhile(UEdGraph& Graph);
    bool AuthorDoWhile(UEdGraph& Graph);
    bool AuthorContinue(UEdGraph& Graph);
    bool AuthorBreak(UEdGraph& Graph);
    bool AuthorNestedLoops(UEdGraph& Graph);
    bool AuthorSwitch(UEdGraph& Graph);
    bool AuthorCallerArgument(UEdGraph& Graph);
    bool AuthorContextFailSilent(UEdGraph& Graph);
    bool AuthorLetDelegate(UEdGraph& Graph);
    bool AuthorDelegateCreateDelegate(UEdGraph& Graph);
    bool AuthorLetEnum(UEdGraph& Graph);
    bool AuthorDelegateInstance(UEdGraph& Graph);

    // Rebuilds the ubergraph's first page with the ubergraph-only fixtures: a custom event with a data
    // output, and a second event that only chains to a set. The default Actor events are replaced, and the
    // method returns false when the blueprint has no ubergraph page.
    bool AuthorEventGraphFixtures();

private:
    UBlueprint& Blueprint;
};

#endif // WITH_EDITOR
