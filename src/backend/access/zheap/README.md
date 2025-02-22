src/backend/access/zheap/README

Zheap
=====

The main purpose of this README is to provide an overview of the current
design of zheap, a new storage format for PostgreSQL.  This project has three
major objectives:

1. Provide better control over bloat.  In the existing  heap, we always create
a new version of tuple when it is  updated. These new versions are later
removed by vacuum or hot-pruning, but this only frees up space for reuse by
future inserts or updates; nothing is returned to the operating system.  A
similar problem occurs for tuples that are deleted. zheap will prevent bloat
(a) by allowing in-place updates in common cases and (b) by reusing space as
soon as a transaction that has performed a delete or non-in-place-update has
committed.  In short, with this new storage, whenever possible, we’ll avoid
creating bloat in the first place.

2. Reduce write amplification both by avoiding rewrites of heap pages and by
making it possible to do an update that touches indexed columns without
updating every index.

3. Reduce the tuple size by (a) shrinking the  tuple header and
(b) eliminating most alignment padding.

In-place updates will be supported except when (a) the new tuple is larger
than the old tuple and the increase in size makes it impossible to fit the
larger tuple onto the same page or (b) some column is modified which is
covered by an index that has not been modified to support “delete-marking”.
We have not begun work on delete-marking support for indexes yet, but intend
to support it at least for btree indexes.

General idea of zheap with undo
--------------------------------
Each backend is attached to a separate undo log to which it writes undo
records.  Each undo record is identified by a 64-bit undo record pointer of
which the first 24 bits are used for the log number and the remaining 40 bits
are used for an offset within that undo log.  Only one transaction at a time
can write to any given undo log, so the undo records for any given transaction
are always consecutive.

Each zheap page has fixed set of transaction slots each of which contains the
transaction information (transaction id and epoch) and the latest undo record
pointer for that transaction.  As of now, we have four transaction slots per
page, but this can be changed.  Currently, this is a compile-time option;  we
can decide later whether such an option is desirable in general for users.
Each transaction slot occupies 16 bytes. We allow the transaction slots to be
reused after the transaction is committed which allows us to operate without
needing too many slots.  We can allow slots to be reused after a transaction
abort as well, once undo actions are complete.  We have observed that smaller
tables say having very few pages typically need more slots; for larger tables,
four slots are enough.  In our internal testing, we have found that 16 slots
give a very good performance, but more tests are needed to identify the right
number of slots.  The one known problem with the fixed number of slots is that
it can lead to deadlock, so we are planning to add  a mechanism to allow the
array of transactions slots to be continued on a separate overflow page.  We
also need such a mechanism to support cases where a large number of
transactions acquire SHARE or KEY SHARE locks on a single page.  The overflow
pages will be stored in the zheap itself, interleaved with regular pages.
These overflow pages will be marked in such a way that sequential scans will
ignore them.  We will have a meta page in zheap from which all overflow pages
will be tracked.

Typically, each zheap operation that modifies a page needs to first allocate a
transaction slot on that page and then prepare an undo record for the operation.
Then, in a critical section, it must write the undo record, perform the
operation on heap page, update the transaction slot in a page, and finally
write a WAL record for the operation.  What we write as part of undo record
and WAL depends on the operation.

Insert: Apart from the generic info, we write the TID (block number and offset
number) of the tuple in undo record to identify the record during undo replay.
In WAL, we write the offset number and the tuple, plus some minimal
information which will be needed to regenerate the undo record during replay.

Delete: We write the complete tuple in the undo record even though we could get
away with just  writing the TID as we do for an insert operation.  This allows
us to reuse the space occupied by the deleted record as soon as the transaction
that has performed the operation commits.  In WAL, we need to write the tuple
only if full page writes are not enabled.  If full page writes are enabled, we
can rely on the page state to be same during recovery as it is during the
actual operation, so we can retrieve the tuple from page to copy it into the
undo record.

