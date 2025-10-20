/**
 * Copyright (c) 2021-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

struct DynamicLibrary {
  DynamicLibrary(const DynamicLibrary&) = delete;

  void operator=(const DynamicLibrary&) = delete;

  DynamicLibrary(const char* name, const char* alt_name);

  void* sym(const char* name);

  ~DynamicLibrary();

 private:
  const std::string lib_name;
  void* handle = nullptr;
};
