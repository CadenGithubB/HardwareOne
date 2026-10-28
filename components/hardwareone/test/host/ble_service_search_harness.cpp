// Runs the vendored BLE library's service discovery as it ships: the Bluedroid
// BLEClient::getServices() and FreeRTOS::Semaphore (take/timedWait/give) from
// components/arduino/libraries/BLE/src, over a fake kernel semaphore that
// reports a wait that could never end instead of hanging, and a scripted
// esp_ble_gattc_search_service.
//
// Cases:
//   ring_incident  the 2026-10-25 R1 sequence: a search that never completes
//                  (bounded timeout), the link's one DISCONNECT_EVT give, a
//                  second search, then a third whose request is refused on the
//                  dead link. Every call must return; none may wait forever
//                  (the old code blocked in an unbounded take on the third).
//   disconnected   a client the DISCONNECT_EVT marked down is not searched.
//   fast_complete  a SEARCH_CMPL_EVT delivered before the wait is not lost.
//   failed_status  a completion carrying an error status is not "have services".
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

// ---- fake FreeRTOS kernel ---------------------------------------------------
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdTRUE 1
#define pdFALSE 0
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))

struct FakeSemaphore { int count = 0; };
typedef FakeSemaphore* SemaphoreHandle_t;
static int gForeverWaits = 0;     // takes that a real kernel would never return from
static uint32_t gClockMs = 0;
static FakeSemaphore* gLastTakenSem = nullptr;
static int gTimedWaits = 0;

SemaphoreHandle_t xSemaphoreCreateBinary() { return new FakeSemaphore(); }   // created empty
void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->count = 1; return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t*) { s->count = 1; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  gLastTakenSem = s;
  if (s->count) { s->count = 0; return pdTRUE; }
  if (ticks == portMAX_DELAY) {   // nothing in this single-threaded run will ever give it
    ++gForeverWaits;
    return pdFALSE;
  }
  if (ticks) { ++gTimedWaits; gClockMs += ticks; }
  return pdFALSE;
}

#define log_v(...) do { } while (0)
#define log_e(...) do { } while (0)
#define log_i(...) do { } while (0)
using String = std::string;

class FreeRTOS {
 public:
  // INSERT_SEMAPHORE_CLASS
};

// INSERT_SEMAPHORE_DEFINITIONS

// ---- scripted Bluedroid client -----------------------------------------------
typedef int esp_err_t;
typedef uint8_t esp_gatt_if_t;
#define ESP_OK 0
#define ESP_FAIL -1
struct esp_bt_uuid_t;
class BLERemoteService;
class BLEClient;

static esp_err_t gSearchResult = ESP_OK;  // what the next request returns
static int gSearchCompletesWith = -1;     // >=0: SEARCH_CMPL_EVT arrives during the request
static int gSearchRequests = 0;
static BLEClient* gClient = nullptr;

class BLEClient {
 public:
  std::map<std::string, BLERemoteService*>* getServices();
  bool isConnected() { return m_isConnected; }
  esp_gatt_if_t getGattcIf() { return 4; }
  uint16_t getConnId() { return m_conn_id; }
  void clearServices() { m_servicesMap.clear(); }
  // What ESP_GATTC_DISCONNECT_EVT does to the search waiter (BLEClient.cpp).
  void disconnectEvent() {
    m_isConnected = false;
    m_semaphoreSearchCmplEvt.give(1);
    m_conn_id = 0xff;
  }
  void searchCompleteEvent(int status) { m_semaphoreSearchCmplEvt.give(status); }

  FreeRTOS::Semaphore m_semaphoreSearchCmplEvt{"SearchCmplEvt"};
  std::map<std::string, BLERemoteService*> m_servicesMap;
  bool m_haveServices = false;
  bool m_isConnected = true;
  uint16_t m_conn_id = 3;
};

esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t, uint16_t, esp_bt_uuid_t*) {
  ++gSearchRequests;
  if (gSearchResult == ESP_OK && gSearchCompletesWith >= 0 && gClient)
    gClient->searchCompleteEvent(gSearchCompletesWith);
  return gSearchResult;
}

// INSERT_GETSERVICES

static void reset(BLEClient& client) {
  gClient = &client;
  gForeverWaits = gTimedWaits = gSearchRequests = 0;
  gSearchResult = ESP_OK;
  gSearchCompletesWith = -1;
}

int main() {
  {  // ring_incident
    BLEClient client;
    reset(client);
    client.getServices();              // MTU helper's discovery: never completes
    assert(!client.m_haveServices && gTimedWaits == 1);
    client.disconnectEvent();          // the ring leaves range
    client.m_isConnected = true;       // a racing caller still sees the link as up
    client.getServices();              // getService()'s discovery: never completes
    client.m_isConnected = false;
    gSearchResult = ESP_FAIL;          // the debug listing's search: link is gone
    client.m_isConnected = true;       // even if a stale flag let it reach the request
    client.getServices();
    std::printf("  ring_incident: %d search request(s), %d bounded wait(s), %d endless\n",
                gSearchRequests, gTimedWaits, gForeverWaits);
    assert(gForeverWaits == 0);        // the 2026-10-25 wedge
    assert(!client.m_haveServices);
  }

  {  // disconnected
    BLEClient client;
    reset(client);
    client.getServices();              // times out, semaphore left taken
    client.disconnectEvent();
    const int requests = gSearchRequests;
    client.getServices();
    client.getServices();
    assert(gSearchRequests == requests);   // no request on a link that is down
    assert(gForeverWaits == 0);
  }

  {  // fast_complete
    BLEClient client;
    reset(client);
    gSearchCompletesWith = 0;
    client.getServices();
    assert(client.m_haveServices);     // the completion reached the wait
    assert(gTimedWaits == 0 && gForeverWaits == 0);
    client.getServices();              // and again on the same client
    assert(client.m_haveServices && gForeverWaits == 0);
  }

  {  // failed_status
    BLEClient client;
    reset(client);
    gSearchCompletesWith = 0x85;       // GATT error delivered as the completion
    client.getServices();
    assert(!client.m_haveServices && gForeverWaits == 0);
  }

  std::puts("BLE service search checks passed");
  return 0;
}
