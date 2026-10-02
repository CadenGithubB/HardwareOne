#include <stdio.h>
#include <string.h>
#include "esp_loader_io.h"
#include "slip.h"

static const unsigned char *input;
static size_t remaining;
esp_loader_error_t loader_port_read(uint8_t *p,uint16_t n,uint32_t timeout) {
    (void)timeout; if(n>remaining) return ESP_LOADER_ERROR_TIMEOUT;
    memcpy(p,input,n);input+=n;remaining-=n;return ESP_LOADER_SUCCESS;
}
esp_loader_error_t loader_port_write(const uint8_t *p,uint16_t n,uint32_t t) {
    (void)p;(void)n;(void)t;return ESP_LOADER_SUCCESS;
}
uint32_t loader_port_remaining_time(void) { return 1000; }
static int check(const unsigned char *wire,size_t wirelen,const unsigned char *expected,size_t n) {
    unsigned char actual[16]={0};size_t got=0;input=wire;remaining=wirelen;
    int e=SLIP_receive_packet(actual,n,&got);
    return e!=ESP_LOADER_SUCCESS || got!=n || memcmp(actual,expected,n);
}
int main(void) {
    int failed=0;
    const unsigned char a[]={0xc0,0xdb,0xdc,0x01,0xc0},ae[]={0xc0,1};
    const unsigned char b[]={0xc0,0xdb,0xdd,0xc0},be[]={0xdb};
    const unsigned char c[]={0xc0,0x01,0xdb,0xdc,0xdb,0xdd,0xc0},ce[]={1,0xc0,0xdb};
    failed+=check(a,sizeof(a),ae,sizeof(ae));
    failed+=check(b,sizeof(b),be,sizeof(be));
    failed+=check(c,sizeof(c),ce,sizeof(ce));
    printf("SLIP golden frames: %d failed of 3\n",failed);
    return failed ? 1 : 0;
}
