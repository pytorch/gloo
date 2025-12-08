/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// This file contains a libuv wrapper for C++.
//
// The code in this file is in part included verbatim from and
// otherwise inspired by `uvw` (https://github.com/skypjack/uvw).
// See the `LICENSE.uvw` file for a copy of the `uvw` license.
//
// Reasons for not using `uvw` directly:
// * Upstream requires C++17. Gloo requires C++11.
// * No way to pass externally managed memory to the read functions.
//

#include <stdio.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <uv.h> // @manual

#define UV_ASSERT(rv, prefix) \
  {                           \
    if ((rv) != 0) {          \
      fprintf(                \
          stderr,             \
          "[%s:%d] %s: %s\n", \
          __FILE__,           \
          __LINE__,           \
          prefix,             \
          uv_strerror(rv));   \
      abort();                \
    }                         \
  }                           \
  while (0)                   \
    ;

namespace gloo {
namespace transport {
namespace uv {
namespace libuv {

// Base class for handles.
// Used to differentiate between handles and requests.
struct BaseHandle {};

// Base class for requests.
// Used to differentiate between handles and requests.
struct BaseRequest {};

// Event type for errors.
class ErrorEvent {
 public:
  explicit ErrorEvent(int error) : error_(error) {}

  operator bool() const {
    return error_ != 0;
  }

  int code() const {
    return error_;
  }

  const char* what() const {
    return uv_strerror(error_);
  }

 private:
  const int error_;
};

// Event emitter.
//
// Both handles and requests emit events.
//
template <typename T>
class Emitter {
  struct BaseHandler {
    virtual ~BaseHandler() noexcept = default;
  };

  template <typename E>
  struct Handler final : BaseHandler {
    using Listener = std::function<void(E&, T&)>;
    using Element = std::pair<bool, Listener>;
    using ListenerList = std::list<Element>;
    using Connection = typename ListenerList::iterator;

    Connection once(Listener f) {
      return onceL.emplace(onceL.cend(), false, std::move(f));
    }

    Connection on(Listener f) {
      return onL.emplace(onL.cend(), false, std::move(f));
    }

    void erase(Connection conn) noexcept {
      conn->first = true;

      if (!publishing) {
        auto pred = [](Element& element) { return element.first; };
        onceL.remove_if(pred);
        onL.remove_if(pred);
      }
    }

    void publish(E event, T& ref) {
      ListenerList currentL;
      onceL.swap(currentL);

      auto func = [&event, &ref](Element& element) {
        return element.first ? void() : element.second(event, ref);
      };

      publishing = true;

      std::for_each(onL.rbegin(), onL.rend(), func);
      std::for_each(currentL.rbegin(), currentL.rend(), func);

      publishing = false;

      onL.remove_if([](Element& element) { return element.first; });
    }

   private:
    bool publishing{false};
    ListenerList onceL{};
    ListenerList onL{};
  };

  static std::size_t next_type() noexcept {
    static std::size_t counter = 0;
    return counter++;
  }

  template <typename>
  static std::size_t event_type() noexcept {
    static std::size_t value = next_type();
    return value;
  }

  template <typename E>
  Handler<E>& handler() noexcept {
    std::size_t type = event_type<E>();

    if (!(type < handlers_.size())) {
      handlers_.resize(type + 1);
    }

    if (!handlers_[type]) {
      handlers_[type] = std::unique_ptr<Handler<E>>(new Handler<E>);
    }

    return static_cast<Handler<E>&>(*handlers_[type]);
  }

 protected:
  template <typename E>
  void publish(E event) {
    handler<E>().publish(std::move(event), *static_cast<T*>(this));
  }

 public:
  template <typename E>
  using Listener = typename Handler<E>::Listener;

  template <typename E>
  struct Connection : private Handler<E>::Connection {
    template <typename>
    friend class Emitter;

    Connection() = default;
    Connection(const Connection&) = default;
    Connection(Connection&&) = default;

    Connection(typename Handler<E>::Connection conn)
        : Handler<E>::Connection{std::move(conn)} {}

    Connection& operator=(const Connection&) = default;
    Connection& operator=(Connection&&) = default;
  };

  template <typename E>
  Connection<E> once(Listener<E> f) {
    return handler<E>().once(std::move(f));
  }

  template <typename E>
  Connection<E> on(Listener<E> f) {
    return handler<E>().on(std::move(f));
  }

  template <typename E>
  void erase(Connection<E> conn) noexcept {
    handler<E>().erase(std::move(conn));
  }

