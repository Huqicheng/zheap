/*-------------------------------------------------------------------------
 *
 * zheapam_handler.c
 *	  zheap table access method code
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/zheap/zheapam_handler.c
 *
 *
 * NOTES
 *	  This file contains the zheap_ routines which implement
 *	  the POSTGRES zheap table access method used for all POSTGRES
 *	  relations.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"

#include "access/multixact.h"
#include "access/relscan.h"
#include "access/rewritezheap.h"
#include "access/tableam.h"
#include "access/tpd.h"
#include "access/tsmapi.h"
#include "access/tuptoaster.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/zheap.h"
#include "access/zheapscan.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/ztqual.h"


/*
 * ZBORKED: don't want to include heapam.h to avoid mistakes - the syncscan
 * stuff should probably be moved to a different header.
 */
extern void ss_report_location(Relation rel, BlockNumber location);

/* ----------------------------------------------------------------
 *				storage AM support routines for zheapam
 * ----------------------------------------------------------------
 */

static bool
zheapam_fetch_row_version(Relation relation,
						  ItemPointer tid,
						  Snapshot snapshot,
						  TupleTableSlot *slot)
{
	ZHeapTupleTableSlot *zslot = (ZHeapTupleTableSlot *) slot;
	Buffer		buffer;

	ExecClearTuple(slot);

	if (zheap_fetch(relation, snapshot, tid, &zslot->tuple, &buffer, false))
	{
		ExecStoreZHeapTuple(zslot->tuple, slot, true);
		ReleaseBuffer(buffer);

		slot->tts_tableOid = RelationGetRelid(relation);

		return true;
	}

	slot->tts_tableOid = RelationGetRelid(relation);

	return false;
}

/*
 * Insert a heap tuple from a slot, which may contain an OID and speculative
 * insertion token.
 */
static void
zheapam_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
			   int options, BulkInsertState bistate)
{
	ZHeapTuple	tuple = ExecGetZHeapTupleFromSlot(slot);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	if (slot->tts_tableOid != InvalidOid)
		tuple->t_tableOid = slot->tts_tableOid;

	/* Perform the insertion, and copy the resulting ItemPointer */
	zheap_insert(relation, tuple, cid, options, bistate, 0);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
}

static void
zheapam_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid,
						   int options, BulkInsertState bistate, uint32 specToken)
{
	ZHeapTuple	tuple = ExecGetZHeapTupleFromSlot(slot);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	if (slot->tts_tableOid != InvalidOid)
		tuple->t_tableOid = slot->tts_tableOid;

	options |= ZHEAP_INSERT_SPECULATIVE;

	/* Perform the insertion, and copy the resulting ItemPointer */
	zheap_insert(relation, tuple, cid, options, bistate, specToken);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
}

static void
zheapam_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 spekToken,
							 bool succeeded)
{
	/* adjust the tuple's state accordingly */
	if (succeeded)
		zheap_finish_speculative(relation, &slot->tts_tid);
	else
		zheap_abort_speculative(relation, &slot->tts_tid);
}


static TM_Result
zheapam_delete(Relation relation, ItemPointer tid, CommandId cid,
			   Snapshot snapshot, Snapshot crosscheck, bool wait,
			   TM_FailureData *tmfd, bool changingPart)
{
	/*
	 * Currently Deleting of index tuples are handled at vacuum, in case if
	 * the storage itself is cleaning the dead tuples by itself, it is the
	 * time to call the index tuple deletion also.
	 */
	return zheap_delete(relation, tid, cid, crosscheck, snapshot, wait, tmfd, changingPart);
}


/*
 * Locks tuple and fetches its newest version and TID.
 *
 *	relation - table containing tuple
 *	tid - TID of tuple to lock
 *	snapshot - snapshot identifying required version (used for assert check only)
 *	slot - tuple to be returned
 *	cid - current command ID (used for visibility test, and stored into
 *		  tuple's cmax if lock is successful)
 *	mode - indicates if shared or exclusive tuple lock is desired
 *	wait_policy - what to do if tuple lock is not available
 *	flags – indicating how do we handle updated tuples
 *	*tmfd - filled in failure cases
 *
 * Function result may be:
 *	HeapTupleMayBeUpdated: lock was successfully acquired
 *	HeapTupleInvisible: lock failed because tuple was never visible to us
 *	HeapTupleSelfUpdated: lock failed because tuple updated by self
 *	HeapTupleUpdated: lock failed because tuple updated by other xact
 *	HeapTupleDeleted: lock failed because tuple deleted by other xact
 *	HeapTupleWouldBlock: lock couldn't be acquired and wait_policy is skip
 *
 * In the failure cases other than HeapTupleInvisible, the routine fills
 * *tmfd with the tuple's t_ctid, t_xmax (resolving a possible MultiXact,
 * if necessary), and t_cmax (the last only for HeapTupleSelfUpdated,
 * since we cannot obtain cmax from a combocid generated by another
 * transaction).
 * See comments for struct TM_FailureData for additional info.
 */
