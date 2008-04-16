/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team 1998-2006
 *
 * Generational garbage collector: utilities
 *
 * Documentation on the architecture of the Garbage Collector can be
 * found in the online commentary:
 * 
 *   http://hackage.haskell.org/trac/ghc/wiki/Commentary/Rts/Storage/GC
 *
 * ---------------------------------------------------------------------------*/

#include "Rts.h"
#include "RtsFlags.h"
#include "Storage.h"
#include "GC.h"
#include "GCUtils.h"
#include "Printer.h"
#include "Trace.h"

#ifdef THREADED_RTS
SpinLock gc_alloc_block_sync;
#endif

bdescr *
allocBlock_sync(void)
{
    bdescr *bd;
    ACQUIRE_SPIN_LOCK(&gc_alloc_block_sync);
    bd = allocBlock();
    RELEASE_SPIN_LOCK(&gc_alloc_block_sync);
    return bd;
}

void
freeChain_sync(bdescr *bd)
{
    ACQUIRE_SPIN_LOCK(&gc_alloc_block_sync);
    freeChain(bd);
    RELEASE_SPIN_LOCK(&gc_alloc_block_sync);
}

/* -----------------------------------------------------------------------------
   Workspace utilities
   -------------------------------------------------------------------------- */

bdescr *
grab_todo_block (step_workspace *ws)
{
    bdescr *bd;
    step *stp;

    stp = ws->step;
    bd = NULL;

    if (ws->buffer_todo_bd)
    {
	bd = ws->buffer_todo_bd;
	ASSERT(bd->link == NULL);
	ws->buffer_todo_bd = NULL;
	return bd;
    }

    ACQUIRE_SPIN_LOCK(&stp->sync_todo);
    if (stp->todos) {
	bd = stp->todos;
        if (stp->todos == stp->todos_last) {
            stp->todos_last = NULL;
        }
	stp->todos = bd->link;
        stp->n_todos--;
	bd->link = NULL;
    }	
    RELEASE_SPIN_LOCK(&stp->sync_todo);
    return bd;
}

void
push_scanned_block (bdescr *bd, step_workspace *ws)
{
    ASSERT(bd != NULL);
    ASSERT(bd->link == NULL);
    ASSERT(bd->step == ws->step);
    ASSERT(bd->u.scan == bd->free);

    if (bd->start + BLOCK_SIZE_W - bd->free > WORK_UNIT_WORDS)
    {
        // a partially full block: put it on the part_list list.
        bd->link = ws->part_list;
        ws->part_list = bd;
        ws->n_part_blocks++;
        IF_DEBUG(sanity, 
                 ASSERT(countBlocks(ws->part_list) == ws->n_part_blocks));
    }
    else
    {
        // put the scan block on the ws->scavd_list.
        bd->link = ws->scavd_list;
        ws->scavd_list = bd;
        ws->n_scavd_blocks ++;
        IF_DEBUG(sanity, 
                 ASSERT(countBlocks(ws->scavd_list) == ws->n_scavd_blocks));
    }
}

StgPtr
todo_block_full (nat size, step_workspace *ws)
{
    bdescr *bd;

    bd = ws->todo_bd;

    ASSERT(bd != NULL);
    ASSERT(bd->link == NULL);
    ASSERT(bd->step == ws->step);

    bd->free = ws->todo_free;

    // If the global list is not empty, or there's not much work in
    // this block to push, and there's enough room in
    // this block to evacuate the current object, then just increase
    // the limit.
    if (ws->step->todos != NULL || 
        (bd->free - bd->u.scan < WORK_UNIT_WORDS / 2)) {
        if (bd->free + size < bd->start + BLOCK_SIZE_W) {
            debugTrace(DEBUG_gc, "increasing limit for %p", bd->start);
            ws->todo_lim = stg_min(bd->start + BLOCK_SIZE_W,
                                   ws->todo_lim + stg_max(WORK_UNIT_WORDS,size));
            return ws->todo_free;
        }
    }
    
    ASSERT(bd->u.scan >= bd->start && bd->u.scan <= bd->free);

    // If this block is not the scan block, we want to push it out and
    // make room for a new todo block.
    if (bd != ws->scan_bd)
    {
        // If this block does not have enough space to allocate the
        // current object, but it also doesn't have any work to push, then 
        // push it on to the scanned list.  It cannot be empty, because
        // then there would be enough room to copy the current object.
        if (bd->u.scan == bd->free)
        {
            ASSERT(bd->free != bd->start);
            push_scanned_block(bd, ws);
        }
        // Otherwise, push this block out to the global list.
        else 
        {
            step *stp;
            stp = ws->step;
            trace(TRACE_gc|DEBUG_gc, "push todo block %p (%d words), step %d, n_todos: %d", 
                  bd->start, bd->free - bd->u.scan, stp->abs_no, stp->n_todos);
            // ToDo: use buffer_todo
            ACQUIRE_SPIN_LOCK(&stp->sync_todo);
            if (stp->todos_last == NULL) {
                stp->todos_last = bd;
                stp->todos = bd;
            } else {
                stp->todos_last->link = bd;
                stp->todos_last = bd;
            }
            stp->n_todos++;
            RELEASE_SPIN_LOCK(&stp->sync_todo);
        }
    }

    ws->todo_bd   = NULL;
    ws->todo_free = NULL;
    ws->todo_lim  = NULL;

    alloc_todo_block(ws, size);

    return ws->todo_free;
}

StgPtr
alloc_todo_block (step_workspace *ws, nat size)
{
    bdescr *bd;

    // Grab a part block if we have one, and it has enough room
    if (ws->part_list != NULL && 
        ws->part_list->start + BLOCK_SIZE_W - ws->part_list->free > (int)size)
    {
        bd = ws->part_list;
        ws->part_list = bd->link;
        ws->n_part_blocks--;
    }
    else
    {
        bd = allocBlock_sync();
        bd->gen_no = ws->step->gen_no;
        bd->step = ws->step;
        bd->u.scan = bd->start;

        // blocks in to-space in generations up to and including N
        // get the BF_EVACUATED flag.
        if (ws->step->gen_no <= N) {
            bd->flags = BF_EVACUATED;
        } else {
            bd->flags = 0;
        }
    }

    bd->link = NULL;

    ws->todo_bd = bd;
    ws->todo_free = bd->free;
    ws->todo_lim  = stg_min(bd->start + BLOCK_SIZE_W,
                            bd->free + stg_max(WORK_UNIT_WORDS,size));

    debugTrace(DEBUG_gc, "alloc new todo block %p for step %d", 
               bd->start, ws->step->abs_no);

    return ws->todo_free;
}

/* -----------------------------------------------------------------------------
 * Debugging
 * -------------------------------------------------------------------------- */

#if DEBUG
void
printMutableList(generation *gen)
{
    bdescr *bd;
    StgPtr p;

    debugBelch("mutable list %p: ", gen->mut_list);

    for (bd = gen->mut_list; bd != NULL; bd = bd->link) {
	for (p = bd->start; p < bd->free; p++) {
	    debugBelch("%p (%s), ", (void *)*p, info_type((StgClosure *)*p));
	}
    }
    debugBelch("\n");
}
#endif /* DEBUG */
