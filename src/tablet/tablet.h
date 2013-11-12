// Copyright (c) 2012, Cloudera, inc.
#ifndef KUDU_TABLET_TABLET_H
#define KUDU_TABLET_TABLET_H

#include <string>
#include <vector>

#include "common/generic_iterators.h"
#include "common/iterator.h"
#include "common/schema.h"
#include "common/predicate_encoder.h"
#include "gutil/atomicops.h"
#include "gutil/gscoped_ptr.h"
#include "gutil/macros.h"
#include "server/metadata.h"
#include "tablet/diskrowset.h"
#include "tablet/memrowset.h"
#include "tablet/lock_manager.h"
#include "tablet/rowset_tree.h"
#include "tablet/transaction_context.h"
#include "util/env.h"
#include "util/locks.h"
#include "util/status.h"
#include "util/slice.h"

namespace kudu {

class MetricContext;

namespace consensus {
class Consensus;
}

namespace tablet {

using std::string;
using std::tr1::shared_ptr;

class RowSetsInCompaction;
class CompactionPolicy;
struct TabletMetrics;

class Tablet {
 public:
  class CompactionFaultHooks;
  class FlushCompactCommonHooks;
  class FlushFaultHooks;
  class Iterator;

  // Create a new tablet.
  //
  // If 'parent_metrics_context' is non-NULL, then this tablet will store
  // metrics in a sub-context of this context. Otherwise, no metrics are collected.
  Tablet(gscoped_ptr<metadata::TabletMetadata> metadata,
         const MetricContext* parent_metric_context = NULL);

  ~Tablet();

  // Open the tablet.
  Status Open();

  // TODO update tests so that we can remove Insert() and Mutate()
  // and use only InsertUnlocked() and MutateUnlocked().

  // Creates a PreparedRowWrite with write_type() INSERT, acquires the row lock
  // for the row and creates a probe for later use. 'row_write' is set to the
  // PreparedRowWrite if this method returns OK.
  //
  // TODO when we get to remove the locked versions of Insert/Mutate we
  // can make the PreparedRowWrite own the row and can revert to passing just
  // the raw row data, but right now we need to pass the built ConstContinuousRow
  // as there are cases where row is passed as a reference (old API).
  Status CreatePreparedInsert(const TransactionContext* tx_ctx,
                              const ConstContiguousRow* row,
                              gscoped_ptr<PreparedRowWrite>* row_write);

  // Insert a new row into the tablet.
  //
  // The provided 'data' slice should have length equivalent to this
  // tablet's Schema.byte_size().
  //
  // After insert, the row and any referred-to memory (eg for strings)
  // have been copied into internal memory, and thus the provided memory
  // buffer may safely be re-used or freed.
  //
  // Returns Status::AlreadyPresent() if an entry with the same key is already
  // present in the tablet.
  // Returns Status::OK unless allocation fails.
  Status Insert(TransactionContext *tx_ctx, const ConstContiguousRow& row);

  // A version of Insert that does not acquire locks and instead assumes that
  // they were already acquired. Requires that handles for the relevant locks
  // and Mvcc transaction are present in the transaction context.
  Status InsertUnlocked(TransactionContext *tx_ctx,
                        const PreparedRowWrite* insert);

  // Creates a PreparedRowWrite with write_type() MUTATE, acquires the row lock
  // for the row and creates a probe for later use. 'row_write' is set to the
  // PreparedRowWrite if this method returns OK.
  //
  // TODO when we get to remove the locked versions of Insert/Mutate we
  // can make the PreparedRowWrite own the row and can revert to passing just
  // the raw row data, but right now we need to pass the built ConstContinuousRow
  // as there are cases where row is passed as a reference (old API).
  Status CreatePreparedMutate(const TransactionContext* tx_ctx,
                              const ConstContiguousRow* row_key,
                              const Schema* changelist_schema,
                              const RowChangeList* changelist,
                              gscoped_ptr<PreparedRowWrite>* row_write);

  // Update a row in this tablet.
  // The specified schema is the full user schema necessary to decode
  // the update RowChangeList.
  //
  // If the row does not exist in this tablet, returns
  // Status::NotFound().
  Status MutateRow(TransactionContext *tx_ctx,
                   const ConstContiguousRow& row_key,
                   const Schema& update_schema,
                   const RowChangeList& update);

  // A version of MutateRow that does not acquire locks and instead assumes
  // they were already acquired. Requires that handles for the relevant locks
  // and Mvcc transaction are present in the transaction context.
  Status MutateRowUnlocked(TransactionContext *tx_ctx,
                           const PreparedRowWrite* mutate);

  // Create a new row iterator which yields the rows as of the current MVCC
  // state of this tablet.
  // The returned iterator is not initialized.
  Status NewRowIterator(const Schema &projection,
                        gscoped_ptr<RowwiseIterator> *iter) const;

