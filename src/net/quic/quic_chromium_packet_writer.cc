// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_chromium_packet_writer.h"

#include <string>
#include <utility>

#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/sparse_histogram.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/quic/quic_chromium_client_session.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace net {

namespace {


enum NotReusableReason {
  NOT_REUSABLE_NULLPTR = 0,
  NOT_REUSABLE_TOO_SMALL = 1,
  NOT_REUSABLE_REF_COUNT = 2,
  NUM_NOT_REUSABLE_REASONS = 3,
};

const int kMaxRetries = 12;  // 2^12 = 4 seconds, which should be a LOT.

void RecordNotReusableReason(NotReusableReason reason) {
  UMA_HISTOGRAM_ENUMERATION("Net.QuicSession.WritePacketNotReusable", reason,
                            NUM_NOT_REUSABLE_REASONS);
}

void RecordRetryCount(int count) {
  UMA_HISTOGRAM_EXACT_LINEAR("Net.QuicSession.RetryAfterWriteErrorCount2",
                             count, kMaxRetries + 1);
}

const net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("quic_chromium_packet_writer", R"(
        semantics {
          sender: "QUIC Packet Writer"
          description:
            "A QUIC packet is written to the wire based on a request from "
            "a QUIC stream."
          trigger:
            "A request from QUIC stream."
          data: "Any data sent by the stream."
          destination: OTHER
          destination_other: "Any destination choosen by the stream."
        }
        policy {
          cookies_allowed: NO
          setting: "This feature cannot be disabled in settings."
          policy_exception_justification:
            "Essential for network access."
        }
        comments:
          "All requests that are received by QUIC streams have network traffic "
          "annotation, but the annotation is not passed to the writer function "
          "due to technial overheads. Please see QuicChromiumClientSession and "
          "QuicChromiumClientStream classes for references."
    )");

EcnCodePoint QuicheEcnToChromiumEcn(const quic::QuicEcnCodepoint codepoint) {
  switch (codepoint) {
    case quic::ECN_NOT_ECT:
      return ECN_NOT_ECT;
    case quic::ECN_ECT1:
      return ECN_ECT1;
    case quic::ECN_ECT0:
      return ECN_ECT0;
    case quic::ECN_CE:
      return ECN_CE;
    default:
      break;
  }
  NOTREACHED();
}

}  // namespace

namespace {
// The outermost ScopedFlushDeferral on this thread, if any. QUIC sessions,
// readers and writers all live on the network thread.
thread_local QuicChromiumPacketWriter::ScopedFlushDeferral*
    g_current_flush_deferral = nullptr;
}  // namespace

QuicChromiumPacketWriter::ScopedFlushDeferral::ScopedFlushDeferral()
    : outermost_(g_current_flush_deferral == nullptr) {
  if (outermost_) {
    g_current_flush_deferral = this;
  }
}

QuicChromiumPacketWriter::ScopedFlushDeferral::~ScopedFlushDeferral() {
  if (!outermost_) {
    return;
  }
  g_current_flush_deferral = nullptr;
  // FlushDeferred() may run delegate callbacks; take the list first.
  std::vector<base::WeakPtr<QuicChromiumPacketWriter>> deferred;
  deferred.swap(deferred_);
  for (const auto& writer : deferred) {
    if (writer) {
      writer->FlushDeferred();
    }
  }
}

void QuicChromiumPacketWriter::FlushDeferred() {
  flush_deferred_ = false;
  if (!socket_ || !HasUnsentBatch() || IsWriteBlocked()) {
    return;
  }
  quic::WriteResult result = SendBatch();
  if (result.status == quic::WRITE_STATUS_ERROR && delegate_ != nullptr) {
    delegate_->OnWriteError(result.error_code);
  }
}

QuicChromiumPacketWriter::ReusableIOBuffer::ReusableIOBuffer(size_t capacity)
    : IOBufferWithSize(capacity), capacity_(capacity) {}

QuicChromiumPacketWriter::ReusableIOBuffer::~ReusableIOBuffer() = default;

void QuicChromiumPacketWriter::ReusableIOBuffer::Set(const char* buffer,
                                                     size_t buf_len) {
  CHECK_LE(buf_len, capacity_);
  CHECK(HasOneRef());
  size_ = buf_len;
  span().copy_prefix_from(
      base::as_bytes(UNSAFE_TODO(base::span(buffer, buf_len))));
}

QuicChromiumPacketWriter::QuicChromiumPacketWriter(
    DatagramClientSocket* socket,
    base::SequencedTaskRunner* task_runner)
    : socket_(socket),
      packet_(base::MakeRefCounted<ReusableIOBuffer>(
          quic::kMaxOutgoingPacketSize)) {
  retry_timer_.SetTaskRunner(task_runner);
  write_callback_ = base::BindRepeating(
      &QuicChromiumPacketWriter::OnWriteComplete, weak_factory_.GetWeakPtr());
}

