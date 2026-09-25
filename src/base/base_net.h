// A single TCP connect helper - just enough for the control-connection
// thread to get a raw socket to hand to libssh2 (libssh2 does its own
// send/recv on the fd once the handshake starts; we only own connect and
// close). Implemented per-OS (see os_core_win32.c).

#ifndef BASE_NET_H
#define BASE_NET_H

//- @per_os_impl
internal B32  net_tcp_connect(String8 host, U16 port, U64 *out_socket);
internal void net_close(U64 socket);

#endif // BASE_NET_H
