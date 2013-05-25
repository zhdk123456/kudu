// Copyright (c) 2013, Cloudera, inc.
// All rights reserved.
#ifndef KUDU_TABLET_DELTATRACKER_H
#define KUDU_TABLET_DELTATRACKER_H

#include <boost/noncopyable.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <gtest/gtest.h>

#include "common/iterator.h"
#include "tablet/cfile_set.h"
#include "tablet/delta_store.h"
#include "util/status.h"

namespace kudu {
namespace tablet {

using std::tr1::shared_ptr;

class DeltaMemStore;
class DeltaFileReader;

// The DeltaTracker is the part of a DiskRowSet which is responsible for
// tracking modifications against the base data. It consists of a set of
// DeltaStores which each contain a set of mutations against the base data.
// These DeltaStores may be on disk (DeltaFileReader) or in-memory (DeltaMemStore).
//
// This class is also responsible for flushing the in-memory deltas to disk.
class DeltaTracker : public boost::noncopyable {
public:
  DeltaTracker(Env *env,
               const Schema &schema,
               const string &dir);

  ColumnwiseIterator *WrapIterator(const shared_ptr<ColumnwiseIterator> &base,
                                   const MvccSnapshot &mvcc_snap) const;

  // TODO: this shouldn't need to return a shared_ptr, but there is some messiness
  // where this has bled around.
  shared_ptr<DeltaIterator> NewDeltaIterator(const Schema &schema,
                                                      const MvccSnapshot &snap) const;


  Status Open();
  Status Flush();

  // Update the given row in the database.
  // Copies the data, as well as any referenced
  // values into a local arena.
  void Update(txid_t txid, rowid_t row_idx, const RowChangeList &update);

private:
  friend class DiskRowSet;

  FRIEND_TEST(TestRowSet, TestRowSetUpdate);
  FRIEND_TEST(TestRowSet, TestDMSFlush);

  Status OpenDeltaFileReaders();
  Status FlushDMS(const DeltaMemStore &dms,
                  gscoped_ptr<DeltaFileReader> *dfr);
  void CollectTrackers(vector<shared_ptr<DeltaStore> > *deltas) const;

  Env *env_;
  const Schema schema_;
  string dir_;

  bool open_;

  // The suffix to use on the next flushed deltafile. Delta files are named
  // delta_<N> to designate the order in which they were flushed.
  uint32_t next_deltafile_idx_;

  // The current delta memrowset into which updates should be written.
  shared_ptr<DeltaMemStore> dms_;
  vector<shared_ptr<DeltaStore> > delta_trackers_;


  // read-write lock protecting dms_ and delta_trackers_.
  // - Readers and mutators take this lock in shared mode.
  // - Flushers take this lock in exclusive mode before they modify the
  //   structure of the rowset.
  //
  // TODO(perf): convert this to a reader-biased lock to avoid any cacheline
  // contention between threads.
  mutable boost::shared_mutex component_lock_;

};


////////////////////////////////////////////////////////////
// Delta-applying iterators
////////////////////////////////////////////////////////////

// A DeltaApplier takes in a base ColumnwiseIterator along with a a
// DeltaIterator. It is responsible for applying the updates coming
// from the delta iterator to the results of the base iterator.
class DeltaApplier : public ColumnwiseIterator, boost::noncopyable {
public:
  virtual Status Init(ScanSpec *spec) {
    RETURN_NOT_OK(base_iter_->Init(spec));
    RETURN_NOT_OK(delta_iter_->Init());
    RETURN_NOT_OK(delta_iter_->SeekToOrdinal(0));
    return Status::OK();
  }

  Status PrepareBatch(size_t *nrows);

  Status FinishBatch();

  bool HasNext() const {
    return base_iter_->HasNext();
  }

  string ToString() const {
    string s;
    s.append("DeltaApplier(");
    s.append(base_iter_->ToString());
    s.append(" + ");
    s.append(delta_iter_->ToString());
    s.append(")");
    return s;
  }

  const Schema &schema() const {
    return base_iter_->schema();
  }

  Status MaterializeColumn(size_t col_idx, ColumnBlock *dst);
private:
  friend class DeltaTracker;

  // Construct. The base_iter and delta_iter should not be Initted.
  DeltaApplier(const shared_ptr<ColumnwiseIterator> &base_iter,
               const shared_ptr<DeltaIterator> delta_iter) :
    base_iter_(base_iter),
    delta_iter_(delta_iter)
  {
  }

  shared_ptr<ColumnwiseIterator> base_iter_;
  shared_ptr<DeltaIterator> delta_iter_;
};


inline Status DeltaApplier::PrepareBatch(size_t *nrows) {
  RETURN_NOT_OK(base_iter_->PrepareBatch(nrows));
  if (*nrows == 0) {
    return Status::NotFound("no more rows left");
  }

  RETURN_NOT_OK(delta_iter_->PrepareBatch(*nrows));
  return Status::OK();
}

inline Status DeltaApplier::FinishBatch() {
  return base_iter_->FinishBatch();
}

inline Status DeltaApplier::MaterializeColumn(size_t col_idx, ColumnBlock *dst) {
  // Copy the base data.
  RETURN_NOT_OK(base_iter_->MaterializeColumn(col_idx, dst));

  // Apply all the updates for this column.
  RETURN_NOT_OK(delta_iter_->ApplyUpdates(col_idx, dst));
  return Status::OK();
}

} // namespace tablet
} // namespace kudu

#endif
