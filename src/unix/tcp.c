/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

/*
1 获取一个新的socket fd
2 把fd保存到handle里，并根据flag进行相关设置
3 绑定到本机随意的地址（如果设置了该标记的话）
*/
static int new_socket(uv_tcp_t* handle, int domain, unsigned long flags) {
  struct sockaddr_storage saddr;
  socklen_t slen;
  int sockfd;
  int err;
  // 获取一个socket
  err = uv__socket(domain, SOCK_STREAM, 0);
  if (err < 0)
    return err;
  sockfd = err;
  // 设置选项和保存socket的文件描述符到io观察者中
  err = uv__stream_open((uv_stream_t*) handle, sockfd, flags);
  if (err) {
    uv__close(sockfd);
    return err;
  }
  // 设置了需要绑定标记UV_HANDLE_BOUND	 
  if (flags & UV_HANDLE_BOUND) {
    /* Bind this new socket to an arbitrary port */
    slen = sizeof(saddr);
    memset(&saddr, 0, sizeof(saddr));
    // 获取fd对应的socket信息，如果没有则随便获取本机的地址
    if (getsockname(uv__stream_fd(handle), (struct sockaddr*) &saddr, &slen)) {
      uv__close(sockfd);
      return UV__ERR(errno);
    }
    // 绑定到socket中
    if (bind(uv__stream_fd(handle), (struct sockaddr*) &saddr, slen)) {
      uv__close(sockfd);
      return UV__ERR(errno);
    }
  }

  return 0;
}

// 对new_socket的封装，并处理了new_socket后面一段代码报错导致遗留的问题
static int maybe_new_socket(uv_tcp_t* handle, int domain, unsigned long flags) {
  struct sockaddr_storage saddr;
  socklen_t slen;

  if (domain == AF_UNSPEC) {
    handle->flags |= flags;
    return 0;
  }
  // 已经有socket fd了
  if (uv__stream_fd(handle) != -1) {

    if (flags & UV_HANDLE_BOUND) {
      /*
	      流是否已经绑定到一个地址了。handle的flag是在new_socket里设置的，
	      如果有这个标记说明已经执行过绑定了，直接更新flags就行。
      */
      if (handle->flags & UV_HANDLE_BOUND) {
        /* It is already bound to a port. */
        handle->flags |= flags;
        return 0;
      }
      // 有socket fd，但是可能还没绑定到一个地址
      /* Query to see if tcp socket is bound. */
      slen = sizeof(saddr);
      memset(&saddr, 0, sizeof(saddr));
      // 获取socket绑定到的地址
      if (getsockname(uv__stream_fd(handle), (struct sockaddr*) &saddr, &slen))
        return UV__ERR(errno);
      // 绑定过了socket地址，则更新flags就行
      if ((saddr.ss_family == AF_INET6 &&
          ((struct sockaddr_in6*) &saddr)->sin6_port != 0) ||
          (saddr.ss_family == AF_INET &&
          ((struct sockaddr_in*) &saddr)->sin_port != 0)) {
        /* Handle is already bound to a port. */
        handle->flags |= flags;
        return 0;
      }
      // 没绑定则绑定到随机地址，bind中实现
      /* Bind to arbitrary port */
      if (bind(uv__stream_fd(handle), (struct sockaddr*) &saddr, slen))
        return UV__ERR(errno);
    }

    handle->flags |= flags;
    return 0;
  }

  return new_socket(handle, domain, flags);
}


int uv_tcp_init_ex(uv_loop_t* loop, uv_tcp_t* tcp, unsigned int flags) {
  int domain;

  /* Use the lower 8 bits for the domain */
  // 低八位是domain
  domain = flags & 0xFF;
  if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNSPEC)
    return UV_EINVAL;
  // 除了第八位的其他位是flags
  if (flags & ~0xFF)
    return UV_EINVAL;

  uv__stream_init(loop, (uv_stream_t*)tcp, UV_TCP);

  /* If anything fails beyond this point we need to remove the handle from
   * the handle queue, since it was added by uv__handle_init in uv_stream_init.
   */

  if (domain != AF_UNSPEC) {
    int err = maybe_new_socket(tcp, domain, 0);
    if (err) {
      // 出错则把该handle移除loop队列
      QUEUE_REMOVE(&tcp->handle_queue);
      return err;
    }
  }

  return 0;
}

// 初始化一个tcp流的结构体
int uv_tcp_init(uv_loop_t* loop, uv_tcp_t* tcp) {
  // 未指定未指定协议
  return uv_tcp_init_ex(loop, tcp, AF_UNSPEC);
}


