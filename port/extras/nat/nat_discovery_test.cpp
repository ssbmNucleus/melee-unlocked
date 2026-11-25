// Tests of nat_discovery (STUN, NAT-PMP, UPnP and the candidate list), taken out of the game's
// port/tests/mu_net_test.cpp when the old launcher's lobby, their only user, was removed.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "nat_discovery.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mu_net;

static int g_failures = 0;
#define EXPECT(condition) \
  do { if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++g_failures; } } while (0)

static void test_stun_codec() {
  // The request: Binding, no attributes, the magic cookie, the transaction id.
  StunId id;
  for (size_t i = 0; i < id.size(); ++i) id[i] = (uint8_t)i;
  uint8_t request[kStunRequestSize];
  stun_write_request(id, request);
  const uint8_t expected_request[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  EXPECT(std::memcmp(request, expected_request, sizeof request) == 0);
  EXPECT(stun_looks_like(request, sizeof request));

  // RFC 5769, section 2.2: the sample IPv4 response. It maps 192.0.2.1 port 32853. The integrity
  // and fingerprint attributes are walked over, not checked.
  const uint8_t sample[80] = {
      0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
      0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
      0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
      0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7,
      0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
  const StunId sample_id = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
  NatEndpoint mapped;
  EXPECT(stun_read_response(sample, sizeof sample, sample_id, mapped) == StunResult::Mapped);
  EXPECT(mapped.ip == 0xC0000201u && mapped.port == 32853);
  EXPECT(endpoint_text(mapped) == "192.0.2.1:32853");
  // Someone else's answer, a cut datagram, a datagram with a byte too many.
  EXPECT(stun_read_response(sample, sizeof sample, id, mapped) == StunResult::OtherTransaction);
  EXPECT(stun_read_response(sample, sizeof sample - 1, sample_id, mapped) == StunResult::NotStun);
  uint8_t longer[81];
  std::memcpy(longer, sample, sizeof sample);
  longer[80] = 0;
  EXPECT(stun_read_response(longer, sizeof longer, sample_id, mapped) == StunResult::NotStun);

  // The plain MAPPED-ADDRESS of an old server: 198.51.100.7 port 40000.
  uint8_t plain[32] = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xA4, 0x42, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                       0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x9C, 0x40, 198, 51, 100, 7};
  EXPECT(stun_read_response(plain, sizeof plain, id, mapped) == StunResult::Mapped);
  EXPECT(mapped.ip == 0xC6336407u && mapped.port == 40000);
  // The XOR form wins when both are there: the same address written both ways, the plain one wrong.
  uint8_t both[44];
  std::memcpy(both, plain, 32);
  both[3] = 0x18;
  both[28] = 10;   // the plain one now says 10.51.100.7
  const uint8_t xor_attr[12] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0x9C ^ 0x21, 0x40 ^ 0x12, 198 ^ 0x21, 51 ^ 0x12, 100 ^ 0xA4, 7 ^ 0x42};
  std::memcpy(both + 32, xor_attr, 12);
  EXPECT(stun_read_response(both, sizeof both, id, mapped) == StunResult::Mapped && mapped.ip == 0xC6336407u && mapped.port == 40000);

  // An attribute that says it is longer than the datagram.
  uint8_t overrun[32];
  std::memcpy(overrun, plain, 32);
  overrun[23] = 0x10;
  EXPECT(stun_read_response(overrun, sizeof overrun, id, mapped) == StunResult::Malformed);
  // An IPv6 address only, a Binding error, a request echoed back.
  uint8_t v6[32];
  std::memcpy(v6, plain, 32);
  v6[25] = 0x02;
  EXPECT(stun_read_response(v6, sizeof v6, id, mapped) == StunResult::NoAddress);
  uint8_t error[20];
  std::memcpy(error, request, 20);
  error[0] = 0x01; error[1] = 0x11;
  EXPECT(stun_read_response(error, sizeof error, id, mapped) == StunResult::NoAddress);
  EXPECT(stun_read_response(request, sizeof request, id, mapped) == StunResult::Malformed);
  // A mapped address nothing can be sent to (0.0.0.0, or port 0).
  uint8_t zero[32];
  std::memcpy(zero, plain, 32);
  zero[28] = zero[29] = zero[30] = zero[31] = 0;
  EXPECT(stun_read_response(zero, sizeof zero, id, mapped) == StunResult::NoAddress);
  // Not STUN at all: a lobby packet, a DHT packet, nothing.
  const char lobby[] = "MUL1{\"t\":\"hello\",\"pad\":0}";
  const char dht[] = "d1:ad2:id20:abcdefghij0123456789e1:q4:ping1:t2:aa1:y1:qe";
  EXPECT(!stun_looks_like(reinterpret_cast<const uint8_t*>(lobby), sizeof lobby - 1));
  EXPECT(!stun_looks_like(reinterpret_cast<const uint8_t*>(dht), sizeof dht - 1));
  EXPECT(!stun_looks_like(nullptr, 0) && !stun_looks_like(request, 19));

  // A round of two requests on a socket the caller owns.
  StunProbe probe;
  EXPECT(probe.request(0) == nullptr && probe.answers() == 0 && probe.mapping() == NatMapping::Unknown);
  EXPECT(probe.begin(2) && probe.servers() == 2);
  EXPECT(probe.request(0) && probe.request(1) && !probe.request(2));
  EXPECT(std::memcmp(probe.request(0) + 8, probe.request(1) + 8, 12) != 0);   // two random ids
  auto answer_for = [&](int server, uint16_t port) {
    std::vector<uint8_t> a(plain, plain + 32);
    std::memcpy(a.data() + 8, probe.request(server) + 8, 12);
    a[26] = (uint8_t)(port >> 8);
    a[27] = (uint8_t)port;
    return a;
  };
  EXPECT(!probe.on_datagram(reinterpret_cast<const uint8_t*>(lobby), sizeof lobby - 1));   // left for the lobby
  EXPECT(probe.on_datagram(sample, sizeof sample) && probe.answers() == 0);                // STUN, but nobody's answer
  std::vector<uint8_t> a0 = answer_for(0, 40000), a1 = answer_for(1, 40000);
  EXPECT(probe.on_datagram(a0.data(), a0.size()) && probe.answers() == 1 && probe.mapping() == NatMapping::Unknown);
  EXPECT(probe.mapped(mapped) && mapped.port == 40000);
  EXPECT(probe.on_datagram(a1.data(), a1.size()) && probe.answers() == 2 && probe.mapping() == NatMapping::Stable);
  // A second answer to the same request changes nothing.
  std::vector<uint8_t> late = answer_for(1, 40777);
  EXPECT(probe.on_datagram(late.data(), late.size()) && probe.mapping() == NatMapping::Stable);
  // A router that gives each destination its own port.
  EXPECT(probe.begin(2));
  a0 = answer_for(0, 40000);
  a1 = answer_for(1, 40001);
  EXPECT(probe.on_datagram(a0.data(), a0.size()) && probe.on_datagram(a1.data(), a1.size()));
  EXPECT(probe.mapping() == NatMapping::PerDestination);

  // The server list: the default, the player's own, none.
  EXPECT(stun_server_list(nullptr).size() == 3 && stun_server_list("  ").size() == 3);
  EXPECT(stun_server_list("off").empty() && stun_server_list("NONE").empty());
  const std::vector<std::string> own = stun_server_list("a.example:1, b.example ,bad host,c.example:99999,d.example:x, e.example,b.example:3478");
  EXPECT(own.size() == 3 && own[0] == "a.example:1" && own[1] == "b.example:3478" && own[2] == "e.example:3478");
  EXPECT(stun_server_list("a:1,b:1,c:1,d:1,e:1,f:1").size() == (size_t)kMaxStunServers);
}