static TM_Result
zheapam_lock_tuple(Relation relation, ItemPointer tid, Snapshot snapshot,
				   TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				   LockWaitPolicy wait_policy, uint8 flags,
				   TM_FailureData *tmfd)
{
	ZHeapTupleTableSlot *zslot = (ZHeapTupleTableSlot *) slot;
	TM_Result	result;
	Buffer		buffer;
	ZHeapTuple	tuple = &zslot->tupdata;
	bool		doWeirdEval = (flags & TUPLE_LOCK_FLAG_WEIRD) != 0;

	tmfd->traversed = false;

	/* ZBORKED: Currently zheap, due to doWeirdEval, doesn't actually signal */
	/* properly that we're traversing. That sucks. */
	if (doWeirdEval)
		tmfd->traversed = true;

retry:
	result = zheap_lock_tuple(relation, tid, cid, mode, wait_policy,
							  (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) ? true : false,
							  doWeirdEval,
							  snapshot, tuple, &buffer, tmfd);

	if (result == TM_Updated &&
		(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
	{
		SnapshotData SnapshotDirty;
		TransactionId priorXmax = tmfd->xmax;

		ReleaseBuffer(buffer);

		/* it was updated, so look at the updated version */
		*tid = tmfd->ctid;
		/* updated row should have xmin matching this xmax */
		priorXmax = tmfd->xmax;

		/*
		 * We should not encounter a speculative tuple on recheck.  Also, for
		 * a deleted item pointer, tuple data is not initialized.
		 */
		Assert((tuple->t_len == 0) ||
			   !(tuple->t_data->t_infomask & ZHEAP_SPECULATIVE_INSERT));

		if (ItemPointerEquals(&tmfd->ctid, &tuple->t_self) &&
			!tmfd->in_place_updated_or_locked)
		{
			/* tuple was deleted, so give up */
			return TM_Deleted;
		}

		/* signal that a tuple later in the chain is getting locked */
		tmfd->traversed = true;

		/*
		 * fetch target tuple
		 *
		 * Loop here to deal with updated or busy tuples
		 */
		InitDirtySnapshot(SnapshotDirty);
		for (;;)
		{
			/* check whether next version would be in a different partition */
			if (ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

			if (zheap_fetch(relation, &SnapshotDirty, tid, &tuple, &buffer, true))
			{
				/*
				 * Ensure that the tuple is same as what we are expecting.  If
				 * the current or any prior version of tuple doesn't contain
				 * the effect of priorXmax, then the slot must have been
				 * recycled and reused for an unrelated tuple.  This implies
				 * that the latest version of the row was deleted, so we need
				 * do nothing.
				 */
				if (!ValidateTuplesXact(relation, tuple, &SnapshotDirty,
										buffer, priorXmax, true))
				{
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/* otherwise xmin should not be dirty... */
				if (TransactionIdIsValid(SnapshotDirty.xmin))
					elog(ERROR, "t_xmin is uncommitted in tuple to be updated");

				/*
				 * If tuple is being updated by other (sub)transaction then we
				 * have to wait for its commit/abort, or die trying.
				 */
				if (SnapshotDirty.subxid != InvalidSubTransactionId &&
					TransactionIdIsValid(SnapshotDirty.xmax))
				{
					ReleaseBuffer(buffer);
					switch (wait_policy)
					{
						case LockWaitBlock:
							SubXactLockTableWait(SnapshotDirty.xmax,
												 SnapshotDirty.subxid,
												 relation, &tuple->t_self,
												 XLTW_FetchUpdated);
							break;
						case LockWaitSkip:
							if (!ConditionalSubXactLockTableWait(SnapshotDirty.xmax,
																 SnapshotDirty.subxid))
								return result;	/* skip instead of waiting */
							break;
						case LockWaitError:
							if (ConditionalSubXactLockTableWait(SnapshotDirty.xmax,
																SnapshotDirty.subxid))
								ereport(ERROR,
										(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
										 errmsg("could not obtain lock on row in relation \"%s\"",
												RelationGetRelationName(relation))));

							break;
					}
					continue;	/* loop back to repeat zheap_fetch */
				}
				else if (TransactionIdIsValid(SnapshotDirty.xmax))
				{
					ReleaseBuffer(buffer);
					switch (wait_policy)
					{
						case LockWaitBlock:
							XactLockTableWait(SnapshotDirty.xmax, relation,
											  &tuple->t_self, XLTW_FetchUpdated);
							break;
						case LockWaitSkip:
							if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
								return result;	/* skip instead of waiting */
							break;
						case LockWaitError:
							if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
								ereport(ERROR,
										(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
										 errmsg("could not obtain lock on row in relation \"%s\"",
												RelationGetRelationName(relation))));
							break;
					}
					continue;	/* loop back to repeat zheap_fetch */
				}

				/*
				 * If tuple was inserted by our own transaction, we have to
				 * check cmin against es_output_cid: cmin >= current CID means
				 * our command cannot see the tuple, so we should ignore it.
				 * Otherwise zheap_lock_tuple() will throw an error, and so
				 * would any later attempt to update or delete the tuple.  (We
				 * need not check cmax because ZHeapTupleSatisfiesDirty will
				 * consider a tuple deleted by our transaction dead,
				 * regardless of cmax.) We just checked that priorXmax ==
				 * xmin, so we can test that variable instead of doing
				 * ZHeapTupleHeaderGetXid again.
				 */
				if (TransactionIdIsCurrentTransactionId(priorXmax))
				{
					CommandId	tup_cid;

					LockBuffer(buffer, BUFFER_LOCK_SHARE);

					/*
					 * Fixme -If the tuple is updated such that its
					 * transaction slot has been changed, then we will never
					 * be able to get the correct tuple from undo.  To avoid,
					 * that we need to get the latest tuple from page rather
					 * than relying on it's in-memory copy.  See
					 * ValidateTuplesXact.
					 */
					tup_cid = ZHeapTupleGetCid(tuple, buffer, InvalidUndoRecPtr,
											   InvalidXactSlotId);
					if (tup_cid >= cid)
					{
						/* ZBORKED: check equivalent heap code */
						tmfd->xmax = priorXmax;
						tmfd->cmax = tup_cid;
						UnlockReleaseBuffer(buffer);
						/* ZBORKED: is this correct? */
						return TM_SelfModified;
					}
					LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				}

				doWeirdEval = true;
				ReleaseBuffer(buffer);
				goto retry;
			}

			/*
			 * If we don't get any tuple, the latest version of the row must
			 * have been deleted, so we need do nothing.
			 */
			if (tuple == NULL)
			{
				ReleaseBuffer(buffer);
				return TM_Deleted;
			}

			/*
			 * Ensure that the tuple is same as what we are expecting as
			 * above.
			 */
			if (!ValidateTuplesXact(relation, tuple, &SnapshotDirty,
									buffer, priorXmax, true))
			{
				if (BufferIsValid(buffer))
					ReleaseBuffer(buffer);
				return TM_Deleted;
			}

			/* check whether next version would be in a different partition */
			if (ZHeapTupleIsMoved(tuple->t_data->t_infomask))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

			if (ItemPointerEquals(&(tuple->t_self), tid))
			{
				/* deleted, so forget about it */
				ReleaseBuffer(buffer);
				return TM_Deleted;
			}

			/* updated row should have xid matching this xmax */
			priorXmax = ZHeapTupleGetTransXID(tuple, buffer, true);

			/*
			 * As we still hold a snapshot to which priorXmax is not visible,
			 * neither the transaction slot on tuple can be marked as frozen
			 * nor the corresponding undo be discarded.
			 */
			Assert(TransactionIdIsValid(priorXmax));

			/* be tidy */
			zheap_freetuple(tuple);
			ReleaseBuffer(buffer);
			/* loop back to fetch next in chain */
		}
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	ExecStoreZHeapTuple(tuple, slot, false);
	/* FIXME:invent option to just transfer pin ? */
	ReleaseBuffer(buffer);

	return result;
}


static TM_Result
zheapam_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
			   CommandId cid, Snapshot snapshot, Snapshot crosscheck,
			   bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
			   bool *update_indexes)
{
	ZHeapTuple	ztuple = ExecGetZHeapTupleFromSlot(slot);
	TM_Result	result;

	/* Update the tuple with table oid */
	if (slot->tts_tableOid != InvalidOid)
		ztuple->t_tableOid = slot->tts_tableOid;

	result = zheap_update(relation, otid, ztuple, cid, crosscheck, snapshot,
						  wait, tmfd, lockmode);
	ItemPointerCopy(&ztuple->t_self, &slot->tts_tid);

	slot->tts_tableOid = RelationGetRelid(relation);

	/*
	 * Note: instead of having to update the old index tuples associated with
	 * the heap tuple, all we do is form and insert new index tuples. This is
	 * because UPDATEs are actually DELETEs and INSERTs, and index tuple
	 * deletion is done later by VACUUM (see notes in ExecDelete). All we do
	 * here is insert new index tuples.  -cim 9/27/89
	 */

	/*
	 * insert index entries for tuple
	 *
	 * Note: heap_update returns the tid (location) of the new tuple in the
	 * t_self field.
	 *
	 * If it's a HOT update, we mustn't insert new index entries.
	 */
	*update_indexes = result == TM_Ok &&
		!ZHeapTupleIsInPlaceUpdated(ztuple->t_data->t_infomask);

	return result;
}

static const TupleTableSlotOps *
zheapam_slot_callbacks(Relation relation)
{
	return &TTSOpsZHeapTuple;
}

static bool
zheapam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	ZHeapTupleTableSlot *zslot = (ZHeapTupleTableSlot *) slot;
	Buffer		buffer;
	ItemPointer tid;
	ZHeapTuple	tup;
	bool		res;

	Assert(TTS_IS_ZHEAP(slot));
	Assert(zslot->tuple);

	tid = &(zslot->tuple->t_self);

	buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/*
	 * NB: current transaction has inserted/updated the tuple, so it can't be
	 * deleted
	 */

	ZHeapTupleFetch(rel, buffer, ItemPointerGetOffsetNumber(tid), snapshot,
					&tup, NULL);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);

	if (!tup)
	{
		/* satisfies routine returned no tuple, so clearly invisible */
		res = false;
	}
	else if (tup->t_len != zslot->tuple->t_len)
	{
		/* length differs, the input tuple can't be visible */
		res = false;
	}
	else if (memcmp(tup->t_data, zslot->tuple->t_data, tup->t_len) != 0)
	{
		/*
		 * ZBORKED: compare tuple contents, to be sure the tuple returned by
		 * the visibility routine is the input tuple. There *got* to be a
		 * better solution than this.
		 */
		res = false;
	}
	else
		res = true;

	if (tup)
		pfree(tup);

	return res;
}

static bool
zheapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	ZHeapScanDesc zscan = (ZHeapScanDesc) scan;

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < zscan->rs_nblocks;
}

static IndexFetchTableData *
zheapam_begin_index_fetch(Relation rel)
{
	IndexFetchZHeapData *hscan = palloc0(sizeof(IndexFetchZHeapData));

	hscan->xs_base.rel = rel;
	hscan->xs_cbuf = InvalidBuffer;
	/* hscan->xs_continue_hot = false; */

	return &hscan->xs_base;
}


static void
zheapam_reset_index_fetch(IndexFetchTableData *scan)
{
	IndexFetchZHeapData *hscan = (IndexFetchZHeapData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}

	/* hscan->xs_continue_hot = false; */
}

static void
zheapam_end_index_fetch(IndexFetchTableData *scan)
{
	IndexFetchZHeapData *hscan = (IndexFetchZHeapData *) scan;

	zheapam_reset_index_fetch(scan);

	pfree(hscan);
}

static bool
zheapam_index_fetch_tuple(struct IndexFetchTableData *scan,
						  ItemPointer tid,
						  Snapshot snapshot,
						  TupleTableSlot *slot,
						  bool *call_again, bool *all_dead)
{
	IndexFetchZHeapData *hscan = (IndexFetchZHeapData *) scan;
	ZHeapTuple	zheapTuple = NULL;

	/*
	 * No HOT chains in zheap.
	 */
	Assert(!*call_again);

	/* Switch to correct buffer if we don't have it already */
	hscan->xs_cbuf = ReleaseAndReadBuffer(hscan->xs_cbuf,
										  hscan->xs_base.rel,
										  ItemPointerGetBlockNumber(tid));

	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	zheapTuple = zheap_search_buffer(tid, hscan->xs_base.rel,
									 hscan->xs_cbuf,
									 snapshot,
									 all_dead);
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (zheapTuple)
	{
		slot->tts_tableOid = RelationGetRelid(scan->rel);
		ExecStoreZHeapTuple(zheapTuple, slot, false);
	}

	return zheapTuple != NULL;
}

/*
 * Similar to IndexBuildHeapRangeScan, but for zheap relations.
 */
static double
IndexBuildZHeapRangeScan(Relation heapRelation,
						 Relation indexRelation,
						 IndexInfo *indexInfo,
						 bool allow_sync,
						 bool anyvisible,
						 bool progress,
						 BlockNumber start_blockno,
						 BlockNumber numblocks,
						 IndexBuildCallback callback,
						 void *callback_state,
						 TableScanDesc sscan)
{
	ZHeapScanDesc scan = (ZHeapScanDesc) sscan;
	bool		is_system_catalog;
	bool		checking_uniqueness;
	HeapTuple	heapTuple;
	ZHeapTuple	zheapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	ZHeapTupleTableSlot *zslot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	TransactionId OldestXmin;
	bool		need_unregister_snapshot = false;
	SubTransactionId subxid_xwait = InvalidSubTransactionId;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));
	Assert(RelationStorageIsZHeap(heapRelation));

	/* Remember if it's a system catalog */
	is_system_catalog = IsSystemRelation(heapRelation);

	/* See whether we're verifying uniqueness/exclusion properties */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/*
	 * "Any visible" mode is not compatible with uniqueness checks; make sure
	 * only one of those is requested.
	 */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);
	zslot = (ZHeapTupleTableSlot *) slot;

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);


	heapTuple = (HeapTuple) palloc0(SizeofHeapTupleHeader);

	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples). In a
	 * concurrent build, or during bootstrap, we take a regular MVCC snapshot
	 * and index whatever is live according to that.
	 */
	OldestXmin = InvalidTransactionId;

	/* It is okay to ignore lazy vacuums here */
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
		OldestXmin = GetOldestXmin(heapRelation, PROCARRAY_FLAGS_VACUUM);

	if (!scan)
	{
		/*
		 * Serial index build.
		 *
		 * Must begin our own heap scan in this case.  We may also need to
		 * register a snapshot whose lifetime is under our direct control.
		 */
		if (!TransactionIdIsValid(OldestXmin))
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;

		sscan = table_beginscan_strat(heapRelation, /* relation */
									  snapshot, /* snapshot */
									  0,	/* number of keys */
									  NULL, /* scan key */
									  true, /* buffer access strategy OK */
									  allow_sync);	/* syncscan OK? */
		scan = (ZHeapScanDesc) sscan;
	}
	else
	{
		/*
		 * Parallel index build.
		 *
		 * Parallel case never registers/unregisters own snapshot.  Snapshot
		 * is taken from parallel heap scan, and is SnapshotAny or an MVCC
		 * snapshot, based on same criteria as serial case.
		 */
		Assert(!IsBootstrapProcessingMode());
		Assert(allow_sync);
		snapshot = scan->rs_base.rs_snapshot;
	}

	/*
	 * Must call GetOldestXmin() with SnapshotAny.  Should never call
	 * GetOldestXmin() with MVCC snapshot. (It's especially worth checking
	 * this for parallel builds, since ambuild routines that support parallel
	 * builds must work these details out for themselves.)
	 */
	Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
	Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
		   !TransactionIdIsValid(OldestXmin));
	Assert(snapshot == SnapshotAny || !anyvisible);

	/* set our scan endpoints */
	if (!allow_sync)
		zheap_setscanlimits(sscan, start_blockno, numblocks);
	else
	{
		/* syncscan can only be requested on whole relation */
		Assert(start_blockno == 0);
		start_blockno = ZHEAP_METAPAGE + 1;
		Assert(numblocks == InvalidBlockNumber);
	}

	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while (zheap_getnextslot(sscan, ForwardScanDirection, slot))
	{
		bool		tupleIsAlive;
		ZHeapTuple	targztuple = NULL;

		zheapTuple = ExecGetZHeapTupleFromSlot(slot);

		CHECK_FOR_INTERRUPTS();

		if (snapshot == SnapshotAny)
		{
			/* do our own time qual check */
			bool		indexIt;
			TransactionId xwait;

	recheck:

			/*
			 * We could possibly get away with not locking the buffer here,
			 * since caller should hold ShareLock on the relation, but let's
			 * be conservative about it.
			 */
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

			targztuple = zheap_copytuple(zheapTuple);
			switch (ZHeapTupleSatisfiesOldestXmin(targztuple, OldestXmin,
												  scan->rs_cbuf, true,
												  &targztuple, &xwait,
												  &subxid_xwait))
			{
				case ZHEAPTUPLE_DEAD:
					/* Definitely dead, we can ignore it */
					indexIt = false;
					tupleIsAlive = false;
					break;
				case ZHEAPTUPLE_LIVE:
					/* Normal case, index and unique-check it */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case ZHEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must index it
					 * anyway to preserve MVCC semantics. (Pre-existing
					 * transactions could try to use the index after we finish
					 * building it, and may need to see such tuples.)
					 */
					indexIt = true;
					tupleIsAlive = false;
					break;
				case ZHEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * In "anyvisible" mode, this tuple is visible and we
					 * don't need any further checks.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = true;
						break;
					}

					/*
					 * Since caller should hold ShareLock or better, normally
					 * the only way to see this is if it was inserted earlier
					 * in our own transaction.  However, it can happen in
					 * system catalogs, since we tend to release write lock
					 * before commit there.  Give a warning if neither case
					 * applies.
					 */
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent insert in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * If we are performing uniqueness checks, indexing
						 * such a tuple could lead to a bogus uniqueness
						 * failure.  In that case we wait for the inserting
						 * transaction to finish and check again.
						 */
						if (checking_uniqueness)
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							if (subxid_xwait != InvalidSubTransactionId)
								SubXactLockTableWait(xwait, subxid_xwait, heapRelation,
													 &zheapTuple->t_self,
													 XLTW_InsertIndexUnique);
							else
								XactLockTableWait(xwait, heapRelation,
												  &zheapTuple->t_self,
												  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();

							if (targztuple != NULL)
								pfree(targztuple);

							goto recheck;
						}
					}

					/*
					 * We must index such tuples, since if the index build
					 * commits then they're good.
					 */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case ZHEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * As with INSERT_IN_PROGRESS case, this is unexpected
					 * unless it's our own deletion or a system catalog; but
					 * in anyvisible mode, this tuple is visible.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = false;
						break;
					}

					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent insert in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * If we are performing uniqueness checks, indexing
						 * such a tuple could lead to a bogus uniqueness
						 * failure.  In that case we wait for the inserting
						 * transaction to finish and check again.
						 */
						if (checking_uniqueness)
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							if (subxid_xwait != InvalidTransactionId)
								SubXactLockTableWait(xwait, subxid_xwait,
													 heapRelation,
													 &zheapTuple->t_self,
													 XLTW_InsertIndexUnique);
							else
								XactLockTableWait(xwait, heapRelation,
												  &zheapTuple->t_self,
												  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();

							if (targztuple != NULL)
								pfree(targztuple);

							goto recheck;
						}

						/*
						 * Otherwise index it but don't check for uniqueness,
						 * the same as a RECENTLY_DEAD tuple.
						 */
						indexIt = true;
					}
					else
					{
						/*
						 * It's a regular tuple deleted by our own xact. Index
						 * it but don't check for uniqueness, the same as a
						 * RECENTLY_DEAD tuple.
						 */
						indexIt = true;
					}
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected ZHeapTupleSatisfiesOldestXmin result");
					indexIt = tupleIsAlive = false; /* keep compiler quiet */
					break;
			}

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			if (!indexIt)
				continue;
		}
		else
		{
			/* zheap_getnextslot did the time qual check */
			tupleIsAlive = true;
			targztuple = zheapTuple;
		}

		reltuples += 1;

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
			{
				/*
				 * For SnapshotAny, targztuple is locally palloced above. So,
				 * free it.
				 */
				if (snapshot == SnapshotAny && targztuple != NULL)
					pfree(targztuple);
				continue;
			}
		}

		/*
		 * For the current tuple, extract all the attributes we use in this
		 * index, and note which are null.  This also performs evaluation of
		 * any expressions needed.
		 *
		 * NOTE: We can't free the zheap tuple fetched by the scan method
		 * before next iteration since this tuple is also referenced by
		 * scan->rs_cztup. which is used by zheap scan API's to fetch the next
		 * tuple. But, for forming and creating the index, we've to store the
		 * correct version of the tuple in the slot. Hence, after forming the
		 * index and calling the callback function, we restore the zheap tuple
		 * fetched by the scan method in the slot.
		 */
		zslot->tuple = targztuple;
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * FIXME: buildCallback functions accepts heaptuple as an argument.
		 * But, it needs only the tid. So, we set t_self for the zheap tuple
		 * and call the AM's callback.
		 */
		heapTuple->t_self = zheapTuple->t_self;

		/* Call the AM's callback routine to process the tuple */
		callback(indexRelation, heapTuple, values, isnull, tupleIsAlive,
				 callback_state);

		zslot->tuple = zheapTuple;

		/*
		 * For SnapshotAny, targztuple is locally palloc'd above. So, free it.
		 */
		if (snapshot == SnapshotAny && targztuple != NULL)
			pfree(targztuple);
	}

	table_endscan(sscan);

	/* we can now forget our snapshot, if set and registered by us */
	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	pfree(heapTuple);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