 private:
  std::vector<std::unique_ptr<BaseHandler>> handlers_;
};

class Loop : public std::enable_shared_from_this<Loop> {
 public:
  explicit Loop(std::unique_ptr<uv_loop_t> ptr) : loop_(std::move(ptr)) {}

  static std::shared_ptr<Loop> create() {
    auto ptr = std::make_unique<uv_loop_t>();
    auto loop = std::make_shared<Loop>(std::move(ptr));
    auto rv = uv_loop_init(loop->loop_.get());
    UV_ASSERT(rv, "uv_loop_init");
    return loop;
  }

  template <typename T, typename... Args>
  std::enable_if_t<std::is_base_of_v<BaseHandle, T>, std::shared_ptr<T>>
  resource(Args&&... args) {
    auto handle = T::create(shared_from_this(), std::forward<Args>(args)...);
    handle->init();
    return handle;
  }

  template <typename T, typename... Args>
  std::enable_if_t<std::is_base_of_v<BaseRequest, T>, std::shared_ptr<T>>
  resource(Args&&... args) {
    return T::create(shared_from_this(), std::forward<Args>(args)...);
  }

  void run() {
    uv_run(loop_.get(), UV_RUN_DEFAULT);
  }

  uv_loop_t* raw() {
    return loop_.get();
  }

 private:
  std::unique_ptr<uv_loop_t> loop_;
};

// Typename T is the wrapped type name.
// Typename U is the underlying libuv type name.
// Base class to both handles and requests.
// Must only be instantiated from an uv_loop.
template <typename T, typename U>
class Resource : public Emitter<T>, public std::enable_shared_from_this<T> {
 protected:
  void leak() {
    leak_ = this->shared_from_this();
  }

  void unleak() {
    leak_.reset();
  }

  const U* get() const noexcept {
    return &resource_;
  }

  U* get() noexcept {
    return &resource_;
  }

  template <typename R>
  const R* get() const noexcept {
    static_assert(!std::is_same_v<R, U>, "!");
    return reinterpret_cast<const R*>(&resource_);
  }

  template <typename R>
  R* get() noexcept {
    static_assert(!std::is_same_v<R, U>, "!");
    return reinterpret_cast<R*>(&resource_);
  }

  template <typename R, typename... P>
  const R* get(const Resource<P...>& other) const noexcept {
    return reinterpret_cast<const R*>(&other.resource_);
  }

  template <typename R, typename... P>
  R* get(Resource<P...>& other) noexcept {
    return reinterpret_cast<R*>(&other.resource_);
  }

  Loop& loop() const noexcept {
    return *loop_;
  }

 public:
  explicit Resource(std::shared_ptr<Loop> loop) : loop_(std::move(loop)) {
    get()->data = static_cast<T*>(this);
  }

  template <typename... Args>
  static std::shared_ptr<T> create(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
  }

  const U* raw() const noexcept {
    return &resource_;
  }

 protected:
  U resource_;

  std::shared_ptr<Loop> loop_;
  std::shared_ptr<T> leak_;
};

class CloseEvent {};

template <typename T, typename U>
class Handle : public Resource<T, U>, public BaseHandle {
  static void uv__close_cb(uv_handle_t* handle) {
    T& ref = *static_cast<T*>(handle->data);
    ref.publish(CloseEvent{});
    ref.unleak();
  };

 protected:
  template <typename F, typename... Args>
  void init(F&& f, Args&&... args) {
    auto rv = std::forward<F>(f)(
        this->loop_->raw(), this->get(), std::forward<Args>(args)...);
    UV_ASSERT(rv, typeid(T).name());
    this->leak();
  }

  template <typename F, typename... Args>
  std::invoke_result_t<F, Args...> invoke(F&& f, Args&&... args) {
    return std::forward<F>(f)(std::forward<Args>(args)...);
  }

 public:
  using Resource<T, U>::Resource;

  bool closing() const noexcept {
    return !(uv_is_closing(this->template get<uv_handle_t>()) == 0);
  }

  void close() {
    if (!closing()) {
      uv_close(this->template get<uv_handle_t>(), &uv__close_cb);
    }
  }
};

template <typename T, typename U>
class Request : public Resource<T, U>, public BaseRequest {
 protected:
  template <typename E>
  static void defaultCallback(U* req, int status) {
    auto& ref = *static_cast<T*>(req->data);
    if (status) {
      ref.publish(ErrorEvent{status});
    } else {
      ref.publish(E{});
    }
    ref.unleak();
  }