static void test_nat_addresses_and_mapping_text() {
  uint32_t ip = 0;
  NatEndpoint e;
  EXPECT(parse_ip("192.168.1.20", ip) && ip == 0xC0A80114u && ip_text(ip) == "192.168.1.20");
  EXPECT(!parse_ip("192.168.1", ip) && !parse_ip("192.168.1.256", ip) && !parse_ip("a.b.c.d", ip) && !parse_ip("1.2.3.4.5", ip) && !parse_ip("", ip));
  EXPECT(parse_endpoint("10.0.0.5:41000", e) && e.ip == 0x0A000005u && e.port == 41000);
  EXPECT(!parse_endpoint("10.0.0.5", e) && !parse_endpoint("10.0.0.5:0", e) && !parse_endpoint("10.0.0.5:65536", e) &&
         !parse_endpoint("example.invalid:1", e) && !parse_endpoint("10.0.0.5:12x", e));
  EXPECT(ip_is_private(0x0A000005u) && ip_is_private(0xAC100001u) && ip_is_private(0xAC1FFFFFu) && !ip_is_private(0xAC200001u));
  EXPECT(ip_is_private(0xC0A80101u) && ip_is_private(0xA9FE0101u) && ip_is_loopback(0x7F000001u));
  EXPECT(ip_is_shared(0x64400001u) && ip_is_shared(0x647FFFFFu) && !ip_is_shared(0x64800001u) && !ip_is_shared(0x643F0001u));
  EXPECT(ip_is_public(0xC6336407u) && !ip_is_public(0x64400001u) && !ip_is_public(0x7F000001u) && !ip_is_public(0xC0A80101u));
  EXPECT(!ip_is_public(0) && !ip_is_public(0xE0000001u) && !ip_is_public(0xFFFFFFFFu));

  // NAT-PMP: map UDP 40000 for an hour, and the router's answer giving 40001.
  uint8_t request[kNatPmpMapRequestSize];
  natpmp_write_map(40000, 40000, 3600, request);
  const uint8_t expected_request[12] = {0, 1, 0, 0, 0x9C, 0x40, 0x9C, 0x40, 0, 0, 0x0E, 0x10};
  EXPECT(std::memcmp(request, expected_request, sizeof request) == 0);
  uint8_t answer[16] = {0, 129, 0, 0, 0, 0, 0x12, 0x34, 0x9C, 0x40, 0x9C, 0x41, 0, 0, 0x0E, 0x10};
  uint16_t external = 0;
  uint32_t lease = 0;
  EXPECT(natpmp_read_map(answer, sizeof answer, 40000, external, lease) && external == 40001 && lease == 3600);
  EXPECT(!natpmp_read_map(answer, sizeof answer, 40002, external, lease));   // an answer about another port
  EXPECT(!natpmp_read_map(answer, 15, 40000, external, lease));
  answer[3] = 2;   // result code: refused
  EXPECT(!natpmp_read_map(answer, sizeof answer, 40000, external, lease));
  const uint8_t address[12] = {0, 128, 0, 0, 0, 0, 0x12, 0x34, 192, 0, 2, 33};
  EXPECT(natpmp_read_address(address, sizeof address, ip) && ip == 0xC0000221u);
  EXPECT(!natpmp_read_address(address, 11, ip) && !natpmp_read_address(answer, sizeof answer, ip));

  // UPnP: the search answer, the description, the control call.
  std::string url, path, type, control, body;
  EXPECT(ssdp_location("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                       "Location:  http://192.168.1.1:5000/rootDesc.xml \r\nSERVER: x\r\n\r\n", url));
  EXPECT(url == "http://192.168.1.1:5000/rootDesc.xml");
  EXPECT(!ssdp_location("HTTP/1.1 200 OK\r\nST: x\r\n\r\n", url));
  EXPECT(http_url(url, e, path) && e.ip == 0xC0A80101u && e.port == 5000 && path == "/rootDesc.xml");
  EXPECT(http_url("HTTP://192.168.1.1", e, path) && e.port == 80 && path == "/");
  EXPECT(!http_url("http://router.local/desc.xml", e, path) && !http_url("https://192.168.1.1/desc.xml", e, path) &&
         !http_url("http://192.168.1.1/a b", e, path) && !http_url("http://0.0.0.0/x", e, path));
  int status = 0;
  EXPECT(http_split("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", status, body) && status == 200 && body == "hello");
  EXPECT(http_split("HTTP/1.1 500 Internal Server Error\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n4;x=y\r\ndefg\r\n0\r\n\r\n", status, body) &&
         status == 500 && body == "abcdefg");
  EXPECT(!http_split("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", status, body) && !http_split("garbage\r\n\r\n", status, body));
  const std::string description =
      "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>"
      "<controlURL>/ctl/CmnIfCfg</controlURL></service></serviceList><deviceList><device><serviceList>"
      "<service>\r\n<serviceType> urn:schemas-upnp-org:service:WANPPPConnection:1 </serviceType><controlURL>/ctl/PPPConn</controlURL></service>"
      "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/IPConn</controlURL>"
      "<eventSubURL>/evt/IPConn</eventSubURL></service></serviceList></device></deviceList></device></root>";
  EXPECT(upnp_find_control(description, type, control) && type == "urn:schemas-upnp-org:service:WANIPConnection:1" && control == "/ctl/IPConn");
  EXPECT(!upnp_find_control("<root><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
                            "<controlURL>/x</controlURL></service></root>", type, control));
  EXPECT(xml_value("<a><errorCode> 725 </errorCode></a>", "errorCode") == "725" && xml_value("<a></a>", "errorCode").empty());
  const std::string soap = upnp_soap(type, "AddPortMapping", {{"NewExternalPort", "40000"}, {"NewPortMappingDescription", "a<b&c"}});
  EXPECT(soap.find("<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">") != std::string::npos);
  EXPECT(soap.find("<NewExternalPort>40000</NewExternalPort>") != std::string::npos);
  EXPECT(soap.find("<NewPortMappingDescription>a&lt;b&amp;c</NewPortMappingDescription></u:AddPortMapping>") != std::string::npos);

  // The mapper does nothing until asked, and stopping it when idle returns at once.
  PortMapper mapper;
  EXPECT(mapper.status().state == MapState::Idle);
  mapper.request(0);
  mapper.stop();
  EXPECT(mapper.status().state == MapState::Idle);
}

static void test_candidate_order() {
  // What a launcher adds to its half of the setup.
  PortMapping mapped;
  mapped.state = MapState::Mapped;
  mapped.external.ip = 0xC6336407u;   // 198.51.100.7
  mapped.external.port = 41001;
  NatEndpoint stun;
  stun.ip = 0xC6336407u;
  stun.port = 52000;
  std::vector<std::string> extra = extra_candidates(mapped, true, stun, 0);
  EXPECT(extra.size() == 2 && extra[0] == "198.51.100.7:41001" && extra[1] == "198.51.100.7:52000");
  // A router behind another router: its mapping is not reachable from outside, the STUN answer may be.
  mapped.external.ip = 0x64400009u;   // 100.64.0.9
  extra = extra_candidates(mapped, true, stun, 0);
  EXPECT(extra.size() == 1 && extra[0] == "198.51.100.7:52000");
  mapped.external.ip = 0xC0A80002u;   // 192.168.0.2
  EXPECT(extra_candidates(mapped, false, stun, 0xC6336407u).empty());
  // A router that mapped the port but did not say its address: the STUN address, else the one another launcher saw.
  mapped.external.ip = 0;
  extra = extra_candidates(mapped, false, stun, 0xCB007109u);   // 203.0.113.9
  EXPECT(extra.size() == 1 && extra[0] == "203.0.113.9:41001");
  EXPECT(extra_candidates(mapped, false, stun, 0).empty());
  extra = extra_candidates(mapped, true, stun, 0xCB007109u);
  EXPECT(extra.size() == 2 && extra[0] == "198.51.100.7:41001");
  // No mapping, and a STUN answer that is itself a carrier-shared address: nothing worth naming.
  stun.ip = 0x64400009u;
  EXPECT(extra_candidates(PortMapping(), true, stun, 0).empty());
  // The same port mapped and seen by STUN is named once.
  stun.ip = 0xC6336407u;
  stun.port = 41001;
  mapped.external.ip = 0xC6336407u;
  EXPECT(extra_candidates(mapped, true, stun, 0).size() == 1);

  // What the other launcher dials: the address it hears us from, the public extras, the LAN addresses.
  const std::vector<std::string> order = order_candidates(
      "203.0.113.9:41000",
      {"203.0.113.9:41000", "198.51.100.4:50000", "192.168.1.9:1", "198.51.100.5:1", "198.51.100.6:1"},
      {"192.168.1.20:41000", "10.0.0.5:41000", "not an address", "192.168.56.1:41000", "192.168.57.1:41000"});
  EXPECT(order.size() == 6);
  if (order.size() == 6) {
    EXPECT(order[0] == "203.0.113.9:41000" && order[1] == "198.51.100.4:50000" && order[2] == "198.51.100.5:1");
    EXPECT(order[3] == "192.168.1.20:41000" && order[4] == "10.0.0.5:41000" && order[5] == "192.168.56.1:41000");
  }
  // An older launcher names no extras: the list is what it always was.
  const std::vector<std::string> old = order_candidates("127.0.0.1:41000", {}, {"192.168.1.20:41000", "127.0.0.1:41000"});
  EXPECT(old.size() == 2 && old[0] == "127.0.0.1:41000" && old[1] == "192.168.1.20:41000");
  // Nothing usable in, nothing out; and never more than asked for.
  EXPECT(order_candidates("", {"0.0.0.0:5", "224.0.0.1:5", "1.2.3.4:0"}, {"x"}).empty());
  EXPECT(order_candidates("203.0.113.9:41000", {"198.51.100.4:50000"}, {"192.168.1.20:41000"}, 2).size() == 2);
}

int main() {
  test_stun_codec();
  test_nat_addresses_and_mapping_text();
  test_candidate_order();
  if (g_failures) { std::printf("nat_discovery_test: %d check(s) failed\n", g_failures); return 1; }
  std::printf("nat_discovery_test: all checks passed\n");
  return 0;
}
