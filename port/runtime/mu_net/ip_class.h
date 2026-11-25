// Which kind of IPv4 address a candidate is (loopback, private, carrier-shared, public), for the
// peer connector's choice of which candidates get neighbouring-port guesses.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace mu_net {

// An IPv4 address here is the number a.b.c.d reads as: (a << 24) | (b << 16) | (c << 8) | d.
inline bool ip_is_loopback(uint32_t ip) { return (ip >> 24) == 127; }
// 10/8, 172.16/12, 192.168/16 and the link-local 169.254/16.
inline bool ip_is_private(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
}
// 100.64/10: what a provider gives a customer who shares a public address with others (carrier-grade NAT).
inline bool ip_is_shared(uint32_t ip) { return (ip >> 22) == 0x191; }
// Something a datagram can be sent to: not 0.x, not multicast, not the reserved top block.
inline bool ip_is_usable(uint32_t ip) { return (ip >> 24) != 0 && (ip >> 28) < 0xE; }
inline bool ip_is_public(uint32_t ip) {
  return ip_is_usable(ip) && !ip_is_loopback(ip) && !ip_is_private(ip) && !ip_is_shared(ip);
}

}  // namespace mu_net
