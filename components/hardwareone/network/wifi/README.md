# Saved Wi-Fi networks

This directory owns the saved-network list: validation, ordering, entry IDs,
snapshots and settings serialization. Radio connections, shared persistence
policy and screen rendering retain their existing owners.

| File | Responsibility |
|---|---|
| [System_WiFiNetworks.h](System_WiFiNetworks.h) | Public operations, credential-free presentation snapshots, settings hooks and disabled-feature behavior |
| [System_WiFiNetworks.cpp](System_WiFiNetworks.cpp) | Private list, mutex, IDs, mutations and serialization implementation |
| [System_WiFiNetworksInternal.h](System_WiFiNetworksInternal.h) | Credential-bearing copies for the radio adapter; not a UI interface |

External callers include `network/wifi/System_WiFiNetworks.h` from the existing
component include root. The radio adapter alone uses the internal interface.
Files inside this directory can include their neighboring headers directly.

## Where to make a change

| Concern | Entry point and integration |
|---|---|
| Saved entry validation, removal or priority | `upsertWiFiNetwork`, `removeWiFiNetworkById`, `setWiFiNetworkPriority` and sorting in the implementation here |
| List presentation | `wifiNetworkSnapshot`; [OLED](../../OLED_Mode_Network.cpp) and [G2](../../G2_Page_Network.cpp) present copies and submit authorized commands |
| Connect to a saved network | [System_WiFi.cpp](../../System_WiFi.cpp) selects a `WifiConnectionTarget`, performs the radio operation and reports connection success through `wifiNetworkMarkConnected` |
| Settings load/save | `wifiNetworksReadJson` / `wifiNetworksWriteJson`; [System_Settings.cpp](../../System_Settings.cpp) retains crypto, load ordering and file-write policy |
| Setup acceptance | `saveSetupWiFiNetwork`; [System_SetupWizard.cpp](../../System_SetupWizard.cpp) and [System_FirstTimeSetup.cpp](../../System_FirstTimeSetup.cpp) handle prompts and outcomes |
| Boot storage allocation | `initWiFiNetworks`, called by [HardwareOne.cpp](../../HardwareOne.cpp) before settings load |

Changing a field starts with the private record and operations here, then the
relevant snapshot or settings hook. Update a UI only when its presentation
changes. Caller copies remain valid independently of later list mutations;
the list mutex is never held during radio waits, persistence or callbacks.

## Build and verification

[CMakeLists.txt](../../CMakeLists.txt) lists the implementation explicitly.
`ENABLE_WIFI` continues to control its body and the public disabled stubs.
This is a source directory within the existing IDF component.

The existing [host suite](../../test/host/README.md) keeps its shared harness:

- `wifi_networks` executes the owner, serialization and disabled interface.
- `wifi_network_consumers` checks settings, setup and command integration.
- `wifi_saved_ui` exercises saved-network selection and display behavior.
- `setup_completion` checks setup result propagation.
- `network_build_selection` verifies source registration in both IDF discovery
  and configured phases, including Wi-Fi-disabled selections.

From the repository root, use an existing configured host build:

```sh
ctest --test-dir /tmp/hw1-hardwareone-host \
  -R '^(wifi_networks|wifi_network_consumers|wifi_saved_ui|setup_completion|network_build_selection)$' \
  --output-on-failure
```
