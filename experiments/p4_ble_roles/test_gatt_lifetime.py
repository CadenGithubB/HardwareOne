"""Run actual terminal-retirement and tree-cleanup code with controlled drivers.

Extracts BLEDevice::deinitChecked, all three cleanup methods, factory ownership,
descriptor adoption and the real map iteration/removal methods. Stub objects
count lifetimes and reject destruction before terminal host/controller state.
This proves ownership/gating behavior, not hardware callback timing or heap size.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parent / "private/app/components/arduino/libraries/BLE/src"
COMPILER = shutil.which("c++")


def function(filename, signature):
    source = (SOURCE / filename).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


PREAMBLE = r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#define CONFIG_BLUEDROID_ENABLED 1
#define CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID 1
#define log_v(...) do {} while (0)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
using esp_err_t = int;
using esp_bluedroid_status_t = int;
constexpr int ESP_OK=0, ESP_ERR_NOT_SUPPORTED=2;
constexpr int ESP_BLUEDROID_STATUS_UNINITIALIZED=0;
constexpr int ESP_BLUEDROID_STATUS_INITIALIZED=1;
constexpr int ESP_BLUEDROID_STATUS_ENABLED=2;
constexpr int BT_HOSTED_CONTROLLER_IDLE=0;
int host=2, controller=2, failure=0;
int servicesAlive=0, charsAlive=0, descriptorsAlive=0, callbacksAlive=0;
int nextHandle=0;
esp_bluedroid_status_t esp_bluedroid_get_status() { return host; }
int esp_bluedroid_disable() { if (failure==1) return 99; host=1; return 0; }
int esp_bluedroid_deinit() { if (failure==2) return 99; host=0; return 0; }
int btHostedControllerStatus() { return controller; }
int btHostedControllerDisable() {
  assert(host==0);
  if (failure==3) return 99;
  controller=1; return 0;
}
int btHostedControllerDeinit() {
  assert(host==0);
  if (failure==4) return 99;
  controller=failure==5 ? -1 : 0; return 0;
}
struct BLELifecycleTransitionGuard {
  bool* flag;
  BLELifecycleTransitionGuard(int*, bool* p):flag(p) {}
  ~BLELifecycleTransitionGuard() { *flag=false; }
};
struct BLEClientRegistryGuard {};
struct Event { void give() {} void wait(const char*) {} };
struct BLEClient {
  bool m_deletionClaimed=false, m_callbackRetiring=false;
  Event m_routingDrainEvt, m_callbackDrainEvt;
};
struct BLEScan { bool stop() {return true;} };
struct BLEAdvertising { bool stop() {return true;} };
struct Callback {
  Callback() {++callbacksAlive;}
  ~Callback() {--callbacksAlive;}
};
struct BLEUUID { int value; BLEUUID(int n=0):value(n) {} };
struct BLEDescriptor {
  int handle=++nextHandle;
  bool m_ownedByCharacteristic=false;
  Callback* callback=nullptr;
  BLEDescriptor() {++descriptorsAlive;}
  virtual ~BLEDescriptor() { assert(host==0 && controller==0); --descriptorsAlive; }
  int getHandle() const {return handle;}
  BLEUUID getUUID() const {return BLEUUID(handle);}
};
struct DerivedDescriptor:BLEDescriptor {
  static int alive;
  DerivedDescriptor() {++alive;}
  ~DerivedDescriptor() override {--alive;}
};
int DerivedDescriptor::alive=0;
struct BLECharacteristic;
struct BLEService;
struct BLEDescriptorMap {
  std::map<BLEDescriptor*,std::string> m_uuidMap;
  std::map<uint16_t,BLEDescriptor*> m_handleMap;
  decltype(m_uuidMap)::iterator m_iterator;
  BLEDescriptor* getFirst();
  void removeDescriptor(BLEDescriptor*);
  void setByUUID(BLEUUID, BLEDescriptor* p) {
    m_uuidMap[p]="descriptor"; m_handleMap[p->getHandle()]=p;
  }
};
struct BLECharacteristicMap {
  std::map<BLECharacteristic*,std::string> m_uuidMap;
  std::map<uint16_t,BLECharacteristic*> m_handleMap;
  decltype(m_uuidMap)::iterator m_iterator;
  BLECharacteristic* getFirst();
  void removeCharacteristic(BLECharacteristic*);
};
struct BLEServiceMap {
  std::map<BLEService*,std::string> m_uuidMap;
  std::map<uint16_t,BLEService*> m_handleMap;
  decltype(m_uuidMap)::iterator m_iterator;
  BLEService* getFirst();
  void removeService(BLEService*);
};
struct BLECharacteristic {
  int handle=++nextHandle;
  bool m_ownedByService=false;
  BLEDescriptorMap m_descriptorMap;
  Callback* callback=nullptr;
  BLECharacteristic(BLEUUID={}, uint32_t=0) {++charsAlive;}
  ~BLECharacteristic() {assert(host==0 && controller==0); --charsAlive;}
  int getHandle() const {return handle;}
  void addDescriptor(BLEDescriptor*, bool takeOwnership=false);
  void destroyOwnedDescriptorsAfterHostDeinit();
};
struct BLEService {
  int handle=++nextHandle;
  BLECharacteristicMap m_characteristicMap;
  BLEService() {++servicesAlive;}
  ~BLEService() {assert(host==0 && controller==0); --servicesAlive;}
  int getHandle() const {return handle;}
  void addCharacteristic(BLECharacteristic* p) {
    m_characteristicMap.m_uuidMap[p]="characteristic";
    m_characteristicMap.m_handleMap[p->getHandle()]=p;
  }
  BLECharacteristic* createCharacteristic(BLEUUID,uint32_t);
  void destroyOwnedCharacteristicsAfterHostDeinit();
};
struct BLEServer {
  BLEServiceMap m_serviceMap;
  Callback* callback=nullptr;
  void destroyGattTreeAfterHostDeinit();
  void add(BLEService* p) {
    m_serviceMap.m_uuidMap[p]="service";
    m_serviceMap.m_handleMap[p->getHandle()]=p;
  }
};
int mux=0;
bool lifecycleTransitionInProgress=false, s_clientCreationBlocked=false;
std::set<BLEClient*> s_allClients;
struct Conn {void* peer_device;};
"""