QuicChromiumPacketWriter::~QuicChromiumPacketWriter() {
  UMA_HISTOGRAM_ENUMERATION(
      "Net.QuicSession.OutgoingEcn",
      static_cast<EcnPermutations>(outgoing_ecn_history_));
}

void QuicChromiumPacketWriter::set_force_write_blocked(
    bool force_write_blocked) {
  force_write_blocked_ = force_write_blocked;
  if (!IsWriteBlocked() && delegate_ != nullptr)
    delegate_->OnWriteUnblocked();
}

void QuicChromiumPacketWriter::SetPacket(const char* buffer, size_t buf_len) {
  if (!packet_) [[unlikely]] {
    packet_ = base::MakeRefCounted<ReusableIOBuffer>(
        std::max(buf_len, static_cast<size_t>(quic::kMaxOutgoingPacketSize)));
    RecordNotReusableReason(NOT_REUSABLE_NULLPTR);
  }
  if (packet_->capacity() < buf_len) [[unlikely]] {
    packet_ = base::MakeRefCounted<ReusableIOBuffer>(buf_len);
    RecordNotReusableReason(NOT_REUSABLE_TOO_SMALL);
  }
  if (!packet_->HasOneRef()) [[unlikely]] {
    packet_ = base::MakeRefCounted<ReusableIOBuffer>(
        std::max(buf_len, static_cast<size_t>(quic::kMaxOutgoingPacketSize)));
    RecordNotReusableReason(NOT_REUSABLE_REF_COUNT);
  }
  packet_->Set(buffer, buf_len);
}

quic::WriteResult QuicChromiumPacketWriter::WritePacket(
    const char* buffer,
    size_t buf_len,
    const quiche::QuicheIpAddress& self_address,
    const quic::QuicSocketAddress& peer_address,
    quic::PerPacketOptions* /*options*/,
    const quic::QuicPacketWriterParams& params) {
  CHECK(!IsWriteBlocked());
  EcnCodePoint new_ecn = QuicheEcnToChromiumEcn(params.ecn_codepoint);
  outgoing_ecn_history_ |= (1 << static_cast<uint8_t>(new_ecn));
  if (IsBatchMode()) {
    // A batch shares one TOS setting: send what is buffered before changing
    // it, and send oversized packets on their own.
    if (HasUnsentBatch() &&
        (new_ecn != outgoing_ecn_ || buf_len > quic::kMaxOutgoingPacketSize)) {
      quic::WriteResult flushed = SendBatch();
      if (flushed.status != quic::WRITE_STATUS_OK) {
        // This packet was not buffered; the connection keeps it.
        if (quic::IsWriteBlockedStatus(flushed.status)) {
          return quic::WriteResult(quic::WRITE_STATUS_BLOCKED, ERR_IO_PENDING);
        }
        return flushed;
      }
    }
    if (new_ecn != outgoing_ecn_) {
      socket_->SetTos(DSCP_NO_CHANGE, new_ecn);
      outgoing_ecn_ = new_ecn;
    }
    if (buf_len <= quic::kMaxOutgoingPacketSize) {
      if (batch_storage_.empty()) {
        batch_storage_.reserve(kMaxBatchedPackets *
                               quic::kMaxOutgoingPacketSize);
      }
      UNSAFE_TODO(batch_storage_.insert(batch_storage_.end(), buffer,
                                        buffer + buf_len));
      batch_lengths_.push_back(buf_len);
      if (batch_lengths_.size() >= kMaxBatchedPackets) {
        quic::WriteResult flushed = SendBatch();
        if (quic::IsWriteBlockedStatus(flushed.status)) {
          return quic::WriteResult(quic::WRITE_STATUS_BLOCKED_DATA_BUFFERED,
                                   ERR_IO_PENDING);
        }
        return flushed;
      }
      // QuicConnection flushes at the end of each send burst; the posted
      // task only guarantees that nothing stays buffered.
      if (!flush_task_posted_) {
        flush_task_posted_ = true;
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&QuicChromiumPacketWriter::FlushFromTask,
                                      weak_factory_.GetWeakPtr()));
      }
      return quic::WriteResult(quic::WRITE_STATUS_OK, 0);
    }
  }
  SetPacket(buffer, buf_len);
  if (new_ecn != outgoing_ecn_) {
    socket_->SetTos(DSCP_NO_CHANGE, new_ecn);
    outgoing_ecn_ = new_ecn;
  }
  return WritePacketToSocketImpl();
}

void QuicChromiumPacketWriter::ClearBatch() {
  batch_storage_.clear();
  batch_lengths_.clear();
  batch_next_ = 0;
}

