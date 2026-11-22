#pragma once

#include "CoreMinimal.h"
#include "BlueprintSourceMap.h"

/**
 * Turns the renderer's location records into the line map for the text it produced. The renderer records a
 * location at the builder offset where a statement or structural line begins; line numbers are assigned
 * here in a post-pass because EX_SwitchValue splices newlines into the top-level stream, which a live
 * counter would miss. Game thread only: every entry point asserts IsInGameThread().
 */
class FBlueprintSourceMapBuilder
{
public:
    // Drops all recorded locations. Build() consumes the records, so this is only needed to discard a
    // render that is abandoned before it is built.
    void Reset();

    // Precondition: Count >= 0 (checked in development builds).
    void Reserve(int32 Count);

    // Offset is the character index in the rendered text where this line begins. Records must be added in
    // nondecreasing offset order, which rendering guarantees; a violation is dropped and logged because
    // Build()'s single forward cursor would otherwise mis-attribute every later line.
    void AddLocation(int32 Offset, const FBlueprintLineInfo& Info);

    // Drops every record at or after Offset. A renderer that removes the text a record describes (the
    // implicit trailing "return;" is pruned before the closing brace) must call this with the offset the
    // removed text began at, or the map keeps a location for text that is no longer in the output.
    void TrimFrom(int32 Offset);

    // One entry per line of InText. A line inherits the last record at or before its first character.
    // An inherited Statement becomes SubStatement (same address, not the line that address reports);
    // an inherited Sequence/Goto/LoopTransfer becomes None and drops its address/arm, because those
    // records only cover their own line; an uncovered line stays non-breakpointable.
    //
    // Consumes the records: the builder is empty afterwards, so a reused builder cannot carry the
    // previous render's offsets into the next map.
    [[nodiscard]] FBlueprintSourceMap Build(const FString& InText);

    [[nodiscard]] int32 Num() const { return Locations.Num(); }

private:
    struct FLocation
    {
        int32 Offset = 0;
        FBlueprintLineInfo Info;
    };

    TArray<FLocation> Locations;
    bool bLoggedNegativeOffset = false;
    bool bLoggedOutOfOrderOffset = false;
};