Update: For in-place updates, we have to write the old tuple in the undo log
and the new tuple in the zheap.  We could optimize and write the diff tuple
instead of the complete tuple in undo, but as of now, we are writing the
complete tuple.  For non-in-place updates, we write the old tuple and the new
TID in undo; essentially this is equivalent to DELETE+INSERT.  As for DELETE,
this allows space to be recycled as soon as the updating transaction commits.
In the WAL, we write a copy of the old tuple only if full pages writes are off
and we write diff tuple for the new tuple (irrespective of the value of
full-page writes) as we do in a current heap.  In the case where a
non-in-place-update happens to insert new tuple on a separate page, we write
two undo records, one for old page and another for the new page.  One can
imagine that writing one undo record would be sufficient as we generally reach
to a new tuple from old tuple if required, but we want to maintain a separate
undo chain for each page.

Select .. For [Key] Share/Update
Tuple locking will work much like a DML operation: reserve a transaction slot,
update the tuple header with the lock information, write UNDO and WAL for the
operation.  To detect conflicts, we sometimes need to traverse the undo chains
of all the active transactions on a page.  We will always mark the tuple with
the strongest lock mode that might be present, just as is done in the current
heap, so that we can cheaply detect whether there is a potential conflict.  If
there is, we must get information about all the locks from undo in order to
decide whether there is an actual conflict.  The tuple will always contain
either the strongest locker information or if all the lockers are of same
strength, then it will contain the latest locker information.  Whenever there
is more than one locker operating on a tuple, we set the multi-locker bit on a
tuple to indicate that the tuple has multiple lockers. Note, that we clear the
multi-locker bit lazily (which means when we decide to wait for all the
lockers to go away and there is no more locker alive on the tuple).  During
Rollback operation, we retain the strongest locker information on the tuple
if there are multiple lockers on a tuple. This is because the conflict
detection mechanism works based on strongest locker.  Now, even if we want to
remove strongest locker information, we don't have second strongest locker
information handy.

Copy: Similar to insert, we need to store the corresponding TID (block number,
offset number) for a tuple in undo to identify the same during undo replay. But,
we can minimize the number of undo records written for a page. First, we
identify the unused offset ranges for a page, then insert one undo record for
each offset range. For example, if we’re about to insert in offsets
(2,3,5,9,10,11), we insert three undo records covering offset ranges (2,3),
(5,5), and (9,11), respectively. For recovery, we insert a single WAL record
containing the above-mentioned offset ranges along with some minimal
information to regenerate the undo records and tuples.

Scans: During scans, we need to make a copy of the tuple instead of just
holding the pin on a page.  In the current heap, holding a pin on the buffer
containing the tuple is sufficient because operations like vacuum which can
rearrange the page always take a cleanup lock on a buffer. In zheap, however,
in-place-updates work with just a exclusive lock on a buffer, so a tuple to
which we hold a pointer might be updated under us.

Insert .. On Conflict: The design is similar to current heap such that we use the
speculative token to detect conflicts.  We store the speculative token in undo
instead of in the tuple header (CTID) simply because zheap’s tuple header
doesn’t have CTID. Additionally, we set a bit in tuple header to indicate
speculative insertion.  ZheapTupleSatisfiesDirty routine checks this bit and
fetches a speculative token from undo.

Toast Tables: Toast tables can use zheap, too.  Since zheap uses shorter tuple
headers, this saves space. In the future, someone might want to support
in-place updates for toast table data instead of doing delete+insert as we do
today.

Transaction slot reuse
-----------------------
Transaction slots can be freely reused if the transaction is committed and
all-visible, or if the transaction is aborted and undo actions for that
transaction, at least relating to that page, have been performed.  If the
transaction is committed but not yet all-visible, we can reuse the slot after
writing an additional, special undo record that lets us make subsequent tuple
visibility decisions correctly.

For committed transactions, there are two possibilities.  If the transaction
slot is not referenced by any tuple in the page, we simply clear the xid from
the transaction slot. The undo record pointer is kept as it is to ensure that
we don't break the undo chain for that slot.  Otherwise, we write an undo
record for each tuple that points to one of the committed transactions.  We
also mark the tuple indicating that the associated slot has been reused.  In
such a case, it is quite possible that the tuple has not been modified, but it
is still pointing to transaction slot which has been reused for a new
transaction which is not yet all-visible.  During the visibility check for
such a tuple, it might appear that the tuple is modified by a current
transaction which is clearly wrong and can lead to wrong results.