int uv__tcp_bind(uv_tcp_t* tcp,
                 const struct sockaddr* addr,
                 unsigned int addrlen,
                 unsigned int flags) {
  int err;
  int on;

  /* Cannot set IPv6-only mode on non-IPv6 socket. */
  if ((flags & UV_TCP_IPV6ONLY) && addr->sa_family != AF_INET6)
    return UV_EINVAL;
  // 如果直接调bind，可能还没有对应的socket fd，第三个参数传0，则不会执行bind等操作
  err = maybe_new_socket(tcp, addr->sa_family, 0);
  if (err)
    return err;

  on = 1;
  // 设置端口复用，在tcp连接2msl的时候里支持复用端口 
  if (setsockopt(tcp->io_watcher.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
    return UV__ERR(errno);

#ifndef __OpenBSD__
#ifdef IPV6_V6ONLY
  if (addr->sa_family == AF_INET6) {
    on = (flags & UV_TCP_IPV6ONLY) != 0;
    if (setsockopt(tcp->io_watcher.fd,
                   IPPROTO_IPV6,
                   IPV6_V6ONLY,
                   &on,
                   sizeof on) == -1) {
#if defined(__MVS__)
      if (errno == EOPNOTSUPP)
        return UV_EINVAL;
#endif
      return UV__ERR(errno);
    }
  }
#endif
#endif

  errno = 0;
  // 在这里进行bind
  if (bind(tcp->io_watcher.fd, addr, addrlen) && errno != EADDRINUSE) {
    if (errno == EAFNOSUPPORT)
      /* OSX, other BSDs and SunoS fail with EAFNOSUPPORT when binding a
       * socket created with AF_INET to an AF_INET6 address or vice versa. */
      return UV_EINVAL;
    return UV__ERR(errno);
  }
  // 记录错误码，成功或者EADDRINUSE
  tcp->delayed_error = UV__ERR(errno);
  
  tcp->flags |= UV_HANDLE_BOUND;
  if (addr->sa_family == AF_INET6)
    tcp->flags |= UV_HANDLE_IPV6;

  return 0;
}


int uv__tcp_connect(uv_connect_t* req,
                    uv_tcp_t* handle,
                    const struct sockaddr* addr,
                    unsigned int addrlen,
                    uv_connect_cb cb) {
  int err;
  int r;

  assert(handle->type == UV_TCP);
  // 已经发起了connect了
  if (handle->connect_req != NULL)
    return UV_EALREADY;  /* FIXME(bnoordhuis) UV_EINVAL or maybe UV_EBUSY. */
  // 申请一个socket和绑定一个地址，如果还没有的话
  err = maybe_new_socket(handle,
                         addr->sa_family,
                         UV_HANDLE_READABLE | UV_HANDLE_WRITABLE);
  if (err)
    return err;

  handle->delayed_error = 0;

  do {
    errno = 0;
    r = connect(uv__stream_fd(handle), addr, addrlen);
  } while (r == -1 && errno == EINTR);

  /* We not only check the return value, but also check the errno != 0.
   * Because in rare cases connect() will return -1 but the errno
   * is 0 (for example, on Android 4.3, OnePlus phone A0001_12_150227)
   * and actually the tcp three-way handshake is completed.
   */
  if (r == -1 && errno != 0) {
    if (errno == EINPROGRESS)
      ; /* not an error */
    else if (errno == ECONNREFUSED)
    /* If we get a ECONNREFUSED wait until the next tick to report the
     * error. Solaris wants to report immediately--other unixes want to
     * wait.
     */
      handle->delayed_error = UV__ERR(errno);
    else
      return UV__ERR(errno);
  }
  // 初始化一个request，并设置某些字段
  uv__req_init(handle->loop, req, UV_CONNECT);
  req->cb = cb;
  req->handle = (uv_stream_t*) handle;
  QUEUE_INIT(&req->queue);
  handle->connect_req = req;
  // 注册到libuv观察者队列，handle->io_watcher已经保存了fd，不需要uv__io_init了
  uv__io_start(handle->loop, &handle->io_watcher, POLLOUT);
  // 连接出错，插入pending队尾
  if (handle->delayed_error)
    uv__io_feed(handle->loop, &handle->io_watcher);

  return 0;
}


int uv_tcp_open(uv_tcp_t* handle, uv_os_sock_t sock) {
  int err;

  if (uv__fd_exists(handle->loop, sock))
    return UV_EEXIST;

  err = uv__nonblock(sock, 1);
  if (err)
    return err;

  return uv__stream_open((uv_stream_t*)handle,
                         sock,
                         UV_HANDLE_READABLE | UV_HANDLE_WRITABLE);
}


int uv_tcp_getsockname(const uv_tcp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {
  socklen_t socklen;

  if (handle->delayed_error)
    return handle->delayed_error;

  if (uv__stream_fd(handle) < 0)
    return UV_EINVAL;  /* FIXME(bnoordhuis) UV_EBADF */

  /* sizeof(socklen_t) != sizeof(int) on some systems. */
  socklen = (socklen_t) *namelen;
  // 获取fd对应的socket信息
  if (getsockname(uv__stream_fd(handle), name, &socklen))
    return UV__ERR(errno);

  *namelen = (int) socklen;
  return 0;
}

// 获取socket对端的地址
int uv_tcp_getpeername(const uv_tcp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {
  socklen_t socklen;

  if (handle->delayed_error)
    return handle->delayed_error;

  if (uv__stream_fd(handle) < 0)
    return UV_EINVAL;  /* FIXME(bnoordhuis) UV_EBADF */

  /* sizeof(socklen_t) != sizeof(int) on some systems. */
  socklen = (socklen_t) *namelen;

  if (getpeername(uv__stream_fd(handle), name, &socklen))
    return UV__ERR(errno);

  *namelen = (int) socklen;
  return 0;
}


int uv_tcp_listen(uv_tcp_t* tcp, int backlog, uv_connection_cb cb) {
  static int single_accept = -1;
  unsigned long flags;
  int err;

  if (tcp->delayed_error)
    return tcp->delayed_error;
  // 是否设置了不连续accept。默认是连续accept。
  if (single_accept == -1) {
    const char* val = getenv("UV_TCP_SINGLE_ACCEPT");
    single_accept = (val != NULL && atoi(val) != 0);  /* Off by default. */
  }
  // 设置不连续accept
  if (single_accept)
    tcp->flags |= UV_HANDLE_TCP_SINGLE_ACCEPT;

  flags = 0;
#if defined(__MVS__)
  /* on zOS the listen call does not bind automatically
     if the socket is unbound. Hence the manual binding to
     an arbitrary port is required to be done manually
  */
  flags |= UV_HANDLE_BOUND;
#endif
  /*
    可能还没有用于listen的fd，socket地址等。
    这里申请一个socket和绑定到一个地址（如果调listen之前没有调bind则绑定到随机地址）
  */
  err = maybe_new_socket(tcp, AF_INET, flags);
  if (err)
    return err;
  // 设置fd为listen状态
  if (listen(tcp->io_watcher.fd, backlog))
    return UV__ERR(errno);
  // 建立连接后的业务回调
  tcp->connection_cb = cb;
  tcp->flags |= UV_HANDLE_BOUND;

  /* Start listening for connections. */
  // 有连接到来时的libuv层回调
  tcp->io_watcher.cb = uv__server_io;
  // 注册读事件，等待连接到来
  uv__io_start(tcp->loop, &tcp->io_watcher, POLLIN);

  return 0;
}

// 开启/关闭nagle算法
int uv__tcp_nodelay(int fd, int on) {
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)))
    return UV__ERR(errno);
  return 0;
}

// 开启/关闭长连接
int uv__tcp_keepalive(int fd, int on, unsigned int delay) {
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)))
    return UV__ERR(errno);

