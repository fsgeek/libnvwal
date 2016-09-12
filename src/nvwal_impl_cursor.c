/*
 * Copyright (c) 2014-2016, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#include "nvwal_impl_cursor.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nvwal_api.h"
#include "nvwal_atomics.h"
#include "nvwal_mds.h"
#include "nvwal_util.h"
#include "nvwal_mds_types.h"

nvwal_error_t nvwal_open_log_cursor(
  struct NvwalContext* wal,
  nvwal_epoch_t begin_epoch,
  nvwal_epoch_t end_epoch,
  struct NvwalLogCursor* out) {
  memset(out, 0, sizeof(*out));
  out->wal_ = wal;
  out->start_epoch_ = begin_epoch;
  out->end_epoch_ = end_epoch;

  nvwal_error_t error_code = cursor_next_initial(out);
  if (error_code) {
    /* Immediately close it in this case. */
    nvwal_close_log_cursor(wal, out);
    return error_code;
  }

  return 0;
}

nvwal_error_t cursor_next_initial(struct NvwalLogCursor* cursor) {
  assert(kNvwalInvalidEpoch == cursor->current_epoch_);
  assert(cursor->cur_segment_disk_fd_ == 0);
  assert(cursor->cur_segment_data_ == 0);
  /* First call to next_epoch after opening the cursor */
  cursor->current_epoch_ = cursor->start_epoch_;
  NVWAL_CHECK_ERROR(cursor_fetch_epoch_metadata(cursor, cursor->start_epoch_));

  if (cursor->fetched_epochs_count_ == 0) {
    return 0;
  }

  struct NvwalCursorEpochMetadata* first_epoch_meta = cursor->fetched_epochs_;
  NVWAL_CHECK_ERROR(cursor_open_segment(cursor, first_epoch_meta->start_dsid_));
  cursor->cur_offset_ = first_epoch_meta->start_offset_;
  if (first_epoch_meta->last_dsid_ != first_epoch_meta->start_dsid_) {
    cursor->cur_len_ = cursor->wal_->config_.segment_size_ - cursor->cur_offset_;
  } else {
    assert(first_epoch_meta->end_offset_ >= first_epoch_meta->start_offset_);
    cursor->cur_len_ = first_epoch_meta->end_offset_ - first_epoch_meta->start_offset_;
  }

  return 0;
}

nvwal_error_t cursor_open_segment(
  struct NvwalLogCursor* cursor,
  nvwal_dsid_t dsid) {
  assert(dsid != kNvwalInvalidDsid);
  struct NvwalContext* wal = cursor->wal_;

  NVWAL_CHECK_ERROR(cursor_close_cur_segment(cursor));

  const nvwal_dsid_t synced_dsid =
    nvwal_atomic_load(&wal->nv_control_block_->fsyncer_progress_.last_synced_dsid_);
  if (dsid >= synced_dsid) {
    /* The segment is on disk! */
    char path[kNvwalMaxPathLength];
    nvwal_construct_disk_segment_path(wal, dsid, path);
    cursor->cur_segment_disk_fd_ = open(path, O_RDONLY, 0);
    assert(cursor->cur_segment_disk_fd_);
    if (cursor->cur_segment_disk_fd_ == -1) {
      assert(errno);
      return errno;
    }

    cursor->cur_segment_data_ = mmap(
      0,
      wal->config_.segment_size_,
      PROT_READ,
      MAP_SHARED,
      cursor->cur_segment_disk_fd_,
      0);
    if (cursor->cur_segment_data_ == MAP_FAILED) {
      assert(errno);
      return errno;
    }
  } else {
    /* The segment is still on NV */
    const uint32_t nv_segment_index = (dsid - 1U) % wal->segment_count_;

    /* TODO safely pin it here */
    struct NvwalLogSegment* nv_segment = wal->segments_ + nv_segment_index;
    assert(nv_segment->dsid_ == dsid);
    cursor->cur_segment_data_ = nv_segment->nv_baseaddr_;
    cursor->cur_segment_from_nv_segment_ = 1;
  }

  cursor->cur_segment_id_ = dsid;
  return 0;
}


nvwal_error_t cursor_close_cur_segment(
  struct NvwalLogCursor* cursor) {
  nvwal_error_t last_seen_error = 0;
  if (cursor->cur_segment_from_nv_segment_) {
    /* The segment is on NV. We just need to unpin it */
    assert(cursor->cur_segment_disk_fd_ == 0);
    cursor->cur_segment_data_ = 0;
    cursor->cur_segment_disk_fd_ = 0;
    /* TODO unpin it here */
    cursor->cur_segment_from_nv_segment_ = 0;
  } else {
    if (cursor->cur_segment_data_ && cursor->cur_segment_data_ != MAP_FAILED) {
      if (munmap(
          cursor->cur_segment_data_,
          cursor->wal_->config_.segment_size_) == -1) {
        last_seen_error = nvwal_stock_error_code(last_seen_error, errno);
      }
    }
    cursor->cur_segment_data_ = 0;

    if (cursor->cur_segment_disk_fd_ && cursor->cur_segment_disk_fd_ != -1) {
      if (close(cursor->cur_segment_disk_fd_) == -1) {
        last_seen_error = nvwal_stock_error_code(last_seen_error, errno);
      }
    }
    cursor->cur_segment_disk_fd_ = 0;
  }

  cursor->cur_segment_id_ = kNvwalInvalidDsid;
  return last_seen_error;
}