/*
 * validate_index_zheapscan - second table scan for concurrent index build
 *
 * This has much code in common with IndexBuildZHeapScan, but it's enough
 * different that it seems cleaner to have two routines not one.
 */
static void
validate_index_zheapscan(Relation heapRelation,
						 Relation indexRelation,
						 IndexInfo *indexInfo,
						 Snapshot snapshot,
						 ValidateIndexState *state)
{
	TableScanDesc sscan;
	ZHeapScanDesc scan;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	bool		in_index[MaxZHeapTuplesPerPage];

	/* state variables for the merge */
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan of the base relation.  We need just those tuples
	 * satisfying the passed-in reference snapshot.  We must disable syncscan
	 * here, because it's critical that we read from block zero forward to
	 * match the sorted TIDs.
	 */
	sscan = table_beginscan_strat(heapRelation, /* relation */
								  snapshot, /* snapshot */
								  0,	/* number of keys */
								  NULL, /* scan key */
								  true, /* buffer access strategy OK */
								  false);	/* syncscan not OK */
	scan = (ZHeapScanDesc) sscan;

	/*
	 * Scan all tuples matching the snapshot.
	 */
	while (zheap_getnextslot(sscan, ForwardScanDirection, slot))
	{
		OffsetNumber offnum = ItemPointerGetOffsetNumber(&slot->tts_tid);

		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		/*
		 * "merge" by skipping through the index tuples until we find or pass
		 * the current tuple.
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &slot->tts_tid) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			if (indexcursor)
			{
				/*
				 * Remember index items seen earlier on the current heap page
				 */
				if (ItemPointerGetBlockNumber(indexcursor) == scan->rs_cblock)
					in_index[ItemPointerGetOffsetNumber(indexcursor) - 1] = true;
			}

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  &ts_val, &ts_isnull, NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;

				/* If int8 is pass-by-ref, free (encoded) TID Datum memory */
#ifndef USE_FLOAT8_BYVAL
				pfree(DatumGetPointer(ts_val));
#endif
			}
			else
			{
				/* Be tidy */
				indexcursor = NULL;
			}
		}

		/*
		 * If the tuplesort has overshot *and* we didn't see a match earlier,
		 * then this tuple is missing from the index, so insert it.
		 */
		if ((tuplesort_empty ||
			 ItemPointerCompare(indexcursor, &slot->tts_tid) > 0) &&
			!in_index[offnum - 1])
		{

			/* Set up for predicate or expression evaluation */

			/*
			 * In a partial index, discard tuples that don't satisfy the
			 * predicate.
			 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}

			/*
			 * For the current heap tuple, extract all the attributes we use
			 * in this index, and note which are null.  This also performs
			 * evaluation of any expressions needed.
			 */
			FormIndexDatum(indexInfo,
						   slot,
						   estate,
						   values,
						   isnull);

			/*
			 * You'd think we should go ahead and build the index tuple here,
			 * but some index AMs want to do further processing on the data
			 * first. So pass the values[] and isnull[] arrays, instead.
			 */

			/*
			 * If the tuple is already committed dead, you might think we
			 * could suppress uniqueness checking, but this is no longer true
			 * in the presence of HOT, because the insert is actually a proxy
			 * for a uniqueness check on the whole HOT-chain.  That is, the
			 * tuple we have here could be dead because it was already
			 * HOT-updated, and if so the updating transaction will not have
			 * thought it should insert index entries.  The index AM will
			 * check the whole HOT-chain and correctly detect a conflict if
			 * there is one.
			 */

			index_insert(indexRelation,
						 values,
						 isnull,
						 &slot->tts_tid,
						 heapRelation,
						 indexInfo->ii_Unique ?
						 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 indexInfo);

			state->tups_inserted += 1;

			MemoryContextReset(econtext->ecxt_per_tuple_memory);
		}
	}

	table_endscan(sscan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

static bool
zheapam_scan_analyze_next_block(TableScanDesc sscan, BlockNumber blockno, BufferAccessStrategy bstrategy)
{
	ZHeapScanDesc scan = (ZHeapScanDesc) sscan;
	Page		targpage;

	/*
	 * We must maintain a pin on the target page's buffer to ensure that the
	 * maxoffset value stays good (else concurrent VACUUM might delete tuples
	 * out from under us).  Hence, pin the page until we are done looking at
	 * it.  We also choose to hold sharelock on the buffer throughout --- we
	 * could release and re-acquire sharelock for each tuple, but since we
	 * aren't doing much work per tuple, the extra lock traffic is probably
	 * better avoided.
	 */
	scan->rs_cblock = blockno;
	scan->rs_cindex = FirstOffsetNumber;

	if (blockno == ZHEAP_METAPAGE)
		return false;

	scan->rs_cbuf = ReadBufferExtended(scan->rs_base.rs_rd, MAIN_FORKNUM, blockno,
									   RBM_NORMAL, bstrategy);
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

	/* Skip TPD pages for zheap relations. */
	targpage = BufferGetPage(scan->rs_cbuf);
	if (IsTPDPage(targpage))
	{
		UnlockReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;

		return false;
	}

	return true;
}

static bool
zheapam_scan_analyze_next_tuple(TableScanDesc sscan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot)
{
	ZHeapScanDesc scan = (ZHeapScanDesc) sscan;
	Page		targpage;
	OffsetNumber maxoffset;

	Assert(TTS_IS_ZHEAP(slot));
	Assert(scan->rs_cblock != ZHEAP_METAPAGE);

	targpage = BufferGetPage(scan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(targpage);

	Assert(!IsTPDPage(targpage));

	/* Inner loop over all tuples on the selected page */
	for (; scan->rs_cindex <= maxoffset; scan->rs_cindex++)
	{
		ItemId		itemid;
		ZHeapTuple	targtuple;
		bool		sample_it = false;
		TransactionId xid;

		itemid = PageGetItemId(targpage, scan->rs_cindex);

		/*
		 * For zheap, we need to count delete committed rows towards dead rows
		 * which would have been same, if the tuple was present in heap.
		 */
		if (ItemIdIsDeleted(itemid))
		{
			*deadrows += 1;
			continue;
		}

		/*
		 * We ignore unused and redirect line pointers.  DEAD line pointers
		 * should be counted as dead, because we need vacuum to run to get rid
		 * of them.  Note that this rule agrees with the way that
		 * heap_page_prune() counts things.
		 */
		if (!ItemIdIsNormal(itemid))
		{
			if (ItemIdIsDead(itemid))
				*deadrows += 1;
			continue;
		}

		/* Allocate memory for target tuple. */
		targtuple = zheap_gettuple(scan->rs_base.rs_rd, scan->rs_cbuf,
								   scan->rs_cindex);

		switch (ZHeapTupleSatisfiesOldestXmin(targtuple, OldestXmin,
											  scan->rs_cbuf, true,
											  &targtuple, &xid, NULL))
		{
			case ZHEAPTUPLE_LIVE:
				sample_it = true;
				*liverows += 1;
				break;

			case ZHEAPTUPLE_DEAD:
			case ZHEAPTUPLE_RECENTLY_DEAD:
				/* Count dead and recently-dead rows */
				*deadrows += 1;
				break;

			case ZHEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * Insert-in-progress rows are not counted.  We assume that
				 * when the inserting transaction commits or aborts, it will
				 * send a stats message to increment the proper count.  This
				 * works right only if that transaction ends after we finish
				 * analyzing the table; if things happen in the other order,
				 * its stats update will be overwritten by ours.  However, the
				 * error will be large only if the other transaction runs long
				 * enough to insert many tuples, so assuming it will finish
				 * after us is the safer option.
				 *
				 * A special case is that the inserting transaction might be
				 * our own.  In this case we should count and sample the row,
				 * to accommodate users who load a table and analyze it in one
				 * transaction.  (pgstat_report_analyze has to adjust the
				 * numbers we send to the stats collector to make this come
				 * out right.)
				 */
				if (TransactionIdIsCurrentTransactionId(xid))
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			case ZHEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * We count delete-in-progress rows as still live, using the
				 * same reasoning given above; but we don't bother to include
				 * them in the sample.
				 *
				 * If the delete was done by our own transaction, however, we
				 * must count the row as dead to make pgstat_report_analyze's
				 * stats adjustments come out right.  (Note: this works out
				 * properly when the row was both inserted and deleted in our
				 * xact.)
				 */
				if (TransactionIdIsCurrentTransactionId(xid))
					*deadrows += 1;
				else
					*liverows += 1;
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		if (sample_it)
		{
			ExecStoreZHeapTuple(targtuple, slot, false);
			scan->rs_cindex++;

			/* note that we leave the buffer locked here! */
			return true;
		}

		/* Free memory for target tuple. */
		if (targtuple)
			zheap_freetuple(targtuple);
	}

	/* Now release the lock and pin on the page */
	UnlockReleaseBuffer(scan->rs_cbuf);
	scan->rs_cbuf = InvalidBuffer;

	return false;
}

static bool
zheap_scan_sample_next_block(TableScanDesc sscan, struct SampleScanState *scanstate)
{
	ZHeapScanDesc scan = (ZHeapScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	/* at least meta page should be there */
	Assert(scan->rs_nblocks > 0);

	/* return false immediately if relation is empty */
	if (scan->rs_nblocks == ZHEAP_METAPAGE + 1)
		return false;

nextblock:
	if (tsm->NextSampleBlock)
	{
		blockno = tsm->NextSampleBlock(scanstate, scan->rs_nblocks);
		scan->rs_cblock = blockno;
	}
	else
	{
		/* scanning table sequentially */

		if (scan->rs_cblock == InvalidBlockNumber)
		{
			Assert(!scan->rs_inited);
			blockno = scan->rs_startblock;
		}
		else
		{
			Assert(scan->rs_inited);

			blockno = scan->rs_cblock + 1;

			if (blockno >= scan->rs_nblocks)
			{
				/* wrap to beginning of rel, might not have started at 0 */
				blockno = 0;
			}

			/*
			 * Report our new scan position for synchronization purposes.
			 *
			 * Note: we do this before checking for end of scan so that the
			 * final state of the position hint is back at the start of the
			 * rel.  That's not strictly necessary, but otherwise when you run
			 * the same query multiple times the starting position would shift
			 * a little bit backwards on every invocation, which is confusing.
			 * We don't guarantee any specific ordering in general, though.
			 */
			if (scan->rs_base.rs_flags & SO_ALLOW_SYNC)
				ss_report_location(scan->rs_base.rs_rd, blockno);

			if (blockno == scan->rs_startblock)
			{
				blockno = InvalidBlockNumber;
			}
		}
	}

	if (!BlockNumberIsValid(blockno))
	{
		if (BufferIsValid(scan->rs_cbuf))
			ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
		scan->rs_cblock = InvalidBlockNumber;
		scan->rs_inited = false;

		return false;
	}

	scan->rs_inited = true;

	/*
	 * If the target block isn't valid, e.g. because it's a tpd page, got to
	 * the next block.
	 */
	if (!zheapgetpage(sscan, blockno))
	{
		CHECK_FOR_INTERRUPTS();
		goto nextblock;
	}

	return true;
}

static bool
zheap_scan_sample_next_tuple(TableScanDesc sscan, struct SampleScanState *scanstate, TupleTableSlot *slot)
{
	ZHeapScanDesc scan = (ZHeapScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno = scan->rs_cblock;
	bool		pagemode = (sscan->rs_flags & SO_ALLOW_PAGEMODE) != 0;
	Page		page = NULL;
	bool		all_visible = false;
	OffsetNumber maxoffset;
	uint8		vmstatus;
	Buffer		vmbuffer = InvalidBuffer;

	ExecClearTuple(slot);

	/*
	 * When not using pagemode, we must lock the buffer during tuple
	 * visibility checks.
	 */
	if (!pagemode)
	{
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = (Page) BufferGetPage(scan->rs_cbuf);
		maxoffset = PageGetMaxOffsetNumber(page);

		if (!scan->rs_base.rs_snapshot->takenDuringRecovery)
		{
			vmstatus = visibilitymap_get_status(scan->rs_base.rs_rd,
												BufferGetBlockNumber(scan->rs_cbuf),
												&vmbuffer);

			all_visible = vmstatus;

			if (BufferIsValid(vmbuffer))
			{
				ReleaseBuffer(vmbuffer);
				vmbuffer = InvalidBuffer;
			}
		}
		else
			all_visible = false;
	}
	else
	{
		maxoffset = scan->rs_ntuples;
	}

	for (;;)
	{
		OffsetNumber tupoffset;

		CHECK_FOR_INTERRUPTS();

		/* Ask the tablesample method which tuples to check on this page. */
		tupoffset = tsm->NextSampleTuple(scanstate,
										 blockno,
										 maxoffset);

		if (OffsetNumberIsValid(tupoffset))
		{
			ZHeapTuple	tuple;

			if (!pagemode)
			{
				ItemId		itemid;
				bool		visible;
				ItemPointerData tid;

				/* Skip invalid tuple pointers. */
				itemid = PageGetItemId(page, tupoffset);
				if (!ItemIdIsNormal(itemid))
					continue;

				ItemPointerSet(&tid, blockno, tupoffset);
				if (all_visible)
				{
					tuple = zheap_gettuple(scan->rs_base.rs_rd, scan->rs_cbuf,
										   tupoffset);
					visible = true;
				}
				else
				{
					visible = ZHeapTupleFetch(scan->rs_base.rs_rd,
											  scan->rs_cbuf,
											  tupoffset,
											  scan->rs_base.rs_snapshot,
											  &tuple, NULL);
				}

				/*
				 * If any prior version is visible, we pass latest visible as
				 * true. The state of latest version of tuple is determined by
				 * the called function.
				 *
				 * Note that, it's possible that tuple is updated in-place and
				 * we're seeing some prior version of that. We handle that
				 * case in ZHeapTupleHasSerializableConflictOut.
				 */
				CheckForSerializableConflictOut(visible, scan->rs_base.rs_rd, (void *) &tid,
												scan->rs_cbuf, scan->rs_base.rs_snapshot);

				/* Try next tuple from same page. */
				if (!visible)
					continue;

				ExecStoreZHeapTuple(tuple, slot, false);

				/* Found visible tuple, return it. */
				LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

				/* Count successfully-fetched tuples as heap fetches */
				pgstat_count_heap_getnext(scan->rs_base.rs_rd);

				return true;
			}
			else
			{
				tuple = scan->rs_visztuples[tupoffset - 1];
				if (tuple == NULL)
					continue;

				ExecStoreZHeapTuple(tuple, slot, false);

				return true;
			}
		}
		else
		{
			/*
			 * If we get here, it means we've exhausted the items on this page
			 * and it's time to move to the next.
			 */
			if (!pagemode)
				LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			break;
		}
	}

	return false;
}

static void
zheapam_relation_nontransactional_truncate(Relation rel)
{
	/*
	 * Don't truncate the meta page.  We'll re-initialize it later.
	 */
	RelationTruncate(rel, ZHEAP_METAPAGE + 1);

	/*
	 * Re-Initialize the existing meta page.
	 */
	ZheapInitMetaPage(rel->rd_node, MAIN_FORKNUM,
					  rel->rd_rel->relpersistence,
					  true);
}

static void
zheap_copy_for_cluster(Relation OldHeap, Relation NewHeap,
					   Relation OldIndex, bool use_sort,
					   TransactionId OldestXmin,
					   TransactionId *xid_cutoff,
					   MultiXactId *multi_cutoff,
					   double *num_tuples,
					   double *tups_vacuumed,
					   double *tups_recently_dead)
{
	RewriteZheapState rwstate;
	IndexScanDesc indexScan;
	TableScanDesc heapScan;
	bool		use_wal;
	Tuplesortstate *tuplesort;
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	TupleTableSlot *slot;
	int			natts;
	Datum	   *values;
	bool	   *isnull;

	*xid_cutoff = InvalidTransactionId;
	*multi_cutoff = InvalidMultiXactId;

	/*
	 * We need to log the copied data in WAL iff WAL archiving/streaming is
	 * enabled AND it's a WAL-logged rel.
	 */
	use_wal = XLogIsNeeded() && RelationNeedsWAL(NewHeap);

	/* use_wal off requires smgr_targblock be initially invalid */
	Assert(RelationGetTargetBlock(NewHeap) == InvalidBlockNumber);

	/* Preallocate values/isnull arrays */
	natts = newTupDesc->natts;
	values = (Datum *) palloc(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/* Initialize the rewrite operation */
	rwstate = begin_zheap_rewrite(OldHeap, NewHeap, OldestXmin, *xid_cutoff,
								  *multi_cutoff, use_wal);


	/* Set up sorting if wanted */
	if (use_sort)
		tuplesort = tuplesort_begin_cluster(oldTupDesc, OldIndex,
											maintenance_work_mem,
											NULL, false);
	else
		tuplesort = NULL;

	/*
	 * Prepare to scan the OldHeap.
	 *
	 * We don't have a way to copy visibility information in zheap, so we just
	 * copy LIVE tuples.  See comments atop rewritezheap.c
	 *
	 * While scanning, we skip meta and tpd pages (done by *getnext API's)
	 * which is okay because we mark the tuples as frozen.  However, when we
	 * extend current implementation to copy visibility information of tuples,
	 * we would require to copy meta page and or TPD page information as well
	 */
	if (OldIndex != NULL && !use_sort)
	{
		heapScan = NULL;
		indexScan = index_beginscan(OldHeap, OldIndex, GetTransactionSnapshot(), 0, 0);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		heapScan = table_beginscan(OldHeap, GetTransactionSnapshot(), 0, (ScanKey) NULL);
		indexScan = NULL;
	}

	slot = table_slot_create(OldHeap, NULL);

	/*
	 * Scan through the OldHeap, either in OldIndex order or sequentially;
	 * copy each tuple into the NewHeap, or transiently to the tuplesort
	 * module.  Note that we don't bother sorting dead tuples (they won't get
	 * to the new table anyway).  While scanning, we skip meta and tpd pages
	 * (done by *getnext API's) which is okay because we mark the tuples as
	 * frozen.  However, when we extend current implementation to copy
	 * visibility information of tuples, we would require to copy meta page
	 * and or TPD page information as well.
	 */
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			if (!index_getnext_slot(indexScan, ForwardScanDirection, slot))
				break;

			/* Since we used no scan keys, should never need to recheck */
			if (indexScan->xs_recheck)
				elog(ERROR, "CLUSTER does not support lossy index conditions");
		}
		else
		{
			if (!table_scan_getnextslot(heapScan, ForwardScanDirection, slot))
				break;
		}

		num_tuples += 1;
		if (tuplesort != NULL)
			tuplesort_putheaptuple(tuplesort, ExecFetchSlotHeapTuple(slot, false, NULL));
		else
		{
			zheap_deform_tuple(ExecGetZHeapTupleFromSlot(slot), oldTupDesc,
							   values, isnull, oldTupDesc->natts);
			reform_and_rewrite_ztuple(oldTupDesc, newTupDesc,
									  values, isnull, rwstate);
		}
	}

	if (indexScan != NULL)
		index_endscan(indexScan);
	if (heapScan != NULL)
		table_endscan(heapScan);

	ExecDropSingleTupleTableSlot(slot);

	/*
	 * In scan-and-sort mode, complete the sort, then read out all live tuples
	 * from the tuplestore and write them to the new relation.
	 */
	if (tuplesort != NULL)
	{
		tuplesort_performsort(tuplesort);

		for (;;)
		{
			HeapTuple	heapTuple;

			CHECK_FOR_INTERRUPTS();

			heapTuple = tuplesort_getheaptuple(tuplesort, true);
			if (heapTuple == NULL)
				break;

			heap_deform_tuple(heapTuple, oldTupDesc, values, isnull);

			reform_and_rewrite_ztuple(oldTupDesc, newTupDesc,
									  values, isnull, rwstate);
		}

		tuplesort_end(tuplesort);
	}

	/* Write out any remaining tuples, and fsync if needed */
	end_zheap_rewrite(rwstate);

	/* Clean up */
	pfree(values);
	pfree(isnull);
}

static void
zheapam_set_new_filenode(Relation rel, const RelFileNode *newrnode,
						 char persistence,
						 TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	srel = RelationCreateStorage(*newrnode, persistence);

	/* initialize the meta page for zheap */
	ZheapInitMetaPage(*newrnode, MAIN_FORKNUM, persistence, false);

	/*
	 * If required, set up an init fork for an unlogged table so that it can
	 * be correctly reinitialized on restart.  An immediate sync is required
	 * even if the page has been logged, because the write did not go through
	 * shared_buffers and therefore a concurrent checkpoint may have moved the
	 * redo pointer past our xlog record.  Recovery may as well remove it
	 * while replaying, for example, XLOG_DBASE_CREATE or XLOG_TBLSPC_CREATE
	 * record. Therefore, logging is necessary even if wal_level=minimal.
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);

		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrnode, INIT_FORKNUM);
		smgrimmedsync(srel, INIT_FORKNUM);

		/* ZBORKED: This causes separate WAL, which doesn't seem optimal */
		ZheapInitMetaPage(*newrnode, INIT_FORKNUM, persistence, false);
	}

	smgrclose(srel);
}

static void
zheapam_relation_copy_data(Relation rel, const RelFileNode *newrnode)
{
	SMgrRelation dstrel;

	dstrel = smgropen(*newrnode, rel->rd_backend);
	RelationOpenSmgr(rel);

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * NOTE: any conflict in relfilenode value will be caught in
	 * RelationCreateStorage().
	 */
	RelationCreateStorage(*newrnode, rel->rd_rel->relpersistence);

	/* copy main fork */
	copy_zrelation_data(rel, dstrel);

	/* copy those extra forks that exist */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(rel->rd_smgr, forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrnode, forkNum);

			/*
			 * In zheap, other forks don't have any undo operation associated
			 * with them.  Hence, we don't need to undergo the costly process
			 * of calling copy_zrelation_data where we read the buffers,
			 * perform undo actions and then copy them.  We can simply copy
			 * the buffers at smgr level.
			 */
			RelationCopyStorage(rel->rd_smgr, dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	/* drop old relation, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static uint64
zheapam_relation_size(Relation rel, ForkNumber forkNumber)
{
	uint64		nblocks = 0;

	/* Open it at the smgr level if not already done */
	RelationOpenSmgr(rel);

	/* InvalidForkNumber indicates returning the size for all forks */
	if (forkNumber == InvalidForkNumber)
	{
		for (int i = 0; i < MAX_FORKNUM; i++)
			nblocks += smgrnblocks(rel->rd_smgr, i);
	}
	else
		nblocks = smgrnblocks(rel->rd_smgr, forkNumber);

	return nblocks * BLCKSZ;
}

/*
 * Check to see whether the table needs a TOAST table.  It does only if
 * (1) there are any toastable attributes, and (2) the maximum length
 * of a tuple could exceed TOAST_TUPLE_THRESHOLD.  (We don't want to
 * create a toast table for something like "f1 varchar(20)".)
 */
static bool
zheapam_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
		{
			/* Fixed-length types are never toastable */
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != 'p')
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(SizeofHeapTupleHeader +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}


static void
zheapam_estimate_rel_size(Relation rel, int32 *attr_widths,
						  BlockNumber *pages, double *tuples, double *allvisfrac)
{
	BlockNumber curpages;
	BlockNumber relpages;
	double		reltuples;
	BlockNumber relallvisible;
	double		density;

	/* it has storage, okay to call the smgr */
	curpages = RelationGetNumberOfBlocks(rel);

	/* coerce values in pg_class to more desirable types */
	relpages = (BlockNumber) rel->rd_rel->relpages;
	reltuples = (double) rel->rd_rel->reltuples;
	relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

	/* subtract one page to account for the meta page */
	if (curpages > 0)
		curpages--;
	if (relpages > 0)
		relpages--;

	/*
	 * HACK: if the relation has never yet been vacuumed, use a minimum size
	 * estimate of 10 pages.  The idea here is to avoid assuming a
	 * newly-created table is really small, even if it currently is, because
	 * that may not be true once some data gets loaded into it.  Once a vacuum
	 * or analyze cycle has been done on it, it's more reasonable to believe
	 * the size is somewhat stable.
	 *
	 * (Note that this is only an issue if the plan gets cached and used again
	 * after the table has been filled.  What we're trying to avoid is using a
	 * nestloop-type plan on a table that has grown substantially since the
	 * plan was made.  Normally, autovacuum/autoanalyze will occur once enough
	 * inserts have happened and cause cached-plan invalidation; but that
	 * doesn't happen instantaneously, and it won't happen at all for cases
	 * such as temporary tables.)
	 *
	 * We approximate "never vacuumed" by "has relpages = 0", which means this
	 * will also fire on genuinely empty relations.  Not great, but
	 * fortunately that's a seldom-seen case in the real world, and it
	 * shouldn't degrade the quality of the plan too much anyway to err in
	 * this direction.
	 *
	 * If the table has inheritance children, we don't apply this heuristic.
	 * Totally empty parent tables are quite common, so we should be willing
	 * to believe that they are empty.
	 */
	if (curpages < 10 &&
		relpages == 0 &&
		!rel->rd_rel->relhassubclass)
		curpages = 10;

	/* report estimated # pages */
	*pages = curpages;
	/* quick exit if rel is clearly empty */
	if (curpages == 0)
	{
		*tuples = 0;
		*allvisfrac = 0;
		return;
	}

	/* estimate number of tuples from previous tuple density */
	if (relpages > 0)
		density = reltuples / (double) relpages;
	else
	{
		/*
		 * When we have no data because the relation was truncated, estimate
		 * tuple width from attribute data types.  We assume here that the
		 * pages are completely full, which is OK for tables (since they've
		 * presumably not been vacuumed yet) but is probably an overestimate
		 * for indexes.  Fortunately get_relation_info() can clamp the
		 * overestimate to the parent table's size.
		 *
		 * Note: this code intentionally disregards alignment considerations,
		 * because (a) that would be gilding the lily considering how crude
		 * the estimate is, and (b) it creates platform dependencies in the
		 * default plans which are kind of a headache for regression testing.
		 */
		int32		tuple_width;

		tuple_width = get_rel_data_width(rel, attr_widths);
		tuple_width += MAXALIGN(SizeofZHeapTupleHeader);
		tuple_width += sizeof(ItemIdData);
		/* note: integer division is intentional here */
		density = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
	}
	*tuples = rint(density * (double) curpages);

	/*
	 * We use relallvisible as-is, rather than scaling it up like we do for
	 * the pages and tuples counts, on the theory that any pages added since
	 * the last VACUUM are most likely not marked all-visible.  But costsize.c
	 * wants it converted to a fraction.
	 */
	if (relallvisible == 0 || curpages <= 0)
		*allvisfrac = 0;
	else if ((double) relallvisible >= curpages)
		*allvisfrac = 1;
	else
		*allvisfrac = (double) relallvisible / curpages;
}

static const TableAmRoutine zheapam_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = zheapam_slot_callbacks,

	.scan_begin = zheap_beginscan,
	.scan_getnextslot = zheap_getnextslot,
	.scan_end = zheap_endscan,
	.scan_rescan = zheap_rescan,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = zheapam_begin_index_fetch,
	.index_fetch_reset = zheapam_reset_index_fetch,
	.index_fetch_end = zheapam_end_index_fetch,
	.index_fetch_tuple = zheapam_index_fetch_tuple,

	.tuple_insert = zheapam_insert,
	.tuple_insert_speculative = zheapam_insert_speculative,
	.tuple_complete_speculative = zheapam_complete_speculative,
	.multi_insert = zheap_multi_insert,
	.tuple_delete = zheapam_delete,
	.tuple_update = zheapam_update,
	.tuple_lock = zheapam_lock_tuple,
	/* finish_bulk_insert is currently not needed */

	.tuple_fetch_row_version = zheapam_fetch_row_version,
	.tuple_get_latest_tid = zheap_get_latest_tid,
	.tuple_tid_valid = zheapam_tuple_tid_valid,
	.tuple_satisfies_snapshot = zheapam_tuple_satisfies_snapshot,
	.compute_xid_horizon_for_tuples = zheap_compute_xid_horizon_for_tuples,

	.relation_vacuum = lazy_vacuum_zheap_rel,
	.relation_nontransactional_truncate = zheapam_relation_nontransactional_truncate,
	.relation_copy_for_cluster = zheap_copy_for_cluster,
	.relation_set_new_filenode = zheapam_set_new_filenode,
	.relation_copy_data = zheapam_relation_copy_data,
	.relation_estimate_size = zheapam_estimate_rel_size,
	.scan_analyze_next_block = zheapam_scan_analyze_next_block,
	.scan_analyze_next_tuple = zheapam_scan_analyze_next_tuple,
	.index_build_range_scan = IndexBuildZHeapRangeScan,
	.index_validate_scan = validate_index_zheapscan,

	.relation_size = zheapam_relation_size,
	.relation_needs_toast_table = zheapam_relation_needs_toast_table,

	.scan_bitmap_next_block = zheap_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = zheap_scan_bitmap_next_tuple,
	.scan_sample_next_block = zheap_scan_sample_next_block,
	.scan_sample_next_tuple = zheap_scan_sample_next_tuple
};

Datum
zheap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&zheapam_methods);
}