  // Call non-void libuv function for this uv_request_t.
  // The request is leaked if the call is successful, under the
  // assumption that it is unleaked when the callback gets called.
  template <typename F, typename... Args>
  std::enable_if_t<
      !std::is_void_v<std::invoke_result_t<F, Args...>>,
      std::invoke_result_t<F, Args...>>
  invoke(F&& f, Args&&... args) {
    auto err = std::forward<F>(f)(std::forward<Args>(args)...);
    if (err) {
      Emitter<T>::publish(ErrorEvent{err});
    } else {
      this->leak();
    }
    return err;
  }

 public:
  using Resource<T, U>::Resource;
};

struct AsyncEvent {};

class Async : public Handle<Async, uv_async_t> {
  static void uv__async_cb(uv_async_t* handle) {
    Async& async = *static_cast<Async*>(handle->data);
    async.publish(AsyncEvent{});
  };

 public:
  using Handle::Handle;

  void init() {
    Handle<Async, uv_async_t>::init(&uv_async_init, &uv__async_cb);
  }

  void send() {
    invoke(&uv_async_send, get());
  }
};

struct TimerEvent {};

class Timer : public Handle<Timer, uv_timer_t> {
  static void uv__timer_cb(uv_timer_t* handle) {
    Timer& timer = *static_cast<Timer*>(handle->data);
    timer.publish(TimerEvent{});
  };

 public:
  using Handle::Handle;

  void init() {
    Handle::init(&uv_timer_init);
  }

  void start(std::chrono::milliseconds timeout) {
    auto rv = uv_timer_start(get(), &uv__timer_cb, timeout.count(), 0);
    UV_ASSERT(rv, "uv_timer_start");
  }
};

class EndEvent {};

class ListenEvent {};

class ConnectEvent {};

class ReadEvent {
 public:
  using Deleter = void (*)(char*);

  ReadEvent(std::unique_ptr<char[], Deleter> data, size_t length)
      : data(std::move(data)), length(length) {}

  std::unique_ptr<char[], Deleter> data;
  size_t length;

  template <typename T>
  std::enable_if_t<std::is_trivially_copyable_v<T>, T> as() const {
    if (length != sizeof(T)) {
      abort();
    }
    return *reinterpret_cast<T* const>(data.get());
  }
};

class WriteEvent {};

namespace detail {

class ReadSegment {
 public:
  using Deleter = void (*)(char*);

  ReadSegment(char* ptr, size_t length)
      : data_(ptr, [](char*) {}), length_(length) {}

  ReadSegment(std::unique_ptr<char[]> data, size_t length)
      : data_(data.release(), [](char* ptr) { delete[] ptr; }),
        length_(length) {}

  ReadSegment(std::unique_ptr<char[], Deleter> data, size_t length)
      : data_(std::move(data)), length_(length) {}

  void alloc(uv_buf_t* buf) {
    buf->base = data_.get() + offset_;
    buf->len = length_ - offset_;
  }

  void read(ssize_t nread) {
    assert(nread > 0);
    assert(nread <= length_ - offset_);
    offset_ += nread;
  }

  bool done() const noexcept {
    return offset_ == length_;
  }

  ReadEvent event() {
    return ReadEvent(std::move(data_), length_);
  }

 private:
  std::unique_ptr<char[], Deleter> data_;
  size_t length_;

  // Offset is updated after reads/writes to reflect how many bytes
  // of this segment have already been read/written.
  size_t offset_ = 0;
};

class WriteRequest final : public Request<WriteRequest, uv_write_t> {
 public:
  using Deleter = void (*)(char*);

  WriteRequest(
      std::shared_ptr<Loop> loop,
      std::unique_ptr<char[], Deleter> data,
      unsigned int length)
      : Request<WriteRequest, uv_write_t>(std::move(loop)),
        data_(std::move(data)),
        buf_(uv_buf_init(data_.get(), length)) {}

  void write(uv_stream_t* handle) {
    invoke(&uv_write, get(), handle, &buf_, 1, &defaultCallback<WriteEvent>);
  }

 private:
  std::unique_ptr<char[], Deleter> data_;
  uv_buf_t buf_;
};

class ConnectRequest final : public Request<ConnectRequest, uv_connect_t> {
 public:
  ConnectRequest(std::shared_ptr<Loop> loop, const struct sockaddr* addr)
      : Request<ConnectRequest, uv_connect_t>(std::move(loop)), addr_{addr} {}

  void connect(uv_tcp_t* handle) {
    invoke(
        &uv_tcp_connect, get(), handle, addr_, &defaultCallback<ConnectEvent>);
  }

 private:
  const struct sockaddr* addr_;
};

}; // namespace detail

class TCP final : public Handle<TCP, uv_tcp_t> {
  static constexpr unsigned int kDefaultListenBacklog = 128;

