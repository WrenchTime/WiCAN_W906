/*
 * Stand-in for <sys/socket.h> as ESP-IDF v5.5.2 has it (components/lwip/port/esp32xx/include/sys/socket.h,
 * which is lwIP): the one call display/main/web.c makes on a connection. setsockopt() is an inline function
 * there (components/lwip/include/lwip/sockets.h) that calls lwip_setsockopt(); the two numbers are those of
 * lwip/sockets.h in the lwIP of that version (espressif/esp-lwip, commit fd432e4e).
 *
 * This file takes the place of the header of the PC for the simulations: they open no connection.
 *
 * One of them needs the header of the PC all the same: gx_sim.c runs display/main/gx.c, which makes every
 * socket call there is, and plays those calls itself under other names - with the types and the numbers of
 * the PC, which are as good as those of lwIP for what gx.c does with them. It says so with SIM_PC_SOCKETS
 * before its first #include, and this file then steps aside for the next <sys/socket.h> on the path
 * (#include_next: gcc and clang both have it). The headers of the PC that include <sys/socket.h> themselves,
 * <netinet/in.h> and <arpa/inet.h>, come by here as well.
 */
#ifdef SIM_PC_SOCKETS
#include_next <sys/socket.h>
#else
#ifndef __SIM_SYS_SOCKET_H__
#define __SIM_SYS_SOCKET_H__

#include <stdint.h>

#define IPPROTO_TCP     6
#define TCP_NODELAY     0x01

int lwip_setsockopt(int s, int level, int optname, const void *optval, uint32_t optlen);

static inline int setsockopt(int s, int level, int optname, const void *opval, uint32_t optlen)
{
	return lwip_setsockopt(s, level, optname, opval, optlen);
}

#endif
#endif