Subtransactions
----------------
zheap only uses the toplevel transaction ID; subtransactions that modify a
zheap do not need separate transaction IDs.  In the regular heap, when
subtransactions are present, the subtransaction’s XID is used to make tuple
visibility decisions correctly.  In a zheap, subtransaction abort is instead
handled by using undo to reverse changes to the zheap pages. This design
minimizes consumption of transaction slots and pg_xact space, and ensures that
all undo records for a toplevel transaction remain consecutive in the undo
log.

Reclaiming space within a page
-------------------------------
Space can be reclaimed within a page after (a) a delete, (b) a non-in-place
update, or (c) an in-place update that reduces the width of the tuple. We can
reuse the space when as soon as  the transaction that has performed the
operation has committed.  We can also reclaim space after inserts or
non-in-place updates have been undone.  There is some difference between the
way space is reclaimed for transactions that are committed and all-visible vs.
the transactions that are committed but still not all-visible. In the former
case, we can just indicate in the line pointer that the corresponding item is
dead whereas for later we need the capability to fetch the prior version of a
tuple for transactions to which the delete is not visible. To allow that, we
copy the transaction slot information into the line pointer so that we can
easily reach the prior version of the tuple.  As a net result, the space for a
deleted tuple can be reclaimed immediately after the delete commits, but the
space consumed by line pointer can only be freed once we delete the
corresponding index tuples.  For an aborted transaction, space can be
reclaimed once undo is complete.  We set the prune xid in page header during
delete or update operations and during rollback of inserts to permit pruning
to happen only when there is a possible benefit.  When we try to prune, we
first check if the prune xid is in progress; only if not will we attempt to
prune the page.

Pruning will be attempted when update operation lands to a page where there is
not enough space to accommodate a new tuple.  We can also allow pruning to
occur when we evict the page from shared buffers or read the page from disk as
those are I/O intensive operations, so doing some CPU intensive operation
doesn't cost much.

With the above idea, it is quite possible that sometimes we try to prune the
page when there is no immediate benefit of doing so. For example, even after
pruning, the page might still not have enough space in the page to accommodate
new tuple.  One idea is to track the space at the transaction slot level, so
that we can know exactly how much space can be freed in page after pruning,
but that will lead to increase in a space used by each transaction slot.

We can also reuse space if a transaction frees up space on the page (e.g. by
delete) and then tries to use additional space (e.g. by a subsequent insert).
We can’t in general reuse space freed up by a transaction until it commits,
because if it aborts we’ll need that space during undo; but an insert or
update could reuse space freed up by earlier operations in the same
transaction, since all or none of them will roll back. This is a good
optimization, but this needs some more thought.

Free Space Map
---------------
We can optimistically update the freespace map when we remove the tuples from
a page in the hope that eventually most of the transactions will commit and
space will be available. Additionally, we might want to update FSM during
aborts when space-consuming actions like inserts are rolled back.  When
requesting free space, we would need to adjust things so that we continue the
search from the previous block instead of repeatedly returning the same block.

I think updating it on every such operation can be costly, so we can perform
it only after some threshold number, so later we might want to add a facility
to track potentially available freespace and merge into the main data
structure.  We also want to make FSM crash-safe, since we can’t count on
VACUUM to recover free space that we neglect to record.

Page format
------------
zheap uses a standard page header,  stores transaction slots in the special
space.

Tuple format
-------------
The tuple header is reduced from 24 bytes to 5 bytes (8 bytes with alignment):
2 bytes each for informask and infomask2, and one byte for t_hoff.  I think we
might be able to squeeze some space from t_infomask, but for now, I have kept
it as two bytes.  All transactional information is stored in undo, so fields
that store such information are not needed here.

The idea is that we occupy somewhat more space at the page level, but save
much more at tuple level, so we come out ahead overall.

