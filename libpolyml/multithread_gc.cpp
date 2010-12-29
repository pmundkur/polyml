/*
    Title:      Multi-Threaded Garbage Collector

    Copyright (c) 2010 David C. J. Matthews

    Based on the original garbage collector code
        Copyright 2000-2008
        Cambridge University Technical Services Limited

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h> // Used in both Windows and Cygwin
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h> // For sysconf
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#include "globals.h"
#include "diagnostics.h"
#include "timing.h"
#include "gc.h"
#include "check_objects.h"
#include "bitmap.h"
#include "memmgr.h"
#include "gctaskfarm.h"

// Settings moved from userOptions.
static unsigned long    immutableSegSize, mutableSegSize;
static unsigned long    immutableFreeSpace, mutableFreeSpace;
static unsigned long    immutableMinFree, mutableMinFree; // Probably remove

static bool dontFreeSpace; // Temporary for testing.

static GCTaskFarm gTaskFarm; // Global task farm.
GCTaskFarm *gpTaskFarm = &gTaskFarm;

/*
    How the garbage collector works.
    The GC is generational.  There are two modes: minor and full.  Most of the
    code is the same for both.  There are two types of local heap: mutable and
    immutable.  ML and RTS code always allocate new objects in a mutable heap.
    Note allocation is from the top of the area down.
    Originally, there were just two areas but now there may be multiple
    heap segments of each type.  The GC has three phases:

    1.  Mark phase.
    Working from the roots; which are the the permanent mutable segments, the RTS
    roots (e.g. thread stacks) and, if this is a minor collection, mutable objects
    collected in previous collections ("gen_top" to "top"), mark all reachable cells.
    Marking involves setting bits in the bitmap for reachable words.  If this is a
    minor collection we only follow cells that are in the current generation
    ("gen_bottom" to "gen_top").

    2. Compact phase.
    Marked objects are copied to try to compact, upwards, the heap segments.  When
    an object is moved the length word of the object in the old location is set as
    a tombstone that points to its new location.  In particular this means that we
    cannot reuse the space where an object previously was during the compaction phase.
    Immutable objects are moved into immutable segments.  When an object is moved
    to a new location the bits are set in the bitmap as though the object had been
    marked at that location.

    3. Update phase.
    The roots and objects marked during the first two phases are scanned and any
    addresses for moved objects are updated.  The lowest address used in the area
    then becomes the base of the area for future allocations.

    Typically, a minor GC moves immutable data into the immutable area and leaves
    mutable data behind.  The immutable data moved is considered "old" and not
    scanned until a major collection.  However, if a collection finds that there
    are significant holes in the heap (these holes must be in the mutable area)
    it is better to try to recollect the current generation.  In this case the
    immutable data moved during this collection are considered as "new" in the
    next minor collection.  Even though we're only concerned there with compacting
    the mutable area we have to process immutable objects that may contain their
    addresses.
    DCJM 27/6/09
*/