  // Create a new row iterator for some historical snapshot.
  Status NewRowIterator(const Schema &projection,
                        const MvccSnapshot &snap,
                        gscoped_ptr<RowwiseIterator> *iter) const;

  Status Flush();
  Status Flush(const Schema& schema);

  // Alter the schema of the tablet.
  // This operation will trigger a flush on the current MemRowSet
  // and on all the DeltaMemStores.
  Status AlterSchema(const Schema& schema);

  // Flags to change the behavior of compaction.
  enum CompactFlag {
    COMPACT_NO_FLAGS = 0,

    // Force the compaction to include all rowsets, regardless of the
    // configured compaction policy. This is currently only used in
    // tests.
    FORCE_COMPACT_ALL = 1 << 0
  };
  typedef int CompactFlags;

  Status Compact(CompactFlags flags);

  size_t MemRowSetSize() const {
    return memrowset_->memory_footprint();
  }

  // Estimate the total on-disk size of this tablet, in bytes.
  size_t EstimateOnDiskSize() const;

  // Get the total size of all the DMS
  size_t DeltaMemStoresSize() const;

  // Flush only the biggest DMS
  Status FlushBiggestDMS();

  // Return the current number of rowsets in the tablet.
  size_t num_rowsets() const;

  // Attempt to count the total number of rows in the tablet.
  // This is not super-efficient since it must iterate over the
  // memrowset in the current implementation.
  Status CountRows(uint64_t *count) const;


  // Verbosely dump this entire tablet to the logs. This is only
  // really useful when debugging unit tests failures where the tablet
  // has a very small number of rows.
  Status DebugDump(vector<string> *lines = NULL);

  // Return the current schema of the metadata. Note that this returns
  // a copy so should not be used in a tight loop.
  Schema schema() const;

  // Returns a reference to the key projection of the tablet schema.
  // The schema keys are immutable.
  const Schema& key_schema() const { return key_schema_; }

  // Return the MVCC manager for this tablet.
  MvccManager* mvcc_manager() { return &mvcc_; }

  // Return the Lock Manager for this tablet
  LockManager* lock_manager() { return &lock_manager_; }

  // Returns the component lock for this tablet
  percpu_rwlock* component_lock() { return &component_lock_; }

  const metadata::TabletMetadata *metadata() const { return metadata_.get(); }
  metadata::TabletMetadata *metadata() { return metadata_.get(); }

  void SetConsensus(consensus::Consensus* consensus);

  void SetCompactionHooksForTests(const shared_ptr<CompactionFaultHooks> &hooks);
  void SetFlushHooksForTests(const shared_ptr<FlushFaultHooks> &hooks);
  void SetFlushCompactCommonHooksForTests(const shared_ptr<FlushCompactCommonHooks> &hooks);

  int32_t CurrentMrsIdForTests() const { return memrowset_->mrs_id(); }

  int32_t TotalMissedDeltasMutationsForTests() const { return total_missed_deltas_mutations_; }

  const std::string& tablet_id() const { return metadata_->oid(); }

  // Return the metrics for this tablet.
  // May be NULL in unit tests, etc.
  TabletMetrics* metrics() { return metrics_.get(); }

  // Return handle to the metric context of this tablet. For unit tests.
  const MetricContext* GetMetricContextForTests() const { return metric_context_.get(); }

 private:
  friend class Iterator;

  DISALLOW_COPY_AND_ASSIGN(Tablet);

  // Capture a set of iterators which, together, reflect all of the data in the tablet.
  //
  // These iterators are not true snapshot iterators, but they are safe against
  // concurrent modification. They will include all data that was present at the time
  // of creation, and potentially newer data.
  //
  // The returned iterators are not Init()ed
  Status CaptureConsistentIterators(const Schema &projection,
                                    const MvccSnapshot &snap,
                                    const ScanSpec *spec,
                                    vector<shared_ptr<RowwiseIterator> > *iters) const;

  Status PickRowSetsToCompact(RowSetsInCompaction *picked,
                              CompactFlags flags) const;

  Status DoCompactionOrFlush(const Schema& schema,
                             const RowSetsInCompaction &input,
                             int64_t mrs_being_flushed);

  Status FlushMetadata(const RowSetVector& to_remove,
                       const metadata::RowSetMetadataVector& to_add,
                       int64_t mrs_being_flushed);

  // Swap out a set of rowsets, atomically replacing them with the new rowset
  // under the lock.
  void AtomicSwapRowSets(const RowSetVector &old_rowsets,
                         const RowSetVector &new_rowsets,
                         MvccSnapshot *snap_under_lock);

