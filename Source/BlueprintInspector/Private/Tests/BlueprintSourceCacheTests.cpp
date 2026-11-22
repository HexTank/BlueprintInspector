#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "BlueprintSourceCache.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSourceCacheTest, "BlueprintInspector.SourceCache.StoreReplaceInvalidate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSourceCacheTest::RunTest(const FString& Parameters)
{
    FBlueprintSourceCache Cache;
    FString Text;
    FBlueprintSourceMap Map;

    TestFalse(TEXT("an empty cache misses text"), Cache.TryCopy(1, Text));
    TestFalse(TEXT("an empty cache misses the map"), Cache.TryCopyMap(1, Map));
    TestFalse(TEXT("an empty cache has no entry"), Cache.Find(1).IsSet());

    FBlueprintRenderedSource First;
    First.Text = TEXT("first;\n");
    First.Map.Lines.Add(FBlueprintLineInfo{});
    Cache.Store(1, MoveTemp(First));

    TestTrue(TEXT("the stored text is returned"), Cache.TryCopy(1, Text));
    TestEqual(TEXT("the stored text matches"), Text, FString(TEXT("first;\n")));
    TestTrue(TEXT("the stored map is returned"), Cache.TryCopyMap(1, Map));
    TestEqual(TEXT("the stored map has one line"), Map.Lines.Num(), 1);
    TestTrue(TEXT("the entry can be found"), Cache.Find(1).IsSet());

    FBlueprintRenderedSource Second;
    Second.Text = TEXT("second;\n");
    Cache.Store(1, MoveTemp(Second));
    TestTrue(TEXT("the replacement is returned"), Cache.TryCopy(1, Text));
    TestEqual(TEXT("the replacement text matches"), Text, FString(TEXT("second;\n")));
    TestTrue(TEXT("the replacement map is returned"), Cache.TryCopyMap(1, Map));
    TestEqual(TEXT("the replacement drops the old lines"), Map.Lines.Num(), 0);

    FBlueprintRenderedSource Sibling;
    Sibling.Text = TEXT("sibling;\n");
    Cache.Store(3, MoveTemp(Sibling));

    Cache.Invalidate(1);
    Text = TEXT("stale");
    Map.Lines.Add(FBlueprintLineInfo{});
    TestFalse(TEXT("an invalidated entry misses text"), Cache.TryCopy(1, Text));
    TestEqual(TEXT("a miss clears the reused text"), Text, FString());
    TestFalse(TEXT("an invalidated entry misses the map"), Cache.TryCopyMap(1, Map));
    TestEqual(TEXT("a miss clears the reused map"), Map.Lines.Num(), 0);
    TestFalse(TEXT("an invalidated entry is gone"), Cache.Find(1).IsSet());
    TestTrue(TEXT("invalidating one entry keeps another"), Cache.TryCopy(3, Text));
    TestEqual(TEXT("the surviving entry keeps its text"), Text, FString(TEXT("sibling;\n")));

    FBlueprintRenderedSource Third;
    Third.Text = TEXT("third;\n");
    Cache.Store(2, MoveTemp(Third));
    Cache.Clear();
    TestFalse(TEXT("clear drops every entry"), Cache.TryCopy(2, Text));
    TestFalse(TEXT("clear leaves the cache empty"), Cache.Find(2).IsSet());
    TestFalse(TEXT("clear drops the entry that outlived an invalidate"), Cache.TryCopy(3, Text));
    return true;
}

#endif