quic::WriteResult QuicChromiumPacketWriter::SendBatch() {
  CHECK(socket_);
  std::vector<base::span<const uint8_t>> packets;
  packets.reserve(batch_lengths_.size() - batch_next_);
  size_t offset = 0;
  for (size_t i = 0; i < batch_lengths_.size(); ++i) {
    if (i >= batch_next_) {
      packets.push_back(base::as_bytes(base::span(batch_storage_))
                            .subspan(offset, batch_lengths_[i]));
    }
    offset += batch_lengths_[i];
  }
  const int rv = socket_->WriteBatch(packets);
  if (rv > 0) {
    batch_next_ += static_cast<size_t>(rv);
  }
  return WriteBatchRemainderSingly();
}

quic::WriteResult QuicChromiumPacketWriter::WriteBatchRemainderSingly() {
  while (HasUnsentBatch()) {
    size_t offset = 0;
    for (size_t i = 0; i < batch_next_; ++i) {
      offset += batch_lengths_[i];
    }
    const size_t length = batch_lengths_[batch_next_];
    ++batch_next_;
    SetPacket(UNSAFE_TODO(batch_storage_.data() + offset), length);
    quic::WriteResult result = WritePacketToSocketImpl();
    if (result.status == quic::WRITE_STATUS_OK) {
      continue;
    }
    if (quic::IsWriteBlockedStatus(result.status)) {
      // OnWriteComplete() resumes with the rest of the batch.
      return quic::WriteResult(quic::WRITE_STATUS_BLOCKED, ERR_IO_PENDING);
    }
    // A write error: the rest of the batch is lost like any packet after a
    // failed write; QUIC loss recovery retransmits its frames.
    ClearBatch();
    return result;
  }
  ClearBatch();
  return quic::WriteResult(quic::WRITE_STATUS_OK, 0);
}

void QuicChromiumPacketWriter::FlushFromTask() {
  flush_task_posted_ = false;
  if (!socket_ || !HasUnsentBatch() || IsWriteBlocked()) {
    return;
  }
  quic::WriteResult result = SendBatch();
  if (result.status == quic::WRITE_STATUS_ERROR && delegate_ != nullptr) {
    delegate_->OnWriteError(result.error_code);
  }
}

void QuicChromiumPacketWriter::WritePacketToSocket(
    scoped_refptr<ReusableIOBuffer> packet) {
  CHECK(!force_write_blocked_);
  CHECK(!IsWriteBlocked());
  packet_ = std::move(packet);
  quic::WriteResult result = WritePacketToSocketImpl();
  if (result.error_code != ERR_IO_PENDING)
    OnWriteComplete(result.error_code);
}

quic::WriteResult QuicChromiumPacketWriter::WritePacketToSocketImpl() {
  base::TimeTicks now = base::TimeTicks::Now();

  // When the connection is closed, the socket is cleaned up. If socket is
  // invalidated, packets should not be written to the socket.
  CHECK(socket_);
  int rv = socket_->Write(packet_.get(), packet_->size(), write_callback_,
                          kTrafficAnnotation);

  if (MaybeRetryAfterWriteError(rv))
    return quic::WriteResult(quic::WRITE_STATUS_BLOCKED_DATA_BUFFERED,
                             ERR_IO_PENDING);

  if (rv < 0 && rv != ERR_IO_PENDING && delegate_ != nullptr) {
    // If write error, then call delegate's HandleWriteError, which
    // may be able to migrate and rewrite packet on a new socket.
    // HandleWriteError returns the outcome of that rewrite attempt.
    rv = delegate_->HandleWriteError(rv, std::move(packet_));
    DCHECK(packet_ == nullptr);
  }

  quic::WriteStatus status = quic::WRITE_STATUS_OK;
  if (rv < 0) {
    if (rv != ERR_IO_PENDING) {
      status = quic::WRITE_STATUS_ERROR;
    } else {
      status = quic::WRITE_STATUS_BLOCKED_DATA_BUFFERED;
      write_in_progress_ = true;
    }
  }

  base::TimeDelta delta = base::TimeTicks::Now() - now;
  if (status == quic::WRITE_STATUS_OK) {
    UMA_HISTOGRAM_TIMES("Net.QuicSession.PacketWriteTime.Synchronous", delta);
  } else if (quic::IsWriteBlockedStatus(status)) {
    UMA_HISTOGRAM_TIMES("Net.QuicSession.PacketWriteTime.Asynchronous", delta);
  }

  return quic::WriteResult(status, rv);
}

void QuicChromiumPacketWriter::RetryPacketAfterNoBuffers() {
  DCHECK_GT(retry_count_, 0);
  if (socket_) {
    quic::WriteResult result = WritePacketToSocketImpl();
    if (result.error_code != ERR_IO_PENDING) {
      OnWriteComplete(result.error_code);
    }
  }
}