  // Same as the above, but without taking the lock. This should only be used
  // in cases where the lock is already held.
  void AtomicSwapRowSetsUnlocked(const RowSetVector &old_rowsets,
                                 const RowSetVector &new_rowsets,
                                 MvccSnapshot *snap_under_lock);

  // Delete the underlying storage for the input layers in a compaction.
  Status DeleteCompactionInputs(const RowSetsInCompaction &input);

  BloomFilterSizing bloom_sizing() const;

  Schema schema_;
  Schema key_schema_;
  gscoped_ptr<metadata::TabletMetadata> metadata_;
  shared_ptr<MemRowSet> memrowset_;
  shared_ptr<RowSetTree> rowsets_;

  gscoped_ptr<MetricContext> metric_context_;
  gscoped_ptr<TabletMetrics> metrics_;

  consensus::Consensus* consensus_;

  Atomic32 next_mrs_id_;

  MvccManager mvcc_;
  LockManager lock_manager_;

  Atomic32 total_missed_deltas_mutations_;

  gscoped_ptr<CompactionPolicy> compaction_policy_;

  // Lock protecting write access to the components of the tablet (memrowset and rowsets).
  // Shared mode:
  // - Inserters, updaters take this in shared mode during their mutation.
  // - Readers take this in shared mode while capturing their iterators.
  // Exclusive mode:
  // - Flushers take this lock in order to lock out concurrent updates when swapping in
  //   a new memrowset.
  //
  // NOTE: callers should avoid taking this lock for a long time, even in shared mode.
  // This is because the lock has some concept of fairness -- if, while a long reader
  // is active, a writer comes along, then all future short readers will be blocked.
  //
  // TODO: this could probably done more efficiently with a single atomic swap of a list
  // and an RCU-style quiesce phase, but not worth it for now.
  mutable percpu_rwlock component_lock_;

  // Lock protecting the selection of rowsets for compaction.
  // Only one thread may run the compaction selection algorithm at a time
  // so that they don't both try to select the same rowset. Before taking
  // this lock, you should also hold component_lock_ in read mode so that
  // no other thread could perform a swap underneath.
  mutable boost::mutex compact_select_lock_;

  // Lock protecting the schema access
  // Shared mode:
  //  - DoCompactionOrFlush takes the lock to access the schema to ensure that
  //    the newly added rowsets have the latest one.
  //  - schema returns a copy of the current schema and takes the lock
  // Exclusive mode:
  //  - AlterSchema take this lock to prevent reading the half set schema
  mutable percpu_rwlock schema_lock_;

  bool open_;

  // Fault hooks. In production code, these will always be NULL.
  shared_ptr<CompactionFaultHooks> compaction_hooks_;
  shared_ptr<FlushFaultHooks> flush_hooks_;
  shared_ptr<FlushCompactCommonHooks> common_hooks_;
};


// Hooks used in test code to inject faults or other code into interesting
// parts of the compaction code.
class Tablet::CompactionFaultHooks {
 public:
  virtual Status PostSelectIterators() { return Status::OK(); }
  virtual ~CompactionFaultHooks() {}
};

class Tablet::FlushCompactCommonHooks {
 public:
  virtual Status PostTakeMvccSnapshot() { return Status::OK(); }
  virtual Status PostWriteSnapshot() { return Status::OK(); }
  virtual Status PostSwapInDuplicatingRowSet() { return Status::OK(); }
  virtual Status PostReupdateMissedDeltas() { return Status::OK(); }
  virtual Status PostSwapNewRowSet() { return Status::OK(); }
  virtual ~FlushCompactCommonHooks() {}
};

// Hooks used in test code to inject faults or other code into interesting
// parts of the Flush() code.
class Tablet::FlushFaultHooks {
 public:
  virtual Status PostSwapNewMemRowSet() { return Status::OK(); }
  virtual ~FlushFaultHooks() {}
};

class Tablet::Iterator : public RowwiseIterator {
 public:
  virtual ~Iterator() {}

  virtual Status Init(ScanSpec *spec);

  virtual Status PrepareBatch(size_t *nrows);

  virtual bool HasNext() const;

  virtual Status MaterializeBlock(RowBlock *dst);

  virtual Status FinishBatch();

  string ToString() const;

  const Schema &schema() const {
    return projection_;
  }

 private:
  friend class Tablet;

  DISALLOW_COPY_AND_ASSIGN(Iterator);

  Iterator(const Tablet *tablet,
           const Schema &projection,
           const MvccSnapshot &snap);

  const Tablet *tablet_;
  const Schema projection_;
  const MvccSnapshot snap_;
  gscoped_ptr<UnionIterator> iter_;
  RangePredicateEncoder encoder_;
};

} // namespace tablet
} // namespace kudu

#endif
