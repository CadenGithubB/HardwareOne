#include "System_BuildConfig.h"
#if ENABLE_CAMERA_SENSOR && defined(CONFIG_IDF_TARGET_ESP32P4)
#include "HAL_Camera_SCCB.h"
#include "esp_sccb_i2c.h"
extern "C" esp_err_t __real_sccb_new_i2c_io(
    i2c_master_bus_handle_t, const sccb_i2c_config_t*, esp_sccb_io_handle_t*);
#if CAMERA_SCCB_SHARED
#include "System_I2C.h"
#include "esp_sccb_io_interface.h"
#include <new>
#include <algorithm>
namespace {
i2c_master_bus_handle_t borrowed = nullptr;
I2CDeviceManager* owner = nullptr;
uint8_t managerBus = 0;
// The SDK serializes its sensor operations through the camera service/ISP.
// The manager additionally serializes every transfer with touch and audio.
struct ManagedSccb {
  esp_sccb_io_t base{};
  I2CDeviceManager* manager;
  I2CDevice* device;
  uint8_t bus, address;
  uint32_t clock;
};
esp_err_t transfer(esp_sccb_io_t* base, const uint8_t* tx, size_t txSize,
                   uint8_t* rx, size_t rxSize, int timeout) {
  if (!base || (!tx && txSize) || (!rx && rxSize) ||
      txSize > 128 || rxSize > 128 || (!txSize && !rxSize))
    return ESP_ERR_INVALID_ARG;
  auto* io = reinterpret_cast<ManagedSccb*>(base);
  esp_err_t result = ESP_ERR_TIMEOUT;
  const uint32_t wait = timeout > 0 ? std::min<uint32_t>(uint32_t(timeout), 500u) : 500u;
  const bool ok = io->device->transaction([&]() {
    TwoWire* wire = io->manager->getWire(io->bus);
    if (!wire) { result = ESP_ERR_INVALID_STATE; return false; }
    if (txSize) {
      wire->beginTransmission(io->address);
      if (wire->write(tx, txSize) != txSize) {
        wire->endTransmission();
        result = ESP_FAIL;
        return false;
      }
      const uint8_t status = wire->endTransmission(rxSize == 0);
      if (status) { result = status == 5 ? ESP_ERR_TIMEOUT : ESP_FAIL; return false; }
    }
    if (rxSize) {
      const size_t got = wire->requestFrom(io->address, rxSize, true);
      if (got != rxSize) {
        while (wire->available()) wire->read();
        result = ESP_FAIL;
        return false;
      }
      for (size_t i = 0; i < rxSize; ++i) rx[i] = wire->read();
    }
    result = ESP_OK;
    return true;
  }, I2CTransactionOptions{io->clock, wait}, I2CDevice::Mode::NACK_TOLERANT);
  return ok ? ESP_OK : result;
}
esp_err_t write(esp_sccb_io_t* io, const uint8_t* tx, size_t size, int timeout) {
  return transfer(io, tx, size, nullptr, 0, timeout);
}
esp_err_t receive(esp_sccb_io_t* io, uint8_t* rx, size_t size, int timeout) {
  return transfer(io, nullptr, 0, rx, size, timeout);
}
esp_err_t destroy(esp_sccb_io_t* io) {
  delete reinterpret_cast<ManagedSccb*>(io);
  return ESP_OK;
}
}  // namespace

bool cameraSharedSccbAcquire(uint8_t bus, int port, i2c_master_bus_handle_t* out) {
  if (!out || borrowed) return false;
  *out = nullptr;
  auto* manager = I2CDeviceManager::existingInstance();
  if (!manager || !manager->retainBus(bus)) return false;
  if (i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(port), out) != ESP_OK || !*out) {
    manager->releaseBus(bus);
    return false;
  }
  owner = manager;
  managerBus = bus;
  borrowed = *out;
  return true;
}
void cameraSharedSccbRelease() {
  if (owner) owner->releaseBus(managerBus);
  owner = nullptr;
  borrowed = nullptr;
}
#endif
// esp_video's public init API accepts an I2C handle but not an SCCB factory.
// Wrap only that factory boundary; dedicated P4X-EYE buses use the SDK unchanged.
// The interface is pinned by the checked esp_sccb_intf headers and host tests.
extern "C" esp_err_t __wrap_sccb_new_i2c_io(i2c_master_bus_handle_t bus,
    const sccb_i2c_config_t* config, esp_sccb_io_handle_t* out) {
#if CAMERA_SCCB_SHARED
  if (bus == borrowed && borrowed) {
    if (!config || !out || config->device_address > 0x7f) return ESP_ERR_INVALID_ARG;
    *out = nullptr;
    auto* device = owner->registerDevice(config->device_address, "Camera SCCB", managerBus);
    if (!device) return ESP_ERR_NO_MEM;
    auto* io = new(std::nothrow) ManagedSccb;
    if (!io) return ESP_ERR_NO_MEM;
    io->manager = owner;
    io->device = device;
    io->bus = managerBus;
    io->address = config->device_address;
    io->clock = config->scl_speed_hz;
    io->base.transmit_reg_a8v8=write;io->base.transmit_reg_a16v8=write;
    io->base.transmit_reg_a8v16=write;io->base.transmit_reg_a16v16=write;
    io->base.transmit_reg_a16v32=write;io->base.transmit_reg_a16=write;
    io->base.transmit_receive_reg_a8v8=transfer;io->base.transmit_receive_reg_a16v8=transfer;
    io->base.transmit_receive_reg_a8v16=transfer;io->base.transmit_receive_reg_a16v16=transfer;
    io->base.transmit_receive_reg_a16v32=transfer;io->base.transmit_receive_reg_a16=transfer;
    io->base.transmit_v16=write;io->base.receive_v16=receive;io->base.del=destroy;
    *out = &io->base;
    return ESP_OK;
  }
#endif
  return __real_sccb_new_i2c_io(bus, config, out);
}
#endif