/*
  The comments below may still be relevant.  I've left them in because they
  contain notes about optimisations that were tried in the past and no longer
  apply.

  How the garbage collector works.
  
  Phase 1: Starting from the roots in the old mutable area, and
           any pointers kept by the runtime system, we mark all
           objects that are found within the two GC areas.
           The GC areas extend from the allocation pointer
           to the top of the area.
  
  Phase 2: Then we scan the immutable object bitmap. When we find
           a mutable object we try to find space for it in the mutable
           area, if we find an immutable object we try to find space
           for it further up the immutable area. We may have to extend
           the mutable area to make room for objects since we must not commit
           and leave mutable objects in the immutable area.
           
           Then we do the same for the mutable area, copying immutable objects
           out into the immutable area, and moving mutable objects up.
           
           We keep track of the lowest object that could not be moved.
           The allocation pointers will be reset to the lowest kept objects,
           and the area below is taken to be free.
  
  Phase 3: Then we start from the roots and runtime system objects and
           look for pointers into the GC areas that point to tombstones.
           These pointers are changed to point to the new position of
           the objects. Then we process all the objects in the areas
           doing the same thing.
           
Further notes:

  The order of processing the immutable and mutable area has been changed
  since the above comment was written (by Dave Matthews?).

  It would be nice to combine phases 2 and 3 - we could traverse the
  reachable data-structures, starting at the roots, adjusting pointers
  as we go (rather like copyGC). We would only use the bitmap created
  in phase 1 to tell us where to find space to move the new objects.
  
  The main advantage of this approach is that is likely to be
  quicker - we only have to traverse the new (small?) data-structure
  rather than scanning the (large) mutable buffer.
  
  The disadvantage is that it would leave part of the heap dirty,
  and I think parts of the RTS may depend on any unused heap
  word containing zeroes. I'll have to look at this very closely!
  Note that this is a different issue from the compiler requiring
  the area below the allocation area to be zeroed. (Should we
  fix this?) Here we are talking about the area *above* the
  allocation pointer, which may contain objects, tombstones
  and zero words only.
  
  A second disadvantage is that the "compress" pass may not give
  as good compression as currently, because it wouldn't explicitly
  start at the bottom and work up. In recompense, we would be able
  to recycle all but the length word of a tombstone, so our
  actual space usage might improve.
  
  SPF 21/10/96
  
  I've now deleted all that carefully optimised code that used to zero the
  heap - it's now the responsibility of the compiler (and alloc) to ensure
  that the store is correctly initialised whenever a GC might occur.
  
  SPF 22/10/96
  
  The GC is required to "clean" each area of the heap between pointer and top;
  this area may only contain objects, tombstones and zero words. The GC
  currently does this for the benefit of OpMutableBlock, but this behaviour
  is also required for the PrintLocalProfileCounts in run_time.c to avoid
  core dumps.
  
  SPF 23/10/96
  
  Let's try to improve the design of the garbage collector, by doing partial GCs
  in 5 phases:
  
     (1) Mark
     (2) CopyImmutables
     (3) FixupImmutable
     (4) CopyMutables
     (5) FixupMutables

   What are the advantages/disadvantages of the new approach?
      
       Advantage:
      
           We can copy mutables into the holes left by copying-out immutables,
           which gives better compaction of the mutable area. The inability
           to do this is currently a problem for some applications because
           it triggers far too many full GCs.
        
       Disadvantage:
      
           We have to run the copy and fix-up phases twice. This may be expensive.
        
   Can we get the advantage without the disadvantage by only splitting the Copy
   and Fixup phases when this looks like a win?
   
   Note: we have to separate the Mark and Copy phases, as otherwise we won't be
   able to handle weak pointers. Shame!
   
   SPF 17/12/1997
*/

/* start <= val <= end */
#define INSOFTRANGE(val,start,end) ((start) <= (val) && (val) <= (end))

// Try to allocate another heap segment.  It tries to allocate the requested size
// but if that fails it allocates what it can.
static bool TryMoreHeap(POLYUNSIGNED size, bool mut)
{
    if (userOptions.debug & DEBUG_NOGROW) return false; // No heap growing.

    do {
        // Return if this succeeded.
        if (gMem.NewLocalSpace(size, mut))
            return true;
        // Otherwise try with half the size and stop when
        // it's less than 64k words.
        size = size / 2;
    } while (size > 64*1024);

    return false;
}

/* The problem with this version of PossiblyExpandArea is that it doesn't always expand
   it enough for the subsequent compaction phase to actually liberate wordsRequiredToAllocate
   of free space. SPF 31/7/96
*/

