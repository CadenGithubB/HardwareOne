/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "esp_now_hosted_rpc.h"

static void request_tests(void)
{
    uint8_t bytes[sizeof(esp_now_hosted_req_t) + ESP_NOW_HOSTED_MAX_PAYLOAD + 1] = {0};
    esp_now_hosted_req_t request = {
        .magic = ESP_NOW_HOSTED_MAGIC,
        .version = ESP_NOW_HOSTED_WIRE_VERSION,
        .opcode = ESP_NOW_HOSTED_OP_SEND,
        .payload_len = ESP_NOW_HOSTED_MAX_PAYLOAD,
        .seq = UINT64_C(0xfedcba9876543210),
    }, decoded;
    memcpy(bytes, &request, sizeof(request));
    assert(esp_now_hosted_decode_request(bytes, sizeof(bytes) - 1, &decoded));
    assert(decoded.seq == request.seq);
    assert(!esp_now_hosted_decode_request(bytes, sizeof(bytes), &decoded));
    for (size_t len = 0; len < sizeof(bytes) - 1; ++len)
        assert(!esp_now_hosted_decode_request(bytes, len, &decoded));
    assert(!esp_now_hosted_decode_request(NULL, sizeof(bytes), &decoded));
    assert(!esp_now_hosted_decode_request(bytes, sizeof(bytes), NULL));

    request.payload_len = 0;
    memcpy(bytes, &request, sizeof(request));
    assert(esp_now_hosted_decode_request(bytes, sizeof(request), &decoded));
    request.seq = 0;
    memcpy(bytes, &request, sizeof(request));
    assert(!esp_now_hosted_decode_request(bytes, sizeof(request), &decoded));
    request.seq = UINT64_MAX;
    request.version++;
    memcpy(bytes, &request, sizeof(request));
    assert(!esp_now_hosted_decode_request(bytes, sizeof(request), &decoded));
    request.version = ESP_NOW_HOSTED_WIRE_VERSION;
    request.magic ^= 1;
    memcpy(bytes, &request, sizeof(request));
    assert(!esp_now_hosted_decode_request(bytes, sizeof(request), &decoded));
    request.magic = ESP_NOW_HOSTED_MAGIC;
    request.payload_len = UINT16_MAX;
    memcpy(bytes, &request, sizeof(request));
    assert(!esp_now_hosted_decode_request(bytes, sizeof(bytes), &decoded));
}

static void info_tests(void)
{
    /* The info block is the largest return and must fit the response bound. */
    assert(sizeof(esp_now_hosted_info_t) == 40);
    assert(ESP_NOW_HOSTED_MAX_RETURN >= sizeof(esp_now_hosted_info_t));
    assert(ESP_NOW_HOSTED_MAX_RETURN >= sizeof(esp_now_hosted_peer_t));
    uint8_t bytes[sizeof(esp_now_hosted_resp_t) + sizeof(esp_now_hosted_info_t)] = {0};
    esp_now_hosted_resp_t response = {
        .request = {
            .magic = ESP_NOW_HOSTED_MAGIC,
            .version = ESP_NOW_HOSTED_WIRE_VERSION,
            .opcode = ESP_NOW_HOSTED_OP_GET_INFO,
            .payload_len = sizeof(esp_now_hosted_info_t),
            .seq = 7,
        },
        .status = 0,
    }, decoded;
    esp_now_hosted_info_t info = {.wire_version = ESP_NOW_HOSTED_WIRE_VERSION,
                                  .image_state = ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY,
                                  .running_slot = 1, .uptime_ms = 123456, .cpu_mhz = 160};
    memcpy(bytes, &response, sizeof(response));
    memcpy(bytes + sizeof(response), &info, sizeof(info));
    assert(esp_now_hosted_decode_response(bytes, sizeof(bytes), &decoded));
    assert(decoded.request.payload_len == sizeof(info));
    esp_now_hosted_info_t back;
    memcpy(&back, bytes + sizeof(response), sizeof(back));
    assert(back.image_state == ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY && back.running_slot == 1 &&
           back.uptime_ms == 123456 && back.cpu_mhz == 160);
    /* The marker a host searches a firmware file for ends in the wire version. */
    const char *marker = ESP_NOW_HOSTED_BRIDGE_MARKER;
    assert(marker[strlen(marker) - 1] == '0' + ESP_NOW_HOSTED_WIRE_VERSION);
}

static void response_tests(void)
{
    uint8_t bytes[sizeof(esp_now_hosted_resp_t) + ESP_NOW_HOSTED_MAX_RETURN + 1] = {0};
    esp_now_hosted_resp_t response = {
        .request = {
            .magic = ESP_NOW_HOSTED_MAGIC,
            .version = ESP_NOW_HOSTED_WIRE_VERSION,
            .opcode = ESP_NOW_HOSTED_OP_GET_PEER,
            .payload_len = ESP_NOW_HOSTED_MAX_RETURN,
            .seq = UINT64_C(0x100000000),
        },
        .status = -12345,
    }, decoded;
    memcpy(bytes, &response, sizeof(response));
    assert(esp_now_hosted_decode_response(bytes, sizeof(bytes) - 1, &decoded));
    assert(decoded.status == -12345 && decoded.request.seq == response.request.seq);
    for (size_t len = 0; len < sizeof(bytes) - 1; ++len)
        assert(!esp_now_hosted_decode_response(bytes, len, &decoded));
    assert(!esp_now_hosted_decode_response(bytes, sizeof(bytes), &decoded));
    response.request.payload_len++;
    memcpy(bytes, &response, sizeof(response));
    assert(!esp_now_hosted_decode_response(bytes, sizeof(bytes), &decoded));
    response.request.payload_len = 0;
    memcpy(bytes, &response, sizeof(response));
    assert(esp_now_hosted_decode_response(bytes, sizeof(response), &decoded));
}

static void event_tests(void)
{
    uint8_t bytes[ESP_NOW_HOSTED_MAX_EVENT + 1] = {0};
    esp_now_hosted_event_header_t event = {
        .magic = ESP_NOW_HOSTED_MAGIC,
        .version = ESP_NOW_HOSTED_WIRE_VERSION,
        .payload_len = ESP_NOW_HOSTED_MAX_EVENT - sizeof(event),
        .epoch = UINT64_C(0xf000000000000001),
    }, decoded;
    memcpy(bytes, &event, sizeof(event));
    assert(esp_now_hosted_decode_event(bytes, sizeof(bytes) - 1, &decoded));
    assert(decoded.epoch == event.epoch);
    for (size_t len = 0; len < sizeof(bytes) - 1; ++len)
        assert(!esp_now_hosted_decode_event(bytes, len, &decoded));
    assert(!esp_now_hosted_decode_event(bytes, sizeof(bytes), &decoded));
    event.reserved = 1;
    memcpy(bytes, &event, sizeof(event));
    assert(!esp_now_hosted_decode_event(bytes, sizeof(bytes) - 1, &decoded));
    event.reserved = 0;
    event.epoch = 0;
    memcpy(bytes, &event, sizeof(event));
    assert(!esp_now_hosted_decode_event(bytes, sizeof(bytes) - 1, &decoded));
}

int main(void)
{
    request_tests();
    response_tests();
    info_tests();
    event_tests();
    puts("ESP-NOW hosted protocol bounds/layout tests passed");
    return 0;
}
