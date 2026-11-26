// System_Net.h - the device's IP uplink, whichever interface carries it.
//
// Code that needs the network asks here and never names the interface:
//   "can I reach the network, and at what address?"   netUp(), net*Text(),
//                                                     netHostname(), netResolveHost()
// Code that shows or acts on the Wi-Fi station itself (its name, signal,
// channel, MAC) asks System_WiFi.h: wifiStaJoined(), wifiStaSsid(), ...
//
// Today the Wi-Fi station is the only uplink, so netUp() is wifiStaJoined()
// and the addresses are the station's. A second uplink (Ethernet on a P4
// board, a tethered link) becomes a change in System_Net.cpp; no caller
// changes. That is why a service (NTP, MQTT, the web server, an HTTP client)
// calls netUp(), and a Wi-Fi screen or the ESP-NOW channel logic does not.
//
// A build without Wi-Fi (ENABLE_WIFI 0) answers as a link that is never up:
// false, "0.0.0.0", "". Callers need no build condition and no <WiFi.h>.
//
// Threading: safe from any task. None of these blocks on the radio companion
// (the P4's station name and signal do: see wifiStaSsid / wifiStaRssi).
#ifndef SYSTEM_NET_H
#define SYSTEM_NET_H

#include <Arduino.h>
#include <IPAddress.h>

// The uplink has an address: a service may open a socket.
bool netUp();

// The uplink's addresses as dotted text; "0.0.0.0" while it is down.
String netLocalIpText();
String netGatewayIpText();
String netDnsIpText();
String netSubnetMaskText();

// The name this device announces on the network; "" in a build without one.
const char* netHostname();

// Resolve a host name (or parse a literal address) through the uplink's DNS.
bool netResolveHost(const char* host, IPAddress& out);

#endif  // SYSTEM_NET_H