Alignment padding
------------------
We omit all alignment padding for pass-by-value types. Even in the current heap,
we never point directly to such values, so the alignment padding doesn’t help
much; it lets us fetch the value using a single instruction, but that is all.
Pass-by-reference types will work as they do in the heap.  Many pass-by-reference
data types will be varlena data types (typlen = -1) with short varlena headers so
no alignment padding will be introduced in that case anyway, but if we have varlenas
with 4-byte headers or if we have fixed-length pass-by-reference types (e.g. interval,
box) then we'll still end up with padding.  We can't directly access unaligned values;
instead, we need to use memcpy.  We believe that the space savings will more than pay
for the additional CPU costs.

We don’t need alignment padding between the tuple header and the tuple data as
we always make a copy of the tuple to support in-place updates. Likewise, we ideally
don't need any alignment padding between tuples. However, there are places in zheap
code where we access tuple header directly from page (e.g. zheap_delete, zheap_update,
etc.) for which we want them to be aligned at two-byte boundary).

Undo chain
-----------
Each undo record header contains the location of previous undo record pointer
of the transaction that is performing the operation.  For example, if
transaction T1 has updated the tuple two times, the undo record for the last
update will have a link for undo record of the previous update.  Thus, the
undo records for a particular page in a particular transaction form a single,
linked chain.

Snapshots and visibility
-------------------------
Given a TID and a snapshot, there are three possibilities: (a) the tuple
currently stored at the given TID; (b) some tuple previously stored at the
given TID and subsequently written to the undo log might be visible; or
(c) there might be nothing visible at all.  To check the visibility of a
tuple, we fetch the transaction slot number stored in the tuple header, and
then get the transaction id and undo record pointer from transaction slot.
Next, we check the current tuple’s visibility based on transaction id fetched
from transaction slot and the last operation performed on the tuple.  For
example, if the last operation on tuple is a delete and the xid is visible to
our snapshot, then we return NULL indicating no visible tuple. But if the xid
that has last operated on tuple is not visible to the snapshot, then we use
the undo record pointer to fetch the prior tuple from undo and similarly check
its visibility.  The only difference in checking the visibility for the undo
tuple is that the xid that previously operated on undo tuple is present in the
undo record, so we can use that instead of relying on the transaction slot.
If the tuple from undo is also not visible, then we fetch the prior tuple from
the undo chain.  We need to traverse undo chains until we find a visible tuple
or reach the initially inserted tuple; if that is also not visible, we can
return NULL.

During visibility checking of a tuple in a zheap page or an undo chain, if we
find that the tuple’s transaction slot has been reused, we retrieve the
transaction information (xid and cid that has modified the tuple) of that
tuple from undo.

EvalPlanQual mechanism
-----------------------
This works in basically the same way as for the existing heap. The only
special consideration is that the updated tuple could have the same TID as the
original one if it was updated in place, so we might want to optimize such
that we need not release the buffer lock and again refetch the tuple.
However, at this stage, we are not sure if there is any big advantage in such
an optimization.

64-bit transaction ids
-----------------------
Transaction slots in zheap pages store both the epoch and the XID; this
eliminates the confusion between a use of a given XID in the current epoch and
a use in some previous epoch, which means that we never need to freeze tuples.
The difference between the oldest running XID and the newest XID is still
limited to 2 billion because of the way that snapshots work.  Moreover, the
oldest XID that still has undo must have an XID age less than 2 billion: among
other problems, this is currently the limit for how long commit status data
can be retained, and it would be bad if we had undo data but didn’t know
whether or not to apply the undo actions.  Currently, this limitation is
enforced by piggybacking on the existing wraparound machinery.

Indexing
---------
Current index AMs are not prepared to cope with multiple tuples at the same
TID with different values stored in the index column.  We plan to introduce
special index AM support for in-place updates; when an index lacks such
support, any modification to the value stored in a column covered by that
index will prevent the use of in-place update.  Additionally, indexes lacking
such support will still require routine vacuuming, which we believe can be
avoided when such support is present.

