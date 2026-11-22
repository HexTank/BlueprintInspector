// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "BlueprintSourceMap.h"
#include "HAL/PlatformTime.h"

#include "GameFramework/Actor.h"

// A benchmark that runs with the suite. It reports ns/op through AddInfo and asserts the cached path
// returns what rebuilding it returns.
//
// The absolute numbers belong to this build, while the cached/uncached ratio is the point. The cache
// exists to keep an FString allocation off the VM's per-step path, so the comparison is against the
// implementation it replaced, reproduced here.
namespace
{
FName BuildClassPathUncached(const UClass* const Class)
{
    if (Class == nullptr)
    {
        return NAME_None;
    }

    FString Path = Class->GetPathName();
    StripPiePackagePrefixesInline(Path);
    return FName(*Path);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintClassPathBenchmark, "BlueprintInspector.Performance.ClassPathCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintClassPathBenchmark::RunTest(const FString& Parameters)
{
    const UClass* const Class = AActor::StaticClass();

    constexpr int32 Warmup = 5000;
    constexpr int32 Iterations = 200000;
    constexpr int32 Rounds = 3;

    // Consuming the result through a volatile comparison stops either loop being elided; comparing
    // against an unrelated name is cheaper than materialising a string and is not what is measured.
    // The read and write are spelled out because C++20 deprecates compound assignment on a volatile.
    const FName Unrelated(TEXT("__BlueprintInspectorBenchmarkSink"));
    volatile uint32 Sink = 0;

    for (int32 Index = 0; Index < Warmup; ++Index)
    {
        uint32 Hit = GetClassPath(Class) == Unrelated ? 1u : 0u;
        Sink = Sink + Hit;
        Hit = BuildClassPathUncached(Class) == Unrelated ? 1u : 0u;
        Sink = Sink + Hit;
    }

    const auto TimeCached = [&]() {
        const double Start = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Iterations; ++Index)
        {
            const uint32 Hit = GetClassPath(Class) == Unrelated ? 1u : 0u;
            Sink = Sink + Hit;
        }
        return FPlatformTime::Seconds() - Start;
    };

    const auto TimeUncached = [&]() {
        const double Start = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < Iterations; ++Index)
        {
            const uint32 Hit = BuildClassPathUncached(Class) == Unrelated ? 1u : 0u;
            Sink = Sink + Hit;
        }
        return FPlatformTime::Seconds() - Start;
    };

    double CachedSeconds = TNumericLimits<double>::Max();
    double UncachedSeconds = TNumericLimits<double>::Max();
    for (int32 Round = 0; Round < Rounds; ++Round)
    {
        if (Round % 2 == 0)
        {
            CachedSeconds = FMath::Min(CachedSeconds, TimeCached());
            UncachedSeconds = FMath::Min(UncachedSeconds, TimeUncached());
        }
        else
        {
            UncachedSeconds = FMath::Min(UncachedSeconds, TimeUncached());
            CachedSeconds = FMath::Min(CachedSeconds, TimeCached());
        }
    }

    // Consume the accumulator once, after both timed loops, so the compiler cannot elide the updates
    // without paying a volatile read-modify-write on every iteration.
    volatile uint32 SinkOut = Sink;
    (void)SinkOut;

    const double CachedNs = CachedSeconds * 1.0e9 / Iterations;
    const double UncachedNs = UncachedSeconds * 1.0e9 / Iterations;
    const double Ratio = CachedNs > 0.0 ? UncachedNs / CachedNs : 0.0;

    AddInfo(FString::Printf(TEXT("GetClassPath: cached %.1f ns/op, uncached %.1f ns/op, %d iterations, ratio %.2fx"), CachedNs, UncachedNs, Iterations, Ratio));

    TestTrue(TEXT("the cached path returns what rebuilding it returns"), GetClassPath(Class) == BuildClassPathUncached(Class));
    TestEqual(TEXT("the cached path names the class"), GetClassPath(Class).ToString(), FString(TEXT("/Script/Engine.Actor")));

    FString PieClassPath(TEXT("/Game/BP/UEDPIE_0_Foo.UEDPIE_0_Foo_C"));
    StripPiePackagePrefixesInline(PieClassPath);
    TestEqual(TEXT("the PIE prefix is stripped from the package and object names"), PieClassPath, FString(TEXT("/Game/BP/Foo.Foo_C")));

    FString NearPrefix(TEXT("/Game/BP/UEDPIE_Foo"));
    StripPiePackagePrefixesInline(NearPrefix);
    TestEqual(TEXT("a UEDPIE_ that is not followed by digits and an underscore is left alone"), NearPrefix, FString(TEXT("/Game/BP/UEDPIE_Foo")));
    return true;
}

#endif
