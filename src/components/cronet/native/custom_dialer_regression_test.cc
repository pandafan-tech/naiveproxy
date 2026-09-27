// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>

#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "net/base/address_list.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/log/net_log_source.h"
#include "net/socket/custom_client_socket_factory.h"
#include "net/socket/transport_client_socket.h"

namespace {

#if BUILDFLAG(IS_POSIX)
bool CanceledDialClosesReturnedDescriptor(
    const net::AddressList& addresses) {
  net::CustomClientSocketFactory::DialerCompletionCallback pending_completion;
  net::CustomClientSocketFactory factory(
      base::BindRepeating(
          [](net::CustomClientSocketFactory::DialerCompletionCallback* pending,
             const std::string&, uint16_t,
             net::CustomClientSocketFactory::DialerCompletionCallback
                 completion) { *pending = std::move(completion); },
          &pending_completion),
      {});
  auto socket = factory.CreateTransportClientSocket(
      addresses, net::handles::kInvalidNetworkHandle, nullptr, nullptr, nullptr,
      net::NetLogSource());
  if (socket->Connect(base::DoNothing()) != net::ERR_IO_PENDING ||
      !pending_completion) {
    return false;
  }
  socket.reset();

  int descriptors[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return false;
  }
  std::move(pending_completion).Run(descriptors[0]);
  base::RunLoop().RunUntilIdle();
  errno = 0;
  const bool closed = fcntl(descriptors[0], F_GETFD) == -1 && errno == EBADF;
  close(descriptors[1]);
  return closed;
}
#endif

}  // namespace

int main() {
  base::SingleThreadTaskExecutor task_executor;
  int dial_count = 0;
  net::CustomClientSocketFactory factory(
      base::BindRepeating(
          [](int* dial_count, const std::string&, uint16_t,
             net::CustomClientSocketFactory::DialerCompletionCallback
                 completion) {
            ++*dial_count;
            std::move(completion).Run(net::ERR_CONNECTION_TIMED_OUT);
          },
          &dial_count),
      {});
  const net::AddressList addresses = net::AddressList::CreateFromIPAddress(
      net::IPAddress::IPv4Localhost(), 443);

  const auto started_at = std::chrono::steady_clock::now();
  auto socket = factory.CreateTransportClientSocket(
      addresses, net::handles::kInvalidNetworkHandle, nullptr, nullptr, nullptr,
      net::NetLogSource());
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  if (elapsed >= std::chrono::milliseconds(50) || dial_count != 0) {
    return 1;
  }

  base::RunLoop run_loop;
  int callback_result = net::OK;
  const int connect_result = socket->Connect(base::BindOnce(
      [](base::RunLoop* run_loop, int* callback_result, int result) {
        *callback_result = result;
        run_loop->Quit();
      },
      &run_loop, &callback_result));
  if (connect_result != net::ERR_IO_PENDING || dial_count != 1) {
    return 2;
  }
  run_loop.Run();

#if BUILDFLAG(IS_POSIX)
  if (!CanceledDialClosesReturnedDescriptor(addresses)) {
    return 3;
  }
#endif

  return callback_result == net::ERR_CONNECTION_TIMED_OUT ? 0 : 4;
}