DEVICE = r"""
struct BLEDevice {
  static bool initialized;
  static BLEServer* m_pServer;
  static BLEScan* m_pScan;
  static BLEAdvertising* m_bleAdvertising;
  static BLEClient* m_pClient;
  static std::map<int,Conn> m_connectedClientsMap;
  static BLEDeviceDeinitResult deinitChecked(bool);
};
bool BLEDevice::initialized=true;
BLEServer* BLEDevice::m_pServer=nullptr;
BLEScan* BLEDevice::m_pScan=nullptr;
BLEAdvertising* BLEDevice::m_bleAdvertising=nullptr;
BLEClient* BLEDevice::m_pClient=nullptr;
std::map<int,Conn> BLEDevice::m_connectedClientsMap;
"""

CHECKS = r"""
BLEServer* makeTree(Callback* callback) {
  auto* server=new BLEServer(); server->callback=callback;
  for (int s=0;s<3;++s) {
    auto* service=new BLEService(); server->add(service);
    for (int c=0;c<(s==2 ? 4 : 3);++c) {
      auto* chr=service->createCharacteristic(BLEUUID(c),0);
      chr->callback=callback;
      if (s==2 || (s==1 && c==1)) {
        auto* descriptor=new DerivedDescriptor(); descriptor->callback=callback;
        chr->addDescriptor(descriptor,true);
        // Duplicate registration aliases must not cause duplicate destruction.
        chr->addDescriptor(descriptor,true);
      }
    }
  }
  return server;
}
int main(int argc, char** argv) {
  assert(argc==2);
  const int scenario=std::atoi(argv[1]);
  Callback borrowedCallback;
  if (scenario<=6) {
    BLEDevice::m_pServer=makeTree(&borrowedCallback);
    assert(servicesAlive==3 && charsAlive==10 && descriptorsAlive==5);
    if (scenario==6) lifecycleTransitionInProgress=true;
    else failure=scenario;
    auto result=BLEDevice::deinitChecked(false);
    if (scenario) {
      assert(!result.success && !result.serverDeleted && BLEDevice::m_pServer);
      assert(servicesAlive==3 && charsAlive==10 && descriptorsAlive==5);
      failure=0; lifecycleTransitionInProgress=false;
      result=BLEDevice::deinitChecked(false);
    }
    assert(result.success && result.terminalState && result.serverDeleted);
    assert(!BLEDevice::m_pServer && !BLEDevice::initialized);
    assert(servicesAlive==0 && charsAlive==0 && descriptorsAlive==0);
    assert(DerivedDescriptor::alive==0 && callbacksAlive==1);
    assert(BLEDevice::deinitChecked(false).success); // idempotent retirement
  } else if (scenario==7) {
    // Runtime map removals transfer objects out of this server's terminal tree.
    BLEDevice::m_pServer=makeTree(&borrowedCallback);
    auto* removed=BLEDevice::m_pServer->m_serviceMap.getFirst();
    BLEDevice::m_pServer->m_serviceMap.removeService(removed);
    const int removedChars=int(removed->m_characteristicMap.m_uuidMap.size());
    assert(BLEDevice::deinitChecked(false).success);
    assert(servicesAlive==1 && charsAlive==removedChars);
    removed->destroyOwnedCharacteristicsAfterHostDeinit(); delete removed;
    assert(servicesAlive==0 && charsAlive==0 && descriptorsAlive==0);
  } else if (scenario==8) {
    // Public attachment APIs borrow unless the descriptor explicitly opts in.
    BLECharacteristic borrowedChar;
    BLEDescriptor borrowedDescriptor;
    BLEDevice::m_pServer=makeTree(&borrowedCallback);
    auto* service=BLEDevice::m_pServer->m_serviceMap.getFirst();
    auto* owned=service->m_characteristicMap.getFirst();
    service->addCharacteristic(&borrowedChar);
    owned->addDescriptor(&borrowedDescriptor);
    assert(BLEDevice::deinitChecked(false).success);
    assert(servicesAlive==0 && charsAlive==1 && descriptorsAlive==1);
    assert(!borrowedChar.m_ownedByService && !borrowedDescriptor.m_ownedByCharacteristic);
    assert(callbacksAlive==1);
  } else if (scenario==9) {
    for (int cycle=0;cycle<20;++cycle) {
      host=controller=2; BLEDevice::initialized=true;
      BLEDevice::m_pServer=makeTree(&borrowedCallback);
      assert(BLEDevice::deinitChecked(false).success);
      assert(servicesAlive==0 && charsAlive==0 && descriptorsAlive==0);
      assert(callbacksAlive==1);
    }
  } else { assert(false); }
}
"""


