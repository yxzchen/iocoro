#pragma once

#include <xz/io/impl/assert.ipp>
#include <xz/io/impl/error.ipp>
#include <xz/io/impl/executor.ipp>
#include <xz/io/impl/io_context.ipp>
#include <xz/io/impl/io_context_impl.ipp>
#ifndef IOCORO_HAS_URING
#include <xz/io/impl/backends/epoll.ipp>
#endif
#include <xz/io/impl/ip.ipp>
#include <xz/io/impl/resolver.ipp>
#include <xz/io/impl/tcp_acceptor.ipp>
#include <xz/io/impl/tcp_socket.ipp>
#include <xz/io/impl/timer_handle.ipp>
#include <xz/io/impl/udp_socket.ipp>