  static void uv__connection_cb(uv_stream_t* server, int status);

  static void uv__alloc_cb(
      uv_handle_t* handle,
      size_t suggested_size,
      uv_buf_t* buf);

  static void uv__read_cb(
      uv_stream_t* stream,
      ssize_t nread,
      const uv_buf_t* buf);

  static void uv__write_cb(uv_write_t* req, int status) {}

 public:
  using Handle::Handle;

  void init() {
    Handle::init(uv_tcp_init);
  }

  bool noDelay(bool value = false) {
    return (0 == uv_tcp_nodelay(get(), value));
  }

  void bind(const struct sockaddr* addr) {
    auto rv = uv_tcp_bind(get(), addr, 0);
    UV_ASSERT(rv, "uv_bind");
  }

  void listen(int backlog = kDefaultListenBacklog) {
    auto rv = uv_listen(
        this->template get<uv_stream_t>(), backlog, &uv__connection_cb);
    UV_ASSERT(rv, "uv_listen");
  }

  template <typename V>
  void accept(V& stream) {
    auto rv = uv_accept(
        this->template get<uv_stream_t>(),
        this->template get<uv_stream_t>(stream));
    UV_ASSERT(rv, "uv_accept");
  }

  void read(char* ptr, size_t length) {
    reads_.emplace_back(ptr, length);
    if (reads_.size() == 1) {
      auto rv = uv_read_start(
          this->template get<uv_stream_t>(), &uv__alloc_cb, &uv__read_cb);
      UV_ASSERT(rv, "uv_read_start");
    }
  }

  void read(std::unique_ptr<char[]> buf, size_t length) {
    reads_.emplace_back(std::move(buf), length);
    if (reads_.size() == 1) {
      auto rv =
          uv_read_start(this->get<uv_stream_t>(), &uv__alloc_cb, &uv__read_cb);
      UV_ASSERT(rv, "uv_read_start");
    }
  }

  void write(char* ptr, size_t length) {
    write(this->loop().resource<detail::WriteRequest>(
        std::unique_ptr<char[], detail::WriteRequest::Deleter>(
            ptr, [](char*) {}),
        length));
  }

  void write(std::unique_ptr<char[]> data, size_t length) {
    write(this->loop().resource<detail::WriteRequest>(
        std::unique_ptr<char[], detail::WriteRequest::Deleter>(
            data.release(), [](char* ptr) { delete[] ptr; }),
        length));
  }

  template <typename T>
  void write(T t) {
    static_assert(
        std::is_trivially_copyable_v<T>,
        "Only trivially copyable types can be written directly.");
    auto data = std::unique_ptr<char[], detail::WriteRequest::Deleter>(
        new char[sizeof(T)], [](char* ptr) { delete[] ptr; });
    std::memcpy(data.get(), &t, sizeof(T));
    write(this->loop().resource<detail::WriteRequest>(
        std::move(data), sizeof(T)));
  }

  void connect(const struct sockaddr& addr) {
    auto req = this->loop().resource<detail::ConnectRequest>(&addr);
    auto handle = shared_from_this();
    req->once<ErrorEvent>(
        [handle](const ErrorEvent& event, const detail::ConnectRequest&) {
          handle->publish(event);
        });
    req->once<ConnectEvent>(
        [handle](const ConnectEvent& event, const detail::ConnectRequest&) {
          handle->publish(event);
        });
    req->connect(get());
  }

  struct sockaddr_storage sockname() const {
    struct sockaddr_storage addr;
    int len = sizeof(addr);
    auto rv = uv_tcp_getsockname(get(), (struct sockaddr*)&addr, &len);
    UV_ASSERT(rv, "uv_tcp_getsockname");
    return addr;
  }

  struct sockaddr_storage peername() const {
    struct sockaddr_storage addr;
    int len = sizeof(addr);
    auto rv = uv_tcp_getpeername(get(), (struct sockaddr*)&addr, &len);
    UV_ASSERT(rv, "uv_tcp_getpeername");
    return addr;
  }

 protected:
  std::deque<detail::ReadSegment> reads_;

 protected:
  void write(std::shared_ptr<detail::WriteRequest> req) {
    auto handle = shared_from_this();
    req->once<ErrorEvent>(
        [handle](const ErrorEvent& event, const detail::WriteRequest&) {
          handle->publish(event);
        });
    req->once<WriteEvent>(
        [handle](const WriteEvent& event, const detail::WriteRequest&) {
          handle->publish(event);
        });
    req->write(get<uv_stream_t>());
  }
};

} // namespace libuv
} // namespace uv
} // namespace transport
} // namespace gloo
