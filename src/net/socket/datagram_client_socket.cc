// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/datagram_client_socket.h"

#include "net/base/net_errors.h"

namespace net {

bool DatagramClientSocket::SupportsWriteBatch() const {
  return false;
}

int DatagramClientSocket::WriteBatch(
    base::span<const base::span<const uint8_t>> packets) {
  return ERR_NOT_IMPLEMENTED;
}

}  // namespace net