The basic idea is that we need to delete-mark index entries when they might no
longer be valid, either because of a delete or because of an update affecting
the indexed column.  An in-place update that does not modify the indexed
column need not delete-mark the corresponding index entries.  Note that an
entry which is delete-marked might still be valid for some snapshots; once no
relevant snapshots remain, we can remove the entry.  In some cases, we may
remove a delete-mark from an entry rather than removing the entry, either
because the transaction which applied the delete-mark has rolled back, or
because the indexed column was changed from value A to value B and then
eventually back to value A.
It is very desirable for performance reasons to have be able to distinguish
from the index page whether or not the corresponding heap tuple is definitely
all-visible, but the delete-marking approach is not quite sufficient for this
purpose unless recently-inserted tuples are also delete-marked -- and that is
undesirable, since the delete-markings would have to be cleared after the
inserting transaction committed, which might end up dirtying many or all
index pages.  An alternative approach is to write undo for index insertions;
then, the undo pointers in the index page tells us whether any index entries
on that page may be recently-inserted, and the presence or absence of a
delete-mark tells us whether any index entries on that page may no longer be
valid.  We intend to adopt this approach; it should allow index-only scans in
most cases without the need for a separately-maintained visibility map.

With this approach, an in-place update touches each index whose indexed
columns are modified twice -- once to delete-mark the old entry (or entries)
and once to insert the new entries.  In some use cases, this will compare
favorably with the existing approach, which touches every index exactly once.
Specifically, it figures to reduce write amplification and index bloat when
only one or a few indexed columns are updated at a time.

Indexes that don't have delete-marking
---------------------------------------
Although indexes which lack delete-marking support still require vacuum, we
can use undo to reduce the current three-pass approach to just two passes,
avoiding the final heap scan.  When a row is deleted, the vacuum will directly
mark the line pointer as unused, writing an undo record as it does,  and then
mark the corresponding index entries as dead.  If vacuum fails midway through
the undo can ensure that changes to the heap page are rolled back.  If the
vacuum goes on to commit, we don't need to revisit the heap page after index
cleanup.

We must be careful about  TID reuse: we will only allow a TID to be reused
when the transaction that has marked it as unused has committed. At that
point, we can be assured that all the index entries corresponding to dead
tuples will be marked as dead.

Undo actions
-------------
We need to apply undo actions during explicit ROLLBACK or ROLLBACK TO
SAVEPOINT operations and when an error causes a transaction or subtransaction
abort.  These actions reverse whatever work was done when the operation was
performed; for example, if an update aborts, we must restore the old version
of the tuple.  During an explicit ROLLBACK or ROLLBACK TO SAVEPOINT, the
transaction is in a good state and we have relevant locks on objects, so
applying undo actions is straightforward, but the same is not true in error
paths.  In the case of a subtransaction abort, undo actions are performed
after rolling back the subtransaction; the parent transaction is still good.
In the case of a top-level abort, we begin an entirely new transaction to
perform the undo actions.  If this new transaction aborts, it can be retried
later.  For short transactions (say, one which generates only few kB of undo
data), it is okay to apply the actions in the foreground but for longer
transactions, it is advisable to delegate the work to an undo worker running
in the background.  The user is provided with a knob to control this behavior.

Just like the DML operations to which they correspond, undo actions require us
to write WAL.  Otherwise, we would be unable to recover after a crash, and
standby servers would not be properly updated.

Applying undo actions
----------------------
In many cases, the same page will be modified multiple times by the same
transaction.  We can save locking and reduce WAL generation by collecting all
of the undo records for a given page and then applying them all at once.
However, it’s difficult to collect all of the records that might apply to a
page from an arbitrarily large undo log in an efficient manner; in particular,
we want to avoid rereading the same undo pages multiple times.  Currently, we
collect all consecutive records which apply to the same page and then apply
them at one shot.  This will cover the cases where most of the changes to heap
pages are performed together.  This algorithm could be improved.  For example,
we could do something like this:

