/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/common/win.h"
#include "gloo/common/enforce.h"

#include <mutex>

namespace gloo {

static std::once_flag init_flag;

// The WSAStartup function must be the first Windows Sockets function
// called by an application or DLL.
// https://docs.microsoft.com/ru-ru/windows/win32/api/winsock/nf-winsock-wsastartup

void init_winsock() {
  std::call_once(init_flag, []() {
    WSADATA wsa_data;
    int res;
    res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (res != 0) {
      GLOO_ENFORCE(false, "WSAStartup failed: ", res);
    }
  });
}

} // namespace gloo
