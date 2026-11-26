// System_Net.cpp - the IP uplink (System_Net.h). The Wi-Fi station is the one
// uplink today; a second one is chosen here.
#include "System_Net.h"

#include "System_BuildConfig.h"

#if ENABLE_WIFI

#include <WiFi.h>

bool netUp() { return WiFi.isConnected(); }

String netLocalIpText() { return WiFi.localIP().toString(); }
String netGatewayIpText() { return WiFi.gatewayIP().toString(); }
String netDnsIpText() { return WiFi.dnsIP().toString(); }
String netSubnetMaskText() { return WiFi.subnetMask().toString(); }

const char* netHostname() { return WiFi.getHostname(); }

bool netResolveHost(const char* host, IPAddress& out) {
  return host && WiFi.hostByName(host, out);
}

#else  // !ENABLE_WIFI

bool netUp() { return false; }

String netLocalIpText() { return IPAddress().toString(); }
String netGatewayIpText() { return IPAddress().toString(); }
String netDnsIpText() { return IPAddress().toString(); }
String netSubnetMaskText() { return IPAddress().toString(); }

const char* netHostname() { return ""; }

bool netResolveHost(const char*, IPAddress&) { return false; }

#endif  // ENABLE_WIFI