bool QuicChromiumPacketWriter::IsWriteBlocked() const {
  return (force_write_blocked_ || write_in_progress_);
}

void QuicChromiumPacketWriter::SetWritable() {
  write_in_progress_ = false;
}

std::optional<int> QuicChromiumPacketWriter::MessageTooBigErrorCode() const {
  return ERR_MSG_TOO_BIG;
}

void QuicChromiumPacketWriter::OnWriteComplete(int rv) {
  DCHECK_NE(rv, ERR_IO_PENDING);
  write_in_progress_ = false;
  if (delegate_ == nullptr)
    return;

  if (rv < 0) {
    if (MaybeRetryAfterWriteError(rv))
      return;

    // If write error, then call delegate's HandleWriteError, which
    // may be able to migrate and rewrite packet on a new socket.
    // HandleWriteError returns the outcome of that rewrite attempt.
    rv = delegate_->HandleWriteError(rv, std::move(packet_));
    DCHECK(packet_ == nullptr);
    if (rv == ERR_IO_PENDING) {
      // Set write blocked back as write error is encountered in this writer,
      // delegate may be able to handle write error but this writer will never
      // be used to write any new data.
      write_in_progress_ = true;
      return;
    }
  }
  if (retry_count_ != 0) {
    RecordRetryCount(retry_count_);
    retry_count_ = 0;
  }

  if (rv < 0) {
    ClearBatch();
    delegate_->OnWriteError(rv);
    return;
  }
  if (HasUnsentBatch()) {
    quic::WriteResult result = WriteBatchRemainderSingly();
    if (quic::IsWriteBlockedStatus(result.status)) {
      return;
    }
    if (result.status == quic::WRITE_STATUS_ERROR) {
      delegate_->OnWriteError(result.error_code);
      return;
    }
  }
  if (!force_write_blocked_)
    delegate_->OnWriteUnblocked();
}

bool QuicChromiumPacketWriter::MaybeRetryAfterWriteError(int rv) {
  if (rv != ERR_NO_BUFFER_SPACE)
    return false;

  if (retry_count_ >= kMaxRetries) {
    RecordRetryCount(retry_count_);
    return false;
  }

  retry_timer_.Start(
      FROM_HERE, base::Milliseconds(UINT64_C(1) << retry_count_),
      base::BindOnce(&QuicChromiumPacketWriter::RetryPacketAfterNoBuffers,
                     weak_factory_.GetWeakPtr()));
  retry_count_++;
  write_in_progress_ = true;
  return true;
}

quic::QuicByteCount QuicChromiumPacketWriter::GetMaxPacketSize(
    const quic::QuicSocketAddress& peer_address) const {
  return quic::kMaxOutgoingPacketSize;
}

bool QuicChromiumPacketWriter::SupportsReleaseTime() const {
  return false;
}

bool QuicChromiumPacketWriter::IsBatchMode() const {
  return socket_ && socket_->SupportsWriteBatch();
}

bool QuicChromiumPacketWriter::SupportsEcn() const {
  return true;
}

quic::QuicPacketBuffer QuicChromiumPacketWriter::GetNextWriteLocation(
    const quiche::QuicheIpAddress& self_address,
    const quic::QuicSocketAddress& peer_address) {
  return {nullptr, nullptr};
}

quic::WriteResult QuicChromiumPacketWriter::Flush() {
  if (!socket_ || !HasUnsentBatch()) {
    return quic::WriteResult(quic::WRITE_STATUS_OK, 0);
  }
  if (IsWriteBlocked()) {
    return quic::WriteResult(quic::WRITE_STATUS_BLOCKED, ERR_IO_PENDING);
  }
  if (g_current_flush_deferral != nullptr) {
    if (!flush_deferred_) {
      flush_deferred_ = true;
      g_current_flush_deferral->deferred_.push_back(weak_factory_.GetWeakPtr());
    }
    return quic::WriteResult(quic::WRITE_STATUS_OK, 0);
  }
  return SendBatch();
}

bool QuicChromiumPacketWriter::OnSocketClosed(DatagramClientSocket* socket) {
  if (socket_ == socket) {
    socket_ = nullptr;
    ClearBatch();
    return true;
  }
  return false;
}

void QuicChromiumPacketWriter::RegisterQuicConnectionClosePayload(
    base::span<uint8_t> payload) {
  if (socket_) {
    socket_->RegisterQuicConnectionClosePayload(payload);
  }
}

void QuicChromiumPacketWriter::UnregisterQuicConnectionClosePayload() {
  if (socket_) {
    socket_->UnregisterQuicConnectionClosePayload();
  }
}

}  // namespace net