nvwal_error_t cursor_fetch_epoch_metadata(
  struct NvwalLogCursor* cursor,
  nvwal_epoch_t from_epoch) {
  assert(from_epoch != kNvwalInvalidEpoch);

  nvwal_epoch_t to_epoch = from_epoch + kNvwalCursorEpochPrefetches;
  if (to_epoch < from_epoch) {
    /* Wrap-around happened. We skip zero (kInvalid) */
    to_epoch += 1;
  }
  if (nvwal_is_epoch_after(to_epoch, cursor->wal_->durable_epoch_)) {
    to_epoch = cursor->wal_->durable_epoch_;
  }

  cursor->fetched_epochs_from_ = from_epoch;
  cursor->fetched_epochs_count_ = 0;
  struct MdsEpochIterator mds_iterator;
  NVWAL_CHECK_ERROR(mds_epoch_iterator_init(
    cursor->wal_,
    from_epoch,
    to_epoch,
    &mds_iterator));
  nvwal_epoch_t cur_epoch = from_epoch;  /* Mostly for sanity check */
  for (int i = 0; i < kNvwalCursorEpochPrefetches; ++i) {
    assert(!mds_epoch_iterator_done(&mds_iterator));

    assert(mds_iterator.epoch_metadata_->epoch_id_ == cur_epoch);
    cursor->fetched_epochs_[i].start_dsid_ = mds_iterator.epoch_metadata_->from_seg_id_;
    cursor->fetched_epochs_[i].last_dsid_ = mds_iterator.epoch_metadata_->to_seg_id_;
    cursor->fetched_epochs_[i].start_offset_ = mds_iterator.epoch_metadata_->from_offset_;
    cursor->fetched_epochs_[i].end_offset_ = mds_iterator.epoch_metadata_->to_off_;

    cur_epoch = nvwal_increment_epoch(cur_epoch);
    ++cursor->fetched_epochs_count_;
    mds_epoch_iterator_next(&mds_iterator);

    if (cur_epoch == to_epoch) {
      assert(mds_epoch_iterator_done(&mds_iterator));
      break;
    }
  }
  assert(cur_epoch == to_epoch);
  assert(cursor->fetched_epochs_count_ <= kNvwalCursorEpochPrefetches);
  NVWAL_CHECK_ERROR(mds_epoch_iterator_destroy(&mds_iterator));
  return 0;
}


nvwal_error_t nvwal_close_log_cursor(
  struct NvwalContext* wal,
  struct NvwalLogCursor* cursor) {
  nvwal_error_t last_seen_error = cursor_close_cur_segment(cursor);
  memset(cursor, 0, sizeof(*cursor));
  return last_seen_error;
}

nvwal_error_t nvwal_cursor_next(
  struct NvwalContext* wal,
  struct NvwalLogCursor* cursor) {
  /*
   * Cases in order of likelihood.
   * 1) Has read a small epoch. Moving on to next epoch within the segment.
   *   1a) Fetched epochs contain next epoch.
   *   1b) Need to fetch next epochs.
   * 2) The epoch has remaining data. Moving on to next segment.
   */
  cursor_close_cur_segment(cursor);

  /* TODO mmm, probably we should have cur_index
  cursor->fetched_epochs_from_;

  struct NvwalCursorEpochMetadata* meta = cursor->fetched_epochs_ + cursor->;
  cursor->current_epoch_ = cursor->start_epoch_;
  NVWAL_CHECK_ERROR(cursor_fetch_epoch_metadata(cursor, cursor->start_epoch_));

  if (cursor->fetched_epochs_count_ == 0) {
    return 0;
  }

  NVWAL_CHECK_ERROR(cursor_open_segment(cursor, first_epoch_meta->start_dsid_));
  cursor->cur_offset_ = first_epoch_meta->start_offset_;
  if (first_epoch_meta->last_dsid_ != first_epoch_meta->start_dsid_) {
    cursor->cur_len_ = cursor->wal_->config_.segment_size_ - cursor->cur_offset_;
  } else {
    assert(first_epoch_meta->end_offset_ >= first_epoch_meta->start_offset_);
    cursor->cur_len_ = first_epoch_meta->end_offset_ - first_epoch_meta->start_offset_;
  }
  */

  return 0;
}