@unittest.skipUnless(COMPILER and (SOURCE / "BLEDevice.cpp").is_file(),
                     "Reconstructed BLE-role copy and C++17 compiler required")
class GattLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        header=(SOURCE / "BLEDevice.h").read_text()
        start=header.index("enum class BLEDeviceDeinitPhase")
        end=header.index("\n};", header.index("struct BLEDeviceDeinitResult",start))+3
        parts=[PREAMBLE,header[start:end],DEVICE]
        for kind,object_type,remove in (
            ("Service","BLEService","removeService"),
            ("Characteristic","BLECharacteristic","removeCharacteristic"),
            ("Descriptor","BLEDescriptor","removeDescriptor")):
            filename=f"BLE{kind}Map.cpp"
            parts.append(function(filename,f"{object_type} *BLE{kind}Map::getFirst()"))
            parts.append(function(filename,f"void BLE{kind}Map::{remove}("))
        for filename,signature in (
            ("BLECharacteristic.cpp","void BLECharacteristic::addDescriptor("),
            ("BLEService.cpp","BLECharacteristic *BLEService::createCharacteristic(BLEUUID"),
            ("BLECharacteristic.cpp","void BLECharacteristic::destroyOwnedDescriptorsAfterHostDeinit()"),
            ("BLEService.cpp","void BLEService::destroyOwnedCharacteristicsAfterHostDeinit()"),
            ("BLEServer.cpp","void BLEServer::destroyGattTreeAfterHostDeinit()"),
            ("BLEDevice.cpp","BLEDeviceDeinitResult BLEDevice::deinitChecked(")):
            parts.append(function(filename,signature))
        parts.append(CHECKS)
        cls.work=tempfile.TemporaryDirectory(prefix="hw1-gatt-lifetime-")
        cls.addClassCleanup(cls.work.cleanup)
        work=Path(cls.work.name)
        (work/"check.cpp").write_text("\n".join(parts))
        cls.binary=work/"check"
        result=subprocess.run([COMPILER,"-std=c++17","-Wall","-Wextra","-Werror",
                               "-Wno-unused-variable",str(work/"check.cpp"),"-o",str(cls.binary)],
                              capture_output=True,text=True,timeout=30)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)

    def scenario(self,n):
        result=subprocess.run([str(self.binary),str(n)],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_terminal_tree_retired_once(self): self.scenario(0)
    def test_host_disable_failure_retains_tree(self): self.scenario(1)
    def test_host_deinit_failure_retains_tree(self): self.scenario(2)
    def test_controller_disable_failure_retains_tree(self): self.scenario(3)
    def test_controller_deinit_failure_retains_tree(self): self.scenario(4)
    def test_unknown_controller_ack_retains_tree(self): self.scenario(5)
    def test_concurrent_transition_retains_tree(self): self.scenario(6)
    def test_removed_service_not_deleted_twice(self): self.scenario(7)
    def test_borrowed_objects_survive(self): self.scenario(8)
    def test_repeated_cycles_return_all_owned_objects(self): self.scenario(9)


if __name__=="__main__": unittest.main()
