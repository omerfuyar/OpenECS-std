// The configuration of nghttp2 for libcurl's HTTP/2 in the web plugin, in place of the one nghttp2's configure script or CMake would write.
// It is for Linux with glibc, and for Windows with MinGW.
#pragma once

#if defined(_WIN32)
#define HAVE_WINDOWS_H 1
#define HAVE_GETTICKCOUNT64 1
#else
#define HAVE_ARPA_INET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DECL_CLOCK_MONOTONIC 1
#endif