1. Read the last 32MB of undo for the transaction being undone (or all of the
undo for the transaction, if there is less than 32MB).
2. For each block that is touched by at least one record in the 32MB chunk,
consolidate all records from this chunk that apply to that block.
3. Sort the blocks by buffer tag and apply the changes in ascending
block-number order within each relation.  Do this even for incomplete chains,
so nothing is saved for later.
4. Go to step 1.

After applying undo actions for a page, we clear the transaction slot on a
page if the oldest undo record we applied is the oldest undo record for that
block generated by that transaction. Otherwise, we rewind the undo pointer in
the page slot to the last record for that block that precedes the last undo
record we applied.  Because applying undo also always updates the transaction
slot on the page, either rewinding it or clearing it completely, we can
always skip applying undo if we find that it’s already been applied
previously.  This could happen if the application of undo for a given
transaction is interrupted a crash, or if it fails for some reason and is
retried later.

This also prevents us from getting confused when the relation is (a) dropped,
(b) rewritten using a new relfilenode, or (c) truncated to a shorter length
(and perhaps subsequently re-extended).  We apply the undo action only if the
page contains the effect of the transaction for which we are applying undo
actions, which can always be determined by examining the undo pointer in the
transaction slot. If there is no transaction slot for the current transaction
or if it is present but the undo record pointer in the slot is less than the
undo record pointer of the undo record under consideration, the undo record
can be ignored; it has already been applied or is no longer relevant.  After a
toplevel transaction abort, undo space is not recycled.  However, after a
subtransaction abort, we rewind the insert pointer to wherever it was at the
start of the subtransaction, so that the undo for the toplevel transaction
remains contiguous.  We can’t do the same for toplevel aborts as that might
contain special undo records related to transaction slots that were reused and
we can’t afford to lose those.  We write these special undo records only for
toplevel transaction when it doesn’t find any free transaction slot or there
is no transaction slot which contains transaction that is all-visible.  In
such cases, we reuse the committed transaction slots and write undo record
which contains transaction information for them as we might need that
information for transaction which still can’t see the committed transaction.
We mark all such slots (that belongs to committed transactions) as available
for reuse in one shot as doing it one slot at a time is quite costly.  Since
we might still need the special undo records for the transaction slots other
than the current transaction, we can’t simply rewind the insert pointer.  Note
that we do this only for toplevel transactions; if we need the new slot when
in a subtransaction, we reclaim only a single transaction slot.

WAL consideration
------------------
Undo records are critical data and must be protected via WAL.  Because an undo
record must be written if and only if a page modification occurs, the undo
record and the record for the page modification must be one and the same.
Moreover, it is very important not to duplicate any information or store any
unnecessary information, since WAL volume has a significant impact on overall
system performance.  In particular, there is no need to log the undo record
pointer.  We only need to ensure that after crash recovery undo record pointer
is set correctly for each of the undo logs.  To ensure that, we log a WAL
record after XID change or at the first operation after checkpoint on undo
log.  The WAL record contains the information of insert point, log number, and
Xid.  This is enough to form an XID->(Log no. + Log insertion point) map which
will be used to calculate the location of undo insertion during recovery.

Another important consideration is that we don't need to have full page images
for data in undo logs. Because the undo logs are always written serially, torn
pages are not an issue.  Suppose that  some block in one of the undo log is
half filled and synced properly to disk; now, a checkpoint occurs  Next, we
add some more data to the block.  During the following checkpoint, the system
crashes while flushing the block.  The block could be in a condition such that
first few bytes of it say 512 bytes are flushed appropriately and rest are
old, but this won't cause problem because anyway old bytes will be intact and
we can always start inserting new records at insert location in undo
reconstructed during recovery.

Undo Worker
------------
Currently, we have one background undo worker which performs undo actions as
required and discards undo logs when they are no longer needed.  Typically, it
performs undo actions in response to a notification from a backend that has
just aborted a transaction, but it will eventually detect and perform undo
actions for any aborted transaction that does not otherwise get cleaned up.

We allow the undo worker to hibernate when there is no activity in the system.
It hibernates for a minimum of 100ms and maximum of 10s, based on the time
the system has remained idle.  The undo worker mechanism will be extended to
multiple undo workers to perform various jobs related to undo logs. For
example, if there are many pending rollback requests, then we can spawn a new
undo worker which can help in processing the requests.

