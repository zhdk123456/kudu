// Copyright (c) 2014 Cloudera, Inc.
// Confidential Cloudera Information: Covered by NDA.
#include "kudu/tserver/remote_bootstrap_client.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus_meta.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/fs/block_id.h"
#include "kudu/fs/block_manager.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/transfer.h"
#include "kudu/tablet/tablet.pb.h"
#include "kudu/tablet/tablet_bootstrap.h"
#include "kudu/tablet/tablet_peer.h"
#include "kudu/tserver/remote_bootstrap.pb.h"
#include "kudu/tserver/remote_bootstrap.proxy.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/util/crc.h"
#include "kudu/util/env.h"
#include "kudu/util/net/net_util.h"

DEFINE_int32(remote_bootstrap_begin_session_timeout_ms, 10000,
             "Tablet server RPC client timeout for BeginRemoteBootstrapSession calls.");

// RETURN_NOT_OK_PREPEND() with a remote-error unwinding step.
#define RETURN_NOT_OK_UNWIND_PREPEND(status, controller, msg) \
  RETURN_NOT_OK_PREPEND(UnwindRemoteError(status, controller), msg)

namespace kudu {
namespace tserver {

using consensus::ConsensusMetadata;
using consensus::ConsensusStatePB;
using consensus::OpId;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using fs::WritableBlock;
using rpc::Messenger;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using strings::Substitute;
using tablet::ColumnDataPB;
using tablet::DeltaDataPB;
using tablet::RowSetDataPB;
using tablet::TabletMetadata;
using tablet::TabletStatusListener;
using tablet::TabletSuperBlockPB;

RemoteBootstrapClient::RemoteBootstrapClient(FsManager* fs_manager,
                                             const shared_ptr<Messenger>& messenger,
                                             const string& client_permanent_uuid)
  : fs_manager_(fs_manager),
    messenger_(messenger),
    permanent_uuid_(client_permanent_uuid),
    state_(kNoSession),
    status_listener_(NULL),
    session_idle_timeout_millis_(0) {
}

Status RemoteBootstrapClient::RunRemoteBootstrap(TabletMetadata* meta,
                                                 const ConsensusStatePB& cstate,
                                                 TabletStatusListener* status_listener) {
  DCHECK(meta != NULL);

  CHECK_EQ(tablet::REMOTE_BOOTSTRAP_COPYING, meta->remote_bootstrap_state());
  const string& tablet_id = meta->tablet_id();

  // Download all the files (serially, for now, but in parallel in the future).
  RETURN_NOT_OK(BeginRemoteBootstrapSession(tablet_id, cstate, status_listener));
  RETURN_NOT_OK(DownloadWALs());
  RETURN_NOT_OK(DownloadBlocks());
  RETURN_NOT_OK(WriteConsensusMetadata());

  // Replace tablet metadata superblock. This will set the tablet metadata state
  // to REMOTE_BOOTSTRAP_DONE, since we checked above that the response
  // superblock is in a valid state to bootstrap from.
  LOG(INFO) << "Tablet " << tablet_id_ << " remote bootstrap complete. Replacing superblock.";
  UpdateStatusMessage("Replacing tablet superblock");
  RETURN_NOT_OK(meta->ReplaceSuperBlock(*new_superblock_));

  // Note: Ending the remote bootstrap session releases anchors on the remote.
  RETURN_NOT_OK(EndRemoteBootstrapSession());

  return Status::OK();
}

Status RemoteBootstrapClient::ExtractLeaderFromConfig(const ConsensusStatePB& cstate,
                                                      RaftPeerPB* leader) {
  BOOST_FOREACH(const RaftPeerPB& peer, cstate.config().peers()) {
    if (!cstate.has_leader_uuid() || cstate.leader_uuid().empty()) break;
    if (peer.permanent_uuid() == cstate.leader_uuid()) {
      leader->CopyFrom(peer);
      return Status::OK();
    }
  }
  return Status::NotFound("No leader found in config");
}

// Decode the remote error into a human-readable Status object.
Status RemoteBootstrapClient::ExtractRemoteError(const rpc::ErrorStatusPB& remote_error) {
  if (PREDICT_TRUE(remote_error.HasExtension(RemoteBootstrapErrorPB::remote_bootstrap_error_ext))) {
    const RemoteBootstrapErrorPB& error =
        remote_error.GetExtension(RemoteBootstrapErrorPB::remote_bootstrap_error_ext);
    return StatusFromPB(error.status()).CloneAndPrepend("Received error code " +
              RemoteBootstrapErrorPB::Code_Name(error.code()) + " from remote service");
  } else {
    return Status::InvalidArgument("Unable to decode remote bootstrap RPC error message",
                                   remote_error.ShortDebugString());
  }
}

// Enhance a RemoteError Status message with additional details from the remote.
Status RemoteBootstrapClient::UnwindRemoteError(const Status& status,
                                                const rpc::RpcController& controller) {
  if (!status.IsRemoteError()) {
    return status;
  }
  Status extension_status = ExtractRemoteError(*controller.error_response());
  return status.CloneAndAppend(extension_status.ToString());
}

void RemoteBootstrapClient::UpdateStatusMessage(const string& message) {
  if (status_listener_ != NULL) {
    status_listener_->StatusMessage("RemoteBootstrap: " + message);
  }
}

Status RemoteBootstrapClient::BeginRemoteBootstrapSession(const std::string& tablet_id,
                                                          const ConsensusStatePB& cstate,
                                                          TabletStatusListener* status_listener) {
  CHECK_EQ(kNoSession, state_);

  tablet_id_ = tablet_id;
  status_listener_ = status_listener;

  UpdateStatusMessage("Initializing remote bootstrap");

  // Find the consensus leader's address.
  // TODO: Support looking up consensus configuration info from Master and also redirecting
  // from follower to consensus leader in the future.
  RaftPeerPB leader;
  RETURN_NOT_OK_PREPEND(ExtractLeaderFromConfig(cstate, &leader),
                        "Cannot find leader tablet in config to remotely bootstrap from: " +
                        cstate.ShortDebugString());
  if (!leader.has_last_known_addr()) {
    return Status::InvalidArgument("Unknown address for config leader", leader.ShortDebugString());
  }
  HostPort host_port;
  RETURN_NOT_OK(HostPortFromPB(leader.last_known_addr(), &host_port));
  Sockaddr addr;
  RETURN_NOT_OK(SockaddrFromHostPort(host_port, &addr));
  LOG(INFO) << "Beginning remote bootstrap session on tablet " << tablet_id
            << " from leader " << host_port.ToString();

  UpdateStatusMessage("Beginning remote bootstrap session with leader " + host_port.ToString());

  // Set up an RPC proxy for the RemoteBootstrapService.
  proxy_.reset(new RemoteBootstrapServiceProxy(messenger_, addr));

  BeginRemoteBootstrapSessionRequestPB req;
  req.set_requestor_uuid(permanent_uuid_);
  req.set_tablet_id(tablet_id);

  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(
      FLAGS_remote_bootstrap_begin_session_timeout_ms));

