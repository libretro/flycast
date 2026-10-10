/* $Id: miniupnpc_socketdef.h,v 1.1 2018/03/13 23:44:10 nanard Exp $ */
/* Miniupnp project : http://miniupnp.free.fr/ or https://miniupnp.tuxfamily.org/
 * Author : Thomas Bernard
 * Copyright (c) 2018 Thomas Bernard
 * This software is subject to the conditions detailed in the
 * LICENCE file provided within this distribution */
#ifndef MINIUPNPC_SOCKETDEF_H_INCLUDED
#define MINIUPNPC_SOCKETDEF_H_INCLUDED

#ifdef _MSC_VER

#define ISINVALID(s) (INVALID_SOCKET==(s))

#else

#ifndef SOCKET
#define SOCKET int
#endif
#ifndef SSIZE_T
#define SSIZE_T ssize_t
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef ISINVALID
#define ISINVALID(s) ((s)<0)
#endif

#endif

/* A send on a connection the other end has closed raises SIGPIPE, and a
 * SIGPIPE nothing handles ends the whole process. It is never raised: by a
 * flag on each send where the system has one, by an option set on the socket
 * when it is made where it has that (Apple, the BSDs). */
#ifndef _WIN32
#include <sys/socket.h>
#endif
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
#define MINIUPNPC_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define MINIUPNPC_MSG_NOSIGNAL 0
#endif
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
#define MINIUPNPC_SET_NOSIGPIPE(s) do { int nosigpipe_ = 1; setsockopt((s), SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe_, sizeof(nosigpipe_)); } while(0)
#else
#define MINIUPNPC_SET_NOSIGPIPE(s) do { } while(0)
#endif

#ifdef _MSC_VER
#define MSC_CAST_INT (int)
#else
#define MSC_CAST_INT
#endif

/* definition of PRINT_SOCKET_ERROR */
#ifdef _WIN32
#define PRINT_SOCKET_ERROR(x)    fprintf(stderr, "Socket error: %s, %d\n", x, WSAGetLastError());
#else
#define PRINT_SOCKET_ERROR(x) perror(x)
#endif

#endif /* MINIUPNPC_SOCKETDEF_H_INCLUDED */