UndoDiscard routine will be called by the undo worker for discarding the old
undo records. UndoDiscard will process all the active undo logs.   It reads
each undo log and checks whether the log corresponding to the first
transaction in a log can be discarded (committed and all visible or aborted
and undo already applied). If so, it moves to the next transaction in that
undo log and continues in the same way. When it finds the first transaction
whose undo  can't be discard yet, it first discards the undo log prior to that
point and then remembers the transaction ID and undo location in shared memory.
We consider undo for a transaction to be discardable once its XID  is smaller
than oldestXmin.

Ideally, for the aborted transactions once the undo actions are replayed, we
should be able to discard it’s undo, however, it might contain the undo records
for reused transaction slots, so we can’t discard them until it becomes
smaller than oldestXmin.  Also, we can’t discard the undo for the aborted
transaction if there is a preceding transaction which is committed and not
all-visible.  We can allow undo for aborted transactions to be discarded
immediately if we remember in the first undo record of the transaction whether
it contains undo of reused transaction slot.  This will help the cases where
the aborted transaction is the last transaction in undo log which is smaller
than oldestXmin.

In Hot Standby mode, undo is discarded via WAL replay.  Before discarding
undo, we ensure that there are no queries running which need to get tuple from
discarded undo.  If there are any, a recovery conflict will occur, similar to
what happens in other cases where a resource held by a particular backend
prevents replay from advancing.

For each undo log, the undo discard module maintains in memory array to hold
the latest undiscarded xid and its start undo record pointer.  The first XID
in the undo log will be compared against GlobalXmin, if the xid is greater
than GlobalXmin then nothing can be discarded;  otherwise, scan  the undo log
starting with the oldest transaction it contains. To avoid processing every
record in the undo log, we maintain a transaction start header in the first
undo record written by any given transaction with space to store a pointer to
the next transaction start undo record in that same undo log. This allows us
to read an undo log transaction by transaction.  When discarding undo, the
background worker will read all active undo logs transaction by transaction
until it finds a transaction with an XID greater than equal to the GlobalXmin.
Once it finds such a transaction, it will discard all earlier undo records in
that undo log, without even writing unflushed buffers to disk.

Avoid fetching discarded undo record
-------------------------------------
The system must never attempt to fetch undo records which have already been
discarded.  Undo is generally discarded in the background by the undo worker,
so we must account for the possibility that undo could be discarded at any
time.  We do maintain the oldest xid that have undo (oldestXidHavingUndo).
Undo worker updates the value of oldestXidHavingUndo after discarding all the
undo.  Backends consider all transactions that precede oldestXidHavingUndo as
all-visible, so they normally don’t try to fetch the undo which is already
discarded.  However, there is a race condition where backend decides that the
transaction is greater than oldestXidHavingUndo and it needs to fetch the undo
record and in the meantime undo worker discards the corresponding undo record.
To handle such race conditions, we need to maintain some synchronization
between backends and undo worker so that backends don’t try to access already
discarded undo.  So whenever undo fetch is trying to read a undo record from
an undo log, first it needs to acquire a log->discard_lock in SHARED mode for
the undo log and check that the undo record pointer is not less than
log->oldest_data, if so, then don't fetch that undo record and return
NULL (that means the previous version is all visible).  And undo worker will
take log->discard_lock in EXCLUSIVE mode for updating the
log->oldest_data. We hold this lock just to update the value in shared
memory, the actual discard happens outside this lock.

Undo Log Storage
-----------------
This subsystem is responsible for life cycle management of undo logs and
backing files, associating undo logs with backends, allocating and managing
space within undo logs.  It provides access to undo log contents via shared
buffers. The list of available undo logs is maintained in shared memory.
Whenever a backend request for undo log allocation, it attaches a first free
undo log to a backend, and if all existing undo logs are busy, it will create
a new one. A set of APIs is provided by this subsystem to efficiently allocate
and discard undo logs.

During a checkpoint, all the undo segment files and undo meta data files will
be flushed to the disk.