  // Begin the remote bootstrap session.
  BeginRemoteBootstrapSessionResponsePB resp;
  RETURN_NOT_OK_UNWIND_PREPEND(proxy_->BeginRemoteBootstrapSession(req, &resp, &controller),
                               controller,
                               "Unable to begin remote bootstrap session");

  // TODO: Support retrying based on updated info from Master or consensus configuration.
  if (resp.superblock().remote_bootstrap_state() != tablet::REMOTE_BOOTSTRAP_DONE) {
    Status s = Status::IllegalState("Leader of config (" + cstate.ShortDebugString() + ")" +
                                    " is currently remotely bootstrapping itself!",
                                    resp.superblock().ShortDebugString());
    LOG(WARNING) << s.ToString();
    return s;
  }

  session_id_ = resp.session_id();
  session_idle_timeout_millis_ = resp.session_idle_timeout_millis();
  superblock_.reset(resp.release_superblock());
  wal_seqnos_.assign(resp.wal_segment_seqnos().begin(), resp.wal_segment_seqnos().end());
  committed_cstate_.reset(new ConsensusStatePB(resp.initial_committed_cstate()));

  state_ = kSessionStarted;

  return Status::OK();
}

Status RemoteBootstrapClient::EndRemoteBootstrapSession() {
  CHECK_EQ(kSessionStarted, state_);

  UpdateStatusMessage("Ending remote bootstrap session");

  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(
        FLAGS_remote_bootstrap_begin_session_timeout_ms));

