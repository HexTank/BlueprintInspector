#pragma once

#include "CoreMinimal.h"
#include "BlueprintInspectionTypes.h"
#include "BlueprintSourceCache.h"
#include "BlueprintSourceMap.h"

class UClass;
class UPackage;

// Capture-report identity for a class handle. The map that holds it is keyed by the stable class path.
// DisplayName is what a response shows, and ID indexes the source cache.
struct FCachedClassIdentity
{
    static constexpr int32 NoHandle = 0;

    FString DisplayName;
    int32 ID = NoHandle;
};

// Two access classes, kept strictly apart:
//  - game-thread operations (EnsureClassHandle) may walk engine state, render and issue a handle;
//  - cache reads (Find, TryCopy*) take only the cache's own lock, never render and never touch engine
//    state, so a DAP request may call them while the game thread is parked.
//
// Locking: ClassHandleLock may be held while the source cache's lock is taken, never the reverse. No
// operation holds ClassHandleLock across an engine call - EnsureClassSource resolves the class under the
// lock and renders after releasing it.
class FBlueprintClassRegistry
{
public:
    // Called on the game thread after the handle lock is released, when a handle starts naming a
    // different class. It may take registry locks.
    using FRetargetHandler = TFunction<void(const UClass*)>;
    void SetRetargetHandler(FRetargetHandler Handler);

    // Game thread: the handle for a class, issuing and binding one when the client has not listed it yet.
    // A frame that steps into an unlisted class needs the handle before the game thread parks, or its
    // first reported line falls back to 1; the render itself comes from EnsureClassSource, not from here.
    int32 EnsureClassHandle(const UClass* Class);

    // Any thread, lock only. The class a handle currently names, or an invalid pointer when it names
    // none. The weak pointer does not keep the class alive; a caller that reads it across a game-thread
    // operation must pin it for that scope.
    TWeakObjectPtr<const UClass> FindClass(int32 Handle) const;
    // Any thread, lock only. The handle bound to a bare class name, or 0 when no handle is bound.
    int32 FindHandleByName(FName ClassName) const;

    // Any thread, lock only. Never renders; a returned copy cannot be invalidated by a later store.
    TOptional<FBlueprintRenderedSource> Find(int32 Handle) const;
    bool TryCopySource(int32 Handle, FString& OutText) const;
    bool TryCopyMap(int32 Handle, FBlueprintSourceMap& OutMap) const;
    bool TryCopySourceMap(const FBlueprintSourceIdentity& Identity, FBlueprintSourceMap& OutMap) const;

    // Game thread: the render for a handle, replacing the previous one.
    void Store(int32 Handle, FBlueprintRenderedSource&& Rendered);

    // Game thread: renders a class's source on first use, then serves the cache. A handle that names no
    // class, or a class that has been collected, yields an empty result.
    TOptional<FBlueprintRenderedSource> EnsureClassSource(int32 Handle);

    // Game thread: the rendered text for a handle, empty when it cannot be rendered.
    FString AcquireClassFunctions(int32 Handle);

    // Game thread: every Blueprint class, with a handle per class. PIEPackage is the play-in-editor
    // world's package, or null outside PIE: the editor class and the PIE duplicate share a name, and the
    // PIE class wins so the source map and its function pointers belong to the class the VM is running.
    TArray<FFunctionProperties> AcquireClassList(const UPackage* PIEPackage);

    // Game thread: the render for an identity, listing classes on demand when the name is not known yet.
    //
    // Identity precedence: a non-empty ClassName wins and SourceReference is ignored; a reference that
    // disagrees with the name is logged and the named class is served.
    TOptional<FBlueprintRenderedSource> EnsureClassSourceFor(const FBlueprintSourceIdentity& Identity, const UPackage* PIEPackage);

    // Game thread: every compiled Blueprint class, with no handles issued. The audit walks these.
    TArray<UObject*> GatherBlueprintClasses() const;

    // Any thread, lock only: the handles issued so far. Each out parameter is cleared first.
    void CopyCachedClassList(TArray<FFunctionProperties>& OutFunctions) const;
    void CopyCachedClassIdentities(TMap<FName, FCachedClassIdentity>& OutByIdentity) const;

private:
    static void GatherAllClasses(TArray<UObject*>& OutClasses, EClassFlags InClassHasAllFlags = CLASS_None, EFunctionFlags InFunctionHasAllFlags = FUNC_None, EFunctionFlags InFunctionDoesNotHaveAllFlags = FUNC_None);

    // Every script function of the class, without its parents, optionally filtered by function flags.
    static void GatherAllFunctionsFromClass(const UClass* InClass, TArray<UObject*>& OutFunctions, EFunctionFlags InFunctionHasAllFlags = FUNC_None, EFunctionFlags InFunctionDoesNotHaveAllFlags = FUNC_None);

    // Called with the handle lock held: drops the render cached for a handle, because it describes the
    // class the handle used to name.
    void InvalidateClassHandle(int32 Handle);

    mutable FCriticalSection ClassHandleLock;

    // A number is issued once and never reused, so a client may hold one across requests or an editor
    // reload; the class it names can be retargeted when a PIE duplicate of the same name replaces the
    // editor class. A handle whose class was collected is pruned when the class list is next built.
    TMap<int32, TWeakObjectPtr<const UClass>> ClassHandles;
    TMap<FName, int32> ClassHandleByName;
    int32 NextClassHandle = 1;

    FBlueprintSourceCache SourceCache;

    FRetargetHandler OnRetargeted;
};