#ifdef TCP_KEEPIDLE
  if (on && setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &delay, sizeof(delay)))
    return UV__ERR(errno);
#endif

  /* Solaris/SmartOS, if you don't support keep-alive,
   * then don't advertise it in your system headers...
   */
  /* FIXME(bnoordhuis) That's possibly because sizeof(delay) should be 1. */
#if defined(TCP_KEEPALIVE) && !defined(__sun)
  if (on && setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &delay, sizeof(delay)))
    return UV__ERR(errno);
#endif

  return 0;
}

// 还没有对应的fd，则先打标记，有则直接设置
int uv_tcp_nodelay(uv_tcp_t* handle, int on) {
  int err;

  if (uv__stream_fd(handle) != -1) {
    err = uv__tcp_nodelay(uv__stream_fd(handle), on);
    if (err)
      return err;
  }

  if (on)
    handle->flags |= UV_HANDLE_TCP_NODELAY;
  else
    handle->flags &= ~UV_HANDLE_TCP_NODELAY;

  return 0;
}

// 同上
int uv_tcp_keepalive(uv_tcp_t* handle, int on, unsigned int delay) {
  int err;

  if (uv__stream_fd(handle) != -1) {
    err =uv__tcp_keepalive(uv__stream_fd(handle), on, delay);
    if (err)
      return err;
  }

  if (on)
    handle->flags |= UV_HANDLE_TCP_KEEPALIVE;
  else
    handle->flags &= ~UV_HANDLE_TCP_KEEPALIVE;

  /* TODO Store delay if uv__stream_fd(handle) == -1 but don't want to enlarge
   *      uv_tcp_t with an int that's almost never used...
   */

  return 0;
}


int uv_tcp_simultaneous_accepts(uv_tcp_t* handle, int enable) {
  if (enable)
    handle->flags &= ~UV_HANDLE_TCP_SINGLE_ACCEPT;
  else
    handle->flags |= UV_HANDLE_TCP_SINGLE_ACCEPT;
  return 0;
}


void uv__tcp_close(uv_tcp_t* handle) {
  uv__stream_close((uv_stream_t*)handle);
}