  EndRemoteBootstrapSessionRequestPB req;
  req.set_session_id(session_id_);
  req.set_is_success(true);
  EndRemoteBootstrapSessionResponsePB resp;
  RETURN_NOT_OK_UNWIND_PREPEND(proxy_->EndRemoteBootstrapSession(req, &resp, &controller),
                               controller,
                               "Failure ending remote bootstrap session");

  UpdateStatusMessage("Remote bootstrap complete");

  return Status::OK();
}

Status RemoteBootstrapClient::DownloadWALs() {
  CHECK_EQ(kSessionStarted, state_);

  // Delete and recreate WAL dir if it already exists, to ensure stray files are
  // not kept from previous bootstraps and runs.
  string path = fs_manager_->GetTabletWalDir(tablet_id_);
  if (fs_manager_->env()->FileExists(path)) {
    RETURN_NOT_OK(fs_manager_->env()->DeleteRecursively(path));
  }
  RETURN_NOT_OK(fs_manager_->env()->CreateDir(path));
  RETURN_NOT_OK(fs_manager_->env()->SyncDir(DirName(path))); // fsync() parent dir.

  // Download the WAL segments.
  int num_segments = wal_seqnos_.size();
  LOG(INFO) << "Starting download of " << num_segments << " WAL segments...";
  uint64_t counter = 0;
  BOOST_FOREACH(uint64_t seg_seqno, wal_seqnos_) {
    UpdateStatusMessage(Substitute("Downloading WAL segment with seq. number $0 ($1/$2)",
                                   seg_seqno, counter + 1, num_segments));
    RETURN_NOT_OK(DownloadWAL(seg_seqno));
    ++counter;
  }
  return Status::OK();
}

Status RemoteBootstrapClient::DownloadBlocks() {
  CHECK_EQ(kSessionStarted, state_);

  // Count up the total number of blocks to download.
  int num_blocks = 0;
  BOOST_FOREACH(const RowSetDataPB& rowset, superblock_->rowsets()) {
    num_blocks += rowset.columns_size();
    num_blocks += rowset.redo_deltas_size();
    num_blocks += rowset.undo_deltas_size();
    if (rowset.has_bloom_block()) {
      num_blocks++;
    }
    if (rowset.has_adhoc_index_block()) {
      num_blocks++;
    }
  }

  // Download each block, writing the new block IDs into the new superblock
  // as each block downloads.
  gscoped_ptr<TabletSuperBlockPB> new_sb(new TabletSuperBlockPB());
  new_sb->CopyFrom(*superblock_);
  int block_count = 0;
  LOG(INFO) << "Starting download of " << num_blocks << " data blocks...";
  BOOST_FOREACH(RowSetDataPB& rowset, *new_sb->mutable_rowsets()) {
    BOOST_FOREACH(ColumnDataPB& col, *rowset.mutable_columns()) {
      RETURN_NOT_OK(DownloadAndRewriteBlock(col.mutable_block(),
                                            &block_count, num_blocks));
    }
    BOOST_FOREACH(DeltaDataPB& redo, *rowset.mutable_redo_deltas()) {
      RETURN_NOT_OK(DownloadAndRewriteBlock(redo.mutable_block(),
                                            &block_count, num_blocks));
    }
    BOOST_FOREACH(DeltaDataPB& undo, *rowset.mutable_undo_deltas()) {
      RETURN_NOT_OK(DownloadAndRewriteBlock(undo.mutable_block(),
                                            &block_count, num_blocks));
    }
    if (rowset.has_bloom_block()) {
      RETURN_NOT_OK(DownloadAndRewriteBlock(rowset.mutable_bloom_block(),
                                            &block_count, num_blocks));
    }
    if (rowset.has_adhoc_index_block()) {
      RETURN_NOT_OK(DownloadAndRewriteBlock(rowset.mutable_adhoc_index_block(),
                                            &block_count, num_blocks));
    }
  }

  // The orphaned physical block ids at the remote have no meaning to us.
  new_sb->clear_orphaned_blocks();

  new_superblock_.swap(new_sb);
  return Status::OK();
}

