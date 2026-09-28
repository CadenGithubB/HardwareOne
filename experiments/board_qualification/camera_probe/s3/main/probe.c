// Isolated XIAO ESP32-S3 Sense control: no NVS, filesystem, SD or radio setup.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static uint32_t crc(const void *p,size_t n) {return esp_rom_crc32_le(0,p,n);}
static void emit_jpeg(const uint8_t *p,size_t n) {
    printf("CAM_JPEG_BEGIN bytes=%u crc32=%08lx\n",(unsigned)n,(unsigned long)crc(p,n));
    for(size_t off=0;off<n;off+=128) {
        printf("CAM_JPEG offset=%u hex=",(unsigned)off);
        for(size_t j=off;j<n && j<off+128;++j) printf("%02x",p[j]);
        printf("\n");fflush(stdout);vTaskDelay(1);
    }
    printf("CAM_JPEG_END\n");
}
static esp_err_t capture_mode(framesize_t fs,unsigned wantw,unsigned wanth,bool emit) {
    camera_config_t cfg={
        .pin_pwdn=-1,.pin_reset=-1,.pin_xclk=10,.pin_sccb_sda=40,.pin_sccb_scl=39,
        .pin_d0=15,.pin_d1=17,.pin_d2=18,.pin_d3=16,.pin_d4=14,.pin_d5=12,.pin_d6=11,.pin_d7=48,
        .pin_vsync=38,.pin_href=47,.pin_pclk=13,
        .xclk_freq_hz=20000000,.ledc_timer=LEDC_TIMER_0,.ledc_channel=LEDC_CHANNEL_0,
        .pixel_format=PIXFORMAT_JPEG,.frame_size=fs,.jpeg_quality=12,.fb_count=2,
        .fb_location=CAMERA_FB_IN_PSRAM,.grab_mode=CAMERA_GRAB_LATEST,
    };
    esp_err_t e=esp_camera_init(&cfg);
    if(e!=ESP_OK) {printf("CAM_FAIL stage=init err=%s\n",esp_err_to_name(e));return e;}
    sensor_t *s=esp_camera_sensor_get();
    printf("CAM_SENSOR pid=0x%04x ver=0x%02x width=%u height=%u\n",s?s->id.PID:0,s?s->id.VER:0,wantw,wanth);
    uint8_t *snapshot=NULL;size_t size=0;
    unsigned captured=0,errors=0;
    int64_t start=esp_timer_get_time();
    for(unsigned j=0;j<35;++j) {
        camera_fb_t *fb=esp_camera_fb_get();
        if(!fb) {errors++;break;}
        bool ok=fb->format==PIXFORMAT_JPEG && fb->width==wantw && fb->height==wanth && fb->len>4 && fb->buf[0]==0xff && fb->buf[1]==0xd8;
        if(!ok) errors++;
        else {
            captured++;
            if(j>=32) printf("CAM_FRAME index=%u bytes=%u width=%u height=%u crc32=%08lx\n",j,(unsigned)fb->len,(unsigned)fb->width,(unsigned)fb->height,(unsigned long)crc(fb->buf,fb->len));
            if(j==34 && emit) {size=fb->len;snapshot=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(snapshot)memcpy(snapshot,fb->buf,size);else errors++;}
        }
        esp_camera_fb_return(fb);
    }
    int64_t elapsed=esp_timer_get_time()-start;
    printf("CAM_CAPTURE width=%u height=%u captured=%u errors=%u elapsed_us=%lld fps=%.2f\n",wantw,wanth,captured,errors,(long long)elapsed,35e6/(double)elapsed);
    esp_err_t de=esp_camera_deinit();
    if(snapshot) {emit_jpeg(snapshot,size);free(snapshot);}
    return (de==ESP_OK && errors==0 && captured==35)?ESP_OK:ESP_FAIL;
}
void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("CAM_PROBE_START board=xiao-s3-sense idf=%s flash_writes=none radio=off sd=off\n",esp_get_idf_version());
    esp_err_t e=capture_mode(FRAMESIZE_QVGA,320,240,false);
    vTaskDelay(pdMS_TO_TICKS(200));
    if(e==ESP_OK)e=capture_mode(FRAMESIZE_VGA,640,480,true);
    printf("CAM_RESULT result=%s heap_ok=%d free_internal=%u free_psram=%u\n",esp_err_to_name(e),heap_caps_check_integrity_all(true),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