// This function is called after the mark phase of a full garbage collection to
// expand the immutable area if necessary.  wordsNeeded is the amount of immutable
// data detected during the mark phase.
static void PossiblyExpandImmutableArea(const POLYUNSIGNED wordsNeeded)
{
    POLYUNSIGNED currentSize = 0;
    unsigned nISpaces = 0; // Number of immutable spaces already
    for (unsigned j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *space = gMem.lSpaces[j];
        if (! space->isMutable)
        {
            currentSize += space->top - space->bottom;
            nISpaces++;
        }
    }
    
    if (immutableFreeSpace + wordsNeeded > currentSize) // need to get some more space
    {
        // We want to ensure that we have immutableFreeSpace free after this
        // collection.  We allocate in units of immutableSegSize so as not to
        // have too many small segments.
        POLYUNSIGNED requestedGrowth = immutableFreeSpace + wordsNeeded - currentSize;
        if (requestedGrowth < immutableSegSize)
            requestedGrowth = immutableSegSize;
        // Make the segments larger if we have already allocated several.
        // The factors here are a guess.  Maybe tune them more carefully
        unsigned spaceFactor = nISpaces / 3;
        while (spaceFactor > 0) { requestedGrowth += immutableSegSize; spaceFactor--; }

        POLYUNSIGNED chunks  = ROUNDUP_UNITS(requestedGrowth, BITSPERWORD);
        POLYUNSIGNED words   = chunks * BITSPERWORD;

        (void)TryMoreHeap(words, false); // If this fails just carry on with what we have.
    }
}


/* This function CHECKS whether we have enough space AFTER the compaction phase. */
static bool BufferIsReallyFull(bool mutableRegion, POLYUNSIGNED wordsNeeded, const bool fullGC)
{
    // This is the space we need to be free.  If this is a mutable area wordsNeeded is the
    // space needed to allocate the object whose attempted allocation triggered this collection.
    // It needs to be available in at least one mutable area.  If this is an immutable area
    // wordsNeeded is the amount of space needed for immutable objects that couldn't be copied
    // out of the mutable area so doesn't need to be contiguous.
    POLYUNSIGNED requiredFree;
    if (mutableRegion)
        requiredFree = fullGC ? mutableFreeSpace: mutableMinFree;
    else
    {
        requiredFree = fullGC ? immutableFreeSpace: immutableMinFree;
        requiredFree += wordsNeeded;
        wordsNeeded = 0;
    }

    for (unsigned i = 0; i < gMem.nlSpaces; i++)
    {
        LocalMemSpace *space = gMem.lSpaces[i];
        if (space->isMutable == mutableRegion)
        {
            POLYUNSIGNED currentlyFree = space->pointer - space->bottom;
            if (currentlyFree >= wordsNeeded)
            {
                currentlyFree -= wordsNeeded;
                wordsNeeded = 0;
            }
            if (currentlyFree >= requiredFree)
                requiredFree = 0;
            else requiredFree -= currentlyFree;
        }
    }
    return wordsNeeded != 0 || requiredFree != 0;
}

// AFTER a full GC, make sure we have a full buffer's worth of free space available.
static void AdjustHeapSize(bool isMutableSpace, POLYUNSIGNED wordsRequired)
{
    POLYUNSIGNED currentSize = 0, currentlyFree = 0;
    unsigned nSpaces = 0;
    POLYUNSIGNED largestFree = 0;
    for (unsigned j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *space = gMem.lSpaces[j];
        if (space->isMutable == isMutableSpace)
        {
            POLYUNSIGNED spaceSize = space->top - space->bottom;
            POLYUNSIGNED spaceFree = space->pointer - space->bottom;
            currentSize += spaceSize;
            currentlyFree += spaceFree;
            if (largestFree < spaceFree) largestFree = spaceFree;
            nSpaces++;
        }
    }
    
    const POLYUNSIGNED requiredFree = wordsRequired +
        (isMutableSpace ? mutableFreeSpace : immutableFreeSpace);
    
    /* Basic sanity checks. */
    ASSERT(0 <= wordsRequired);
    ASSERT(0 <= requiredFree);
    
    // We may be trying to allocate a very large object, e.g. a new stack segment, in
    // which case we must ensure that we have enough space in at least one space.
    // Otherwise we just check we have enough free overall.
    if (requiredFree > currentlyFree || (isMutableSpace && largestFree < wordsRequired))
    {    // expand the heap.
        POLYUNSIGNED requestedGrowth = requiredFree - currentlyFree;
        const POLYUNSIGNED segSize =
            isMutableSpace ? mutableSegSize : immutableSegSize;
        if (requestedGrowth < segSize)
            requestedGrowth = segSize;
        // Make the segments larger if we have already allocated several.
        // The factors here are a guess.  Maybe tune them more carefully
        unsigned spaceFactor = nSpaces / 3;
        while (spaceFactor > 0) { requestedGrowth += segSize; spaceFactor--; }
        if (requestedGrowth < wordsRequired) requestedGrowth = wordsRequired;

        POLYUNSIGNED chunks  = ROUNDUP_UNITS(requestedGrowth, BITSPERWORD);
        POLYUNSIGNED words   = chunks * BITSPERWORD;

        (void)TryMoreHeap(words, isMutableSpace); // If this fails just carry on with what we have.
    }
    else if (! dontFreeSpace) // currentlyFree >= requiredFree
    {
        // The reason for shrinking the stack is to reduce the swap space and
        // possibly the address space requirements.  This may be necessary if
        // we have finished building a large data structure and now want to
        // export it.  The export code requires buffer space and may need the
        // space we're using.
        // Another reason is to get rid of old saved state areas that have been
        // converted into local areas.  These are likely to be small compared with the
        // heap and result in fragmentation of the address space.
        // TODO: We should perhaps deallocate small areas even if that would bring
        // us under the limit because it would be better to reallocate a larger area.
        POLYUNSIGNED requestedShrink = currentlyFree - requiredFree;
        // Delete the most recent space first.
        for (unsigned k = gMem.nlSpaces; k > 0; k--)
        {
            LocalMemSpace *space = gMem.lSpaces[k-1];
            if (space->isMutable == isMutableSpace &&
                space->pointer == space->top /* It's completely empty */ &&
                (POLYUNSIGNED)(space->top - space->bottom) <= requestedShrink)
            {
                // We can free this space without going under our limit
                requestedShrink -= space->top - space->bottom;
                gMem.DeleteLocalSpace(space);
            }
        }
    }
}