Status RemoteBootstrapClient::DownloadWAL(uint64_t wal_segment_seqno) {
  VLOG(1) << "Downloading WAL segment with seqno " << wal_segment_seqno;
  DataIdPB data_id;
  data_id.set_type(DataIdPB::LOG_SEGMENT);
  data_id.set_wal_segment_seqno(wal_segment_seqno);
  string dest_path = fs_manager_->GetWalSegmentFileName(tablet_id_, wal_segment_seqno);

  WritableFileOptions opts;
  opts.sync_on_close = true;
  gscoped_ptr<WritableFile> writer;
  RETURN_NOT_OK_PREPEND(fs_manager_->env()->NewWritableFile(opts, dest_path, &writer),
                        "Unable to open file for writing");
  RETURN_NOT_OK_PREPEND(DownloadFile(data_id, writer.get()),
                        Substitute("Unable to download WAL segment with seq. number $0",
                                   wal_segment_seqno));
  return Status::OK();
}

Status RemoteBootstrapClient::WriteConsensusMetadata() {
  gscoped_ptr<ConsensusMetadata> cmeta;
  return ConsensusMetadata::Create(fs_manager_, tablet_id_, fs_manager_->uuid(),
                                   committed_cstate_->config(),
                                   committed_cstate_->current_term(),
                                   &cmeta);
}

Status RemoteBootstrapClient::DownloadAndRewriteBlock(BlockIdPB* block_id,
                                                      int* block_count, int num_blocks) {
  BlockId old_block_id(BlockId::FromPB(*block_id));
  UpdateStatusMessage(Substitute("Downloading block $0 ($1/$2)",
                                 old_block_id.ToString(), *block_count,
                                 num_blocks));
  BlockId new_block_id;
  RETURN_NOT_OK_PREPEND(DownloadBlock(old_block_id, &new_block_id),
      "Unable to download block with id " + old_block_id.ToString());

  new_block_id.CopyToPB(block_id);
  (*block_count)++;
  return Status::OK();
}

Status RemoteBootstrapClient::DownloadBlock(const BlockId& old_block_id,
                                            BlockId* new_block_id) {
  VLOG(1) << "Downloading block with block_id " << old_block_id.ToString();

  gscoped_ptr<WritableBlock> block;
  RETURN_NOT_OK_PREPEND(fs_manager_->CreateNewBlock(&block),
                        "Unable to create new block");

  DataIdPB data_id;
  data_id.set_type(DataIdPB::BLOCK);
  old_block_id.CopyToPB(data_id.mutable_block_id());
  RETURN_NOT_OK_PREPEND(DownloadFile(data_id, block.get()),
                        Substitute("Unable to download block $0",
                                   old_block_id.ToString()));

  *new_block_id = block->id();
  return Status::OK();
}

template<class Appendable>
Status RemoteBootstrapClient::DownloadFile(const DataIdPB& data_id,
                                           Appendable* appendable) {
  uint64_t offset = 0;
  int32_t max_length = FLAGS_rpc_max_message_size - 1024; // Leave 1K for message headers.

  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(session_idle_timeout_millis_));
  FetchDataRequestPB req;

  bool done = false;
  while (!done) {
    controller.Reset();
    req.set_session_id(session_id_);
    req.mutable_data_id()->CopyFrom(data_id);
    req.set_offset(offset);
    req.set_max_length(max_length);

    FetchDataResponsePB resp;
    RETURN_NOT_OK_UNWIND_PREPEND(proxy_->FetchData(req, &resp, &controller),
                                controller,
                                "Unable to fetch data from remote");

    // Sanity-check for corruption.
    RETURN_NOT_OK_PREPEND(VerifyData(offset, resp.chunk()),
                          Substitute("Error validating data item $0", data_id.ShortDebugString()));

    // Write the data.
    RETURN_NOT_OK(appendable->Append(resp.chunk().data()));

    if (offset + resp.chunk().data().size() == resp.chunk().total_data_length()) {
      done = true;
    }
    offset += resp.chunk().data().size();
  }

  return Status::OK();
}

Status RemoteBootstrapClient::VerifyData(uint64_t offset, const DataChunkPB& chunk) {
  // Verify the offset is what we expected.
  if (offset != chunk.offset()) {
    return Status::InvalidArgument("Offset did not match what was asked for",
        Substitute("$0 vs $1", offset, chunk.offset()));
  }

  // Verify the checksum.
  uint32_t crc32 = crc::Crc32c(chunk.data().data(), chunk.data().length());
  if (PREDICT_FALSE(crc32 != chunk.crc32())) {
    return Status::Corruption(
        Substitute("CRC32 does not match at offset $0 size $1: $2 vs $3",
          offset, chunk.data().size(), crc32, chunk.crc32()));
  }
  return Status::OK();
}

} // namespace tserver
} // namespace kudu