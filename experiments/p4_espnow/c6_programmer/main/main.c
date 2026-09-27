/* P4X-EYE service firmware. Explicit serial commands only; no automatic flash writes.
 * Wiring: Espressif EYE-MB v2.3 schematic: EN9 BOOT33 TX35 RX36.
 * Original C6 firmware is read before any caller requests a write. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp32_port.h"
#include "esp_loader.h"
#include "mbedtls/sha256.h"

static uint32_t flash_size;
static bool connected;
static uint8_t block[1024];
static char line[2112];

static bool read_line(char *s, size_t cap) {
    size_t n = 0; bool overflow = false;
    for (;;) {
        int c = getchar();
        if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\r') continue;
        if (c == '\n') { s[n] = 0; return !overflow; }
        if (n + 1 < cap) s[n++] = (char)c; else overflow = true;
    }
}
static int nibble(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
static void print_hex(const uint8_t *data, size_t len) {
    static const char hex[]="0123456789abcdef";
    for (size_t i=0;i<len;i++) { line[2*i]=hex[data[i]>>4]; line[2*i+1]=hex[data[i]&15]; }
    line[len*2]=0; printf("%s\n",line); fflush(stdout);
}
static int connect_c6(void) {
    connected=false;
    loader_port_change_transmission_rate(115200);
    esp_loader_connect_args_t args=ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err=esp_loader_connect_with_stub(&args);
    if (err) { printf("ERROR connect %d\n",err); return err; }
    if (esp_loader_get_target()!=ESP32C6_CHIP) { puts("ERROR wrong_chip"); return -1; }
    err=esp_loader_flash_detect_size(&flash_size);
    if (err) { printf("ERROR flash_size %d\n",err); return err; }
    err=esp_loader_change_transmission_rate_stub(115200,921600);
    if (err) { printf("ERROR baud %d\n",err); return err; }
    loader_port_change_transmission_rate(921600);
    connected=true;
    printf("INFO chip=esp32c6 flash=%"PRIu32" baud=921600\n",flash_size);
    return 0;
}
static void read_region(uint32_t offset,uint32_t size,bool output) {
    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha,0);
    printf("BEGIN_READ %"PRIu32" %"PRIu32"\n",offset,size); fflush(stdout);
    for(uint32_t pos=0;pos<size;) {
        uint32_t n=size-pos; if(n>sizeof(block)) n=sizeof(block);
        int err=esp_loader_flash_read(block,offset+pos,n);
        if(err) { printf("ERROR read %d %"PRIu32"\n",err,pos); mbedtls_sha256_free(&sha); return; }
        mbedtls_sha256_update(&sha,block,n);
        if(output) { printf("DATA %08"PRIx32" %"PRIu32" ",offset+pos,n); print_hex(block,n); }
        pos+=n;
        if(!output && (pos%262144==0)) { printf("PROGRESS %"PRIu32"\n",pos); fflush(stdout); }
    }
    uint8_t digest[32]; mbedtls_sha256_finish(&sha,digest); mbedtls_sha256_free(&sha);
    printf("SHA256 "); print_hex(digest,sizeof(digest)); puts("DONE_READ");
}
static void write_region(uint32_t offset,uint32_t size) {
    int err=esp_loader_flash_start(offset,size,sizeof(block));
    if(err) { printf("ERROR write_start %d\n",err); return; }
    printf("READY_WRITE %"PRIu32"\n",size); fflush(stdout);
    for(uint32_t pos=0;pos<size;) {
        uint32_t n=size-pos; if(n>sizeof(block)) n=sizeof(block);
        if(!read_line(line,sizeof(line)) || strlen(line)!=5+n*2 || strncmp(line,"DATA ",5)) {
            puts("ERROR input_length"); connected=false; return;
        }
        for(uint32_t i=0;i<n;i++) {
            int hi=nibble(line[5+i*2]),lo=nibble(line[6+i*2]);
            if(hi<0 || lo<0) { puts("ERROR input_hex"); connected=false; return; }
            block[i]=(hi<<4)|lo;
        }
        err=esp_loader_flash_write(block,n);
        if(err) { printf("ERROR write %d %"PRIu32"\n",err,pos); connected=false; return; }
        pos+=n; printf("WROTE %"PRIu32"\n",pos); fflush(stdout);
    }
    err=esp_loader_flash_verify();
    printf("DONE_WRITE verify=%d\n",err); fflush(stdout);
}
void app_main(void) {
    usb_serial_jtag_driver_config_t usb={.rx_buffer_size=8192,.tx_buffer_size=8192};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin,NULL,_IONBF,0); setvbuf(stdout,NULL,_IONBF,0);
    loader_esp32_config_t cfg={.baud_rate=115200,.uart_port=UART_NUM_1,
      .uart_tx_pin=35,.uart_rx_pin=36,.reset_trigger_pin=9,.gpio0_trigger_pin=33,
      .rx_buffer_size=8192,.tx_buffer_size=2048};
    int err=loader_port_esp32_init(&cfg);
    if(err) { printf("ERROR port %d\n",err); return; }
    /* The loader port configures output pins but leaves their levels low.
     * Release BOOT before EN so a fresh service boot can run the C6 normally. */
    gpio_set_level(GPIO_NUM_33,1);
    gpio_set_level(GPIO_NUM_9,1);
    puts("PROGRAMMER_READY commands=connect,read,hash,write,run,monitor");
    while(1) {
        if(!read_line(line,sizeof(line))) { puts("ERROR command_length"); continue; }
        if(!strcmp(line,"connect")) { connect_c6(); continue; }
        if(!strcmp(line,"run")) { gpio_set_level(GPIO_NUM_33,1); esp_loader_reset_target(); connected=false; puts("RUNNING"); continue; }
        if(!strcmp(line,"monitor")) {
            uart_set_baudrate(UART_NUM_1,115200); puts("MONITOR 10s");
            for(int i=0;i<100;i++) { int n=uart_read_bytes(UART_NUM_1,block,sizeof(block),pdMS_TO_TICKS(100)); if(n>0) fwrite(block,1,n,stdout); }
            puts("DONE_MONITOR"); continue;
        }
        char command[16],extra; unsigned long offset,size;
        if(sscanf(line,"%15s %lu %lu %c",command,&offset,&size,&extra)!=3 || !connected ||
          !size || offset>flash_size || size>flash_size-offset) { puts("ERROR command_or_range"); continue; }
        if(!strcmp(command,"read")) read_region(offset,size,true);
        else if(!strcmp(command,"hash")) read_region(offset,size,false);
        else if(!strcmp(command,"write") && !(offset%4096)) write_region(offset,size);
        else puts("ERROR command_or_alignment");
    }
}