static int RecollectThisGeneration(unsigned thisGeneration)
{
    if (thisGeneration > 3)
        return false;

    POLYUNSIGNED total = 0, updated = 0;
    for(unsigned j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *lSpace = gMem.lSpaces[j];
        total += lSpace->gen_top - lSpace->pointer;
        updated += lSpace->updated;
    }
    if (total == 0)
        return false;
    /* I think the idea here is that if we have a significant number of
       objects in the current generation which have not actually been
       visited to have their addresses updated we should not merge
       this generation with the old data and treat them as "old" but
       instead treat them as "new".
       If we have allocated a large object in the mutable area we
       may not have a gap big enough to move it to.  We may though
       have created enough space in this minor GC to move it next time.
       That's because if we have moved an object we can't use the space
       until after the update phase has finished with the tombstone.
       DCJM 27/6/09. */
    return updated * 2 < total; // Less than 50% updated
}

bool doMultithreadGC(bool doFullGC, const POLYUNSIGNED wordsRequiredToAllocate)
{
    /* Invariant: the bitmaps are completely clean. */
    /* Note: this version of doGC does NOT clean the store 
    - that's now the user's resposibility SPF 22/10/96
    */
    unsigned j;
    static bool doFullGCNextTime = 0;
    static unsigned this_generation = 0;
    
    record_gc_time(false);

GC_AGAIN:
    /* Invariant: the bitmaps are completely clean. */
    
    /* At this point, we should have
       lSpace->bottom <= lSpace->pointer <= lSpace->gen_top <= lSpace->top       
    
       lSpace->gen_top divides the current generation from the old one.
       lSpace->pointer is the current allocation pointer.
    */

    for(j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *lSpace = gMem.lSpaces[j];
        ASSERT (lSpace->top     >= lSpace->gen_top);
        ASSERT (lSpace->gen_top >= lSpace->pointer);
        ASSERT (lSpace->pointer >= lSpace->bottom);
        // Record low-water mark before we change anything.
        // gen_bottom is the lowest object actually allocated in the
        // area.
        lSpace->gen_bottom = lSpace->pointer;
        // Set upper and lower limits of weak refs.
        lSpace->highestWeak = lSpace->bottom;
        lSpace->lowestWeak = lSpace->top;
    }

    // Set limits of weak refs.
    for (j = 0; j < gMem.npSpaces; j++)
    {
        PermanentMemSpace *pSpace = gMem.pSpaces[j];
        pSpace->highestWeak = pSpace->bottom;
        pSpace->lowestWeak = pSpace->top;
    }        
    
    
    /* Our recovery actions may insist on a full GC */
    if (doFullGCNextTime)
    {
        doFullGC = true;
        doFullGCNextTime = false;
    }

    if (doFullGC)
    {
        /* Collect everything */
        for(j = 0; j < gMem.nlSpaces; j++)
        {
            LocalMemSpace *lSpace = gMem.lSpaces[j];
            lSpace->gen_top = lSpace->top;
        }
    }

    /* Mark phase */
    GCMarkPhase();

    /* Detect unreferenced streams, windows etc. */
    GCheckWeakRefs();
    
    /* If we are doing a full GC we expand the immutable area now, so that there's
       enough room to copy the immutables that are currently in the mutable buffer.
       There's no point expanding the mutable buffer now - we'll do that later 
       when we know *exactly* how large we want it to be. */ 
    if (doFullGC) 
    {
        POLYUNSIGNED immutableData = 0;
        for(j = 0; j < gMem.nlSpaces; j++)
            immutableData += gMem.lSpaces[j]->i_marked;
        PossiblyExpandImmutableArea(immutableData);
    }

    /* Compact phase */
    POLYUNSIGNED immutable_overflow = 0; // The immutable space we couldn't copy out.
    GCCopyPhase(immutable_overflow);

    POLYUNSIGNED mCopied = 0, iCopied = 0, iMarked = 0;
    for(j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *lSpace = gMem.lSpaces[j];
        if (lSpace->isMutable)
            mCopied += lSpace->copied;
        else
        {
            iMarked += lSpace->i_marked;
            iCopied += lSpace->copied;
        }
    }
    ASSERT(mCopied == 0);
    ASSERT(iCopied <= iMarked);

    for(j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *lSpace = gMem.lSpaces[j];
        ASSERT(INSOFTRANGE(lSpace->pointer, lSpace->bottom, lSpace->gen_top));
    }    

    // Update Phase.
    GCUpdatePhase();

    {
        POLYUNSIGNED iUpdated = 0, mUpdated = 0, iMarked = 0, mMarked = 0;
        for(j = 0; j < gMem.nlSpaces; j++)
        {
            LocalMemSpace *lSpace = gMem.lSpaces[j];
            iMarked += lSpace->i_marked;
            mMarked += lSpace->m_marked;
            if (lSpace->isMutable)
                mUpdated += lSpace->updated;
            else
                iUpdated += lSpace->updated;
        }
        ASSERT(iUpdated == iMarked - immutable_overflow);
        ASSERT(mUpdated == mMarked + immutable_overflow);
    }

    /* Invariant: at most the first (gen_top - bottom) bits of the each bitmap can be dirty. */
    for(j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *lSpace = gMem.lSpaces[j];
        lSpace->bitmap.ClearBits(0, lSpace->gen_top - lSpace->bottom);
    }
    /* Invariant: the bitmaps are completely clean */

    if (doFullGC)
    {
        /* If we've had an immutable overflow, allow for this when we grow the heap */
        AdjustHeapSize(false /* immutable space*/, immutable_overflow);
        bool iFull = BufferIsReallyFull(false /* immutable area */, immutable_overflow, doFullGC);
        bool mFull = BufferIsReallyFull(true /* mutable area */, wordsRequiredToAllocate, doFullGC);
        
        /* If we're going to recollect the current generation, don't adjust the mutable buffer size yet. */
        /* We'll (probably) do that on the next collection. SPF 22/12/1997 */
        if (iFull || ! mFull || ! RecollectThisGeneration(this_generation))
            AdjustHeapSize(true /* mutable space */, wordsRequiredToAllocate);
    }

    CheckMemory();
    
    /* Have we cleared enough space? */
    {
        bool iFull = BufferIsReallyFull(false /* immutable area */, immutable_overflow, doFullGC);
        bool mFull = BufferIsReallyFull(true /* mutable area */, wordsRequiredToAllocate, doFullGC);
        
        if (iFull || mFull)
        {
            /* Recovery actions */
            if (!iFull && RecollectThisGeneration(this_generation)) /* Needs tuning!!! */
            {
                /* The next GC will re-collect THIS generation, which should be
                   enough to recover properly. */
            }
            else if (! doFullGC) // Do a full GC next time
                doFullGCNextTime = true;
            else // It was a full GC but we don't have as much free space as we normally
                 // want at the end of a full GC.  Do we have as much as we would want at the
                 // end of a partial GC?
            if (BufferIsReallyFull(false /* immutable area */, 0, false) ||
                BufferIsReallyFull(true /* mutable area */, wordsRequiredToAllocate, false))
            {
                // No we don't even have that - interrupt console processes and end GC here.
                record_gc_time(true);
                return false;
            }
        }
    }
    

    if (RecollectThisGeneration(this_generation))
    {
        /* Generally we treat all the objects we have left after this GC as "old" for
           the purposes of subsequent minor GCs.  If, though, a collection has left us
           with significant gaps we don't do that merge and instead on the next GC we
           recollect everything since the last collection. */
        /* If this was a full GC, make sure the next one is too, as we may
           need to reconfigure the mutable buffer size. If we only did a
           partial next, we would still have to mark all the immutables again
           (they would still be new) which is the main cost of a full GC.
            */
        doFullGCNextTime |= doFullGC;
        this_generation++;
    }
    else
    {
        /* Merge this generation with the old one */
        for(j = 0; j < gMem.nlSpaces; j++)
        {
            LocalMemSpace *lSpace = gMem.lSpaces[j];
            lSpace->gen_top = lSpace->pointer;
        }
        this_generation = 0;
    }
    
    // Do we have enough space for the original allocation request?
    bool haveSpace = false;
    for(j = 0; j < gMem.nlSpaces; j++)
    {
        LocalMemSpace *space = gMem.lSpaces[j];
        if (space->isMutable)
        {
            if ((POLYUNSIGNED)(space->pointer - space->bottom) >= wordsRequiredToAllocate)
            {
                haveSpace = true;
                break;
            }
        }
    }
    if (! haveSpace)
        /* Try our recovery action immediately */
        goto GC_AGAIN;
    
    /* If the heap is very close to what we can handle on this machine,
       do the full GC immediately, because if we wait, we'll generate
       more data in the mutable buffer which will make the thrashing caused
       by the inevitable full GC even worse. SPF 2/3/1998  */
    if (doFullGCNextTime)
    {
        POLYUNSIGNED memSize = GetPhysicalMemorySize();
        // Ignore this if we can't determine.or if we have more memory than the address space.
        if (memSize != 0 && memSize+1 != 0)
        {
            POLYUNSIGNED memWords = memSize/sizeof(PolyWord);
            POLYUNSIGNED spaceUsed = 0;
            unsigned i;
            for (i = 0; i < gMem.npSpaces; i++)
            {
                MemSpace *space = gMem.pSpaces[i];
                spaceUsed += space->top - space->bottom;
            }
            for (i = 0; i < gMem.nlSpaces; i++)
            {
                LocalMemSpace *space = gMem.lSpaces[i];
                // For mutable segments include all the space since
                // that's going to be used for allocation.  For immutable
                // spaces include only the area currently in use
                if (space->isMutable)
                    spaceUsed += space->top - space->bottom;
                else
                    spaceUsed += space->top - space->pointer;
            }
        
            // This crude estimate leaves out C heap, space for executable etc.
            // We used to include the bitmaps here as well.  Since that's a fixed percentage of
            // the sizes it could easily be taken account of by reducing the percentage of real
            // pages that cause a full collection.
            POLYUNSIGNED heapLoad;
            if (memWords < 100) heapLoad = 100;
            else heapLoad = spaceUsed / (memWords/100);
            // If we're more than 80% full.
            if (heapLoad > 80)
                goto GC_AGAIN;
        }
    }

    /* End of garbage collection */
    record_gc_time(true);
    
    /* Invariant: the bitmaps are completely clean */
    return true; // Completed
}

void initialiseMultithreadGC(unsigned threads)
{
    if (! gTaskFarm.Initialise(threads, 100))
        Crash("Unable to initialise the GC task farm");
}