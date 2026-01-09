/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifndef _WIN32
#include <unistd.h>
#endif

#include <memory>
#include <thread>

namespace gloo {

// The lifecycle of the unbound buffer is controlled by the user.
// It may be constructed as value or unique_ptr and destructed when
// going out of scope or when unwinding the stack.
//
// For example:
//
//   {
//     int i = 0;
//     auto buf = context->createUnboundBuffer(&i, sizeof(i));
//     buf->send(1, 0);
//     ...
//     throw std::runtime_error("Something happened!");
//     ...
//     buf->waitSend();
//   }
//
//   // At this point, if `waitSend` was not yet called, the I/O
//   // thread executing the send operation may still call into it.
//   // The unbound buffer will have been destructed already,
//   // so we need to make sure the reference gets invalidated.
//
// If upon destruction there are pending operations (e.g. a recv
// operation timed out), there will be references to this unbound
// buffer in the `transport::Pair` instances that could fulfill the
// operation. To avoid these pairs calling into the unbound buffer
// after it is destructed, we must ensure that these references are
// invalidated before the destructor returns. Also, if destruction of
// the unbound buffer races with the recv operation completing after
// all, we must block the destructor and wait for the operation to
// finish. Otherwise, we risk the device thread writing to memory it's
// not supposed to.
//
// We solve this by using a shared_ptr of the "this" pointer of the
// unbound buffer. This doesn't magically convert the unbound buffer
// to be a shared_ptr, but it allows for handing out weak_ptr
// instances to refer to it. Then, whenever the unbound buffer is
// used by another thread, it converts the weak_ptr into a shared_ptr
// and uses it for a very short period of time. The wrapper class below
// waits for all shared_ptr instances to be released before returning
// from its destructor. This will block indefinitely if the shared_ptr
// acquired from the weak_ptr stays alive.
//

// Forward definitions.
template <typename T>
class WeakNonOwningPtr;
template <typename T>
class ShareableNonOwningPtr;

// NonOwningPtr is constructed from a WeakNonOwningPtr, if and
// only if the underlying object is still alive. If it is, destruction
// of the underlying object is blocked until the NonOwningPtr
// is destructed. It boxes a shared_ptr instead of being typedef'd as
// one to prevent misuse (e.g. extending its lifetime).
template <typename T>
class NonOwningPtr final {
 public:
  NonOwningPtr() = default;

  explicit NonOwningPtr(const WeakNonOwningPtr<T>& ptr)
      : ptr_(ptr.ptr_.lock()) {}

  T* operator->() const noexcept {
    return ptr_.get();
  }

  explicit operator bool() const noexcept {
    return (bool)ptr_;
  }

 private:
  std::shared_ptr<T> ptr_;
};

// WeakNonOwningPtr can be constructed from a ShareableNonOwningPtr.
// It can instantiate a NonOwningPtr if and only if the
// underlying object is still alive. It boxes a weak_ptr instead of
// being typedef'd as one because it must instantiate the
// NonOwningPtr type instead of a raw shared_ptr.
template <typename T>
class WeakNonOwningPtr final {
 public:
  WeakNonOwningPtr() = default;

  explicit WeakNonOwningPtr(const ShareableNonOwningPtr<T>& ref)
      : ptr_(ref.ptr_) {}

  // Returns true if the instance was initialized.
  explicit operator bool() const noexcept {
    // Per std::weak_ptr::owner_before, "[...] two smart pointers
    // compare equivalent only if they are both empty or if they both
    // own the same object [...]". Therefore, if owner_before is true
    // in either direction w.r.t. an empty weak_ptr, the instance was
    // initialized.
    return ptr_.owner_before(std::weak_ptr<T>{}) ||
        std::weak_ptr<T>{}.owner_before(ptr_);
  }

 protected:
  std::weak_ptr<T> ptr_;

  friend class NonOwningPtr<T>;
};

// ShareableNonOwningPtr is the root reference to the this pointer. It
// can instantiate WeakNonOwningPtr instances. The destructor blocks
// until all NonOwningPtr instances have been destroyed. This
// guarantees that the object it refers to has no other active
// references (it may have weak ones) and can safely be destructed.
template <typename T>
class ShareableNonOwningPtr final {
 public:
  // Initializes private shared_ptr with nop deallocation function.
  explicit ShareableNonOwningPtr(T* t) : ptr_(t, [](void* ptr) {}) {}

  // Disable copy constructors.
  ShareableNonOwningPtr(const ShareableNonOwningPtr&) = delete;
  ShareableNonOwningPtr& operator=(ShareableNonOwningPtr const&) = delete;

  ~ShareableNonOwningPtr() {
    // Acquire weak_ptr to T
    auto weak = std::weak_ptr<T>(ptr_);
    // Release shared_ptr to T
    ptr_.reset();
    // Wait for all shared_ptr's to T have been released
    while (!weak.expired()) {
      std::this_thread::yield();
    }
  }

 protected:
  std::shared_ptr<T> ptr_;

  friend class WeakNonOwningPtr<T>;
};

} // namespace gloo
