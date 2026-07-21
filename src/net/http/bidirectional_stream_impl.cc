// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/bidirectional_stream_impl.h"

#include "net/base/net_errors.h"

namespace net {

BidirectionalStreamImpl::Delegate::Delegate() = default;

BidirectionalStreamImpl::Delegate::~Delegate() = default;

BidirectionalStreamImpl::BidirectionalStreamImpl() = default;

BidirectionalStreamImpl::~BidirectionalStreamImpl() = default;

// Default: subclasses (BidirectionalStreamSpdyImpl) that don't support
// HTTP/3 datagrams return ERR_NOT_IMPLEMENTED. BidirectionalStreamQuicImpl
// overrides to dispatch to QuicChromiumClientStream::Handle.
int BidirectionalStreamImpl::SendHttp3Datagram(base::span<const uint8_t>) {
  return ERR_NOT_IMPLEMENTED;
}

}  // namespace net
