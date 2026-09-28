// Isolated hardware qualification: no NVS, filesystem, SD, LCD or radio setup.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

#define CHECK_ESP(expr) do { esp_err_t e = (expr); if (e != ESP_OK) { printf("CAM_FAIL stage=%s err=%s\n", #expr, esp_err_to_name(e)); return e; } } while (0)
#define CHECK_IO(expr) do { if ((expr) != 0) { printf("CAM_FAIL stage=%s errno=%d\n", #expr, errno); goto fail; } } while (0)
static esp_cam_sensor_xclk_handle_t xclk;
static i2c_master_bus_handle_t i2c;
static uint32_t crc(const void *p, size_t n) { return esp_rom_crc32_le(0, p, n); }

static esp_err_t camera_init(void) {
    // GPIO12 supplies both the camera and optional LCD. Do not initialize LCD.
    CHECK_ESP(rtc_gpio_hold_dis(GPIO_NUM_12));
    CHECK_ESP(rtc_gpio_deinit(GPIO_NUM_12));
    CHECK_ESP(gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT));
    CHECK_ESP(gpio_set_level(GPIO_NUM_12, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_cam_sensor_xclk_config_t xc = { .esp_clock_router_cfg = { .xclk_pin = 11, .xclk_freq_hz = 24000000 } };
    CHECK_ESP(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk));
    CHECK_ESP(esp_cam_sensor_xclk_start(xclk, &xc));
    CHECK_ESP(gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT));
    CHECK_ESP(gpio_set_level(GPIO_NUM_26, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
    i2c_master_bus_config_t bus = {.i2c_port=0, .sda_io_num=14, .scl_io_num=13, .clk_source=I2C_CLK_SRC_DEFAULT, .flags.enable_internal_pullup=true};
    CHECK_ESP(i2c_new_master_bus(&bus, &i2c));
    for (unsigned a=0x08; a<0x78; ++a) if (i2c_master_probe(i2c,a,15)==ESP_OK) printf("CAM_SCCB address=0x%02x\n", a);
    const esp_video_init_csi_config_t csi = {.sccb_config={.init_sccb=false,.i2c_handle=i2c,.freq=400000},.reset_pin=26,.pwdn_pin=-1,.dont_init_ldo=false};
    const esp_video_init_config_t config = {.csi=&csi};
    // esp_video owns MIPI LDO3 at 2.5 V; shared camera rail stays high.
    CHECK_ESP(esp_video_init(&config));
    return ESP_OK;
}

static esp_err_t encode_frame(const uint8_t *raw, size_t raw_size, uint32_t w, uint32_t h) {
    jpeg_encoder_handle_t encoder = NULL;
    jpeg_encode_engine_cfg_t ec = {.timeout_ms=2000};
    CHECK_ESP(jpeg_new_encoder_engine(&ec, &encoder));
    jpeg_encode_memory_alloc_cfg_t out_cfg = {.buffer_direction=JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t out_size=0;
    uint8_t *out=jpeg_alloc_encoder_mem(raw_size,&out_cfg,&out_size);
    if (!out) { jpeg_del_encoder_engine(encoder); return ESP_ERR_NO_MEM; }
    jpeg_encode_cfg_t cfg = {.width=w,.height=h,.src_type=JPEG_ENCODE_IN_FORMAT_RGB565,.sub_sample=JPEG_DOWN_SAMPLING_YUV420,.image_quality=80,.pixel_reverse=false};
    uint32_t used=0;
    int64_t start=esp_timer_get_time();
    esp_err_t e=jpeg_encoder_process(encoder,&cfg,raw,raw_size,out,out_size,&used);
    printf("CAM_JPEG_ENCODE result=%s us=%lld width=%lu height=%lu bytes=%lu\n",esp_err_to_name(e),(long long)(esp_timer_get_time()-start),(unsigned long)w,(unsigned long)h,(unsigned long)used);
    if(e==ESP_OK && used>=4 && used<=out_size && out[0]==0xff && out[1]==0xd8 && out[used-2]==0xff && out[used-1]==0xd9) {
        printf("CAM_JPEG_BEGIN bytes=%lu crc32=%08lx\n",(unsigned long)used,(unsigned long)crc(out,used));
        for (size_t off=0; off<used; off+=128) {
            printf("CAM_JPEG offset=%u hex=",(unsigned)off);
            for (size_t j=off; j<used && j<off+128; ++j) printf("%02x",out[j]);
            printf("\n"); fflush(stdout); vTaskDelay(1);
        }
        printf("CAM_JPEG_END\n");
    } else if(e==ESP_OK) { e=ESP_FAIL; }
    free(out); jpeg_del_encoder_engine(encoder);
    return e;
}

static esp_err_t capture_test(void) {
    int fd=open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME,O_RDONLY);
    if(fd<0) { printf("CAM_FAIL stage=open errno=%d\n",errno); return ESP_FAIL; }
    int type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint8_t *buffers[3]={0};
    size_t lengths[3]={0};
    uint8_t *snapshot=NULL;
    size_t snapshot_size=0;
    bool streaming=false;
    esp_err_t result=ESP_FAIL;
    struct v4l2_capability cap={0};
    CHECK_IO(ioctl(fd,VIDIOC_QUERYCAP,&cap));
    printf("CAM_DEVICE driver=%s card=%s bus=%s\n",cap.driver,cap.card,cap.bus_info);
    esp_cam_sensor_id_t chip={0};
    struct v4l2_ext_control ctrl={.id=ESP_CAM_SENSOR_IOC_G_CHIP_ID,.p_u8=(uint8_t*)&chip,.size=sizeof(chip)};
    struct v4l2_ext_controls ctrls={.ctrl_class=V4L2_CTRL_CLASS_ESP_CAM_IOCTL,.count=1,.controls=&ctrl};
    CHECK_IO(ioctl(fd,VIDIOC_G_EXT_CTRLS,&ctrls));
    printf("CAM_SENSOR pid=0x%04x ver=0x%02x\n",chip.pid,chip.ver);
    esp_cam_sensor_format_t sensor={0};
    CHECK_IO(ioctl(fd,VIDIOC_G_SENSOR_FMT,&sensor));
    printf("CAM_SENSOR_FORMAT name=%s width=%lu height=%lu\n",sensor.name,(unsigned long)sensor.width,(unsigned long)sensor.height);
    struct v4l2_format format={.type=type};
    CHECK_IO(ioctl(fd,VIDIOC_G_FMT,&format));
    // esp_video 2.2.0 keeps optional G_FMT stride/size fields at zero unless
    // the application supplies them. Its raw-buffer allocator derives width *
    // height * bpp. Supply that same checked packed-RGB565 layout explicitly;
    // QUERYBUF capacity and every DQBUF byte count are independently checked.
    if(!format.fmt.pix.width || !format.fmt.pix.height ||
       format.fmt.pix.width>1920 || format.fmt.pix.height>1080) goto fail;
    format.fmt.pix.pixelformat=V4L2_PIX_FMT_RGB565;
    format.fmt.pix.bytesperline=format.fmt.pix.width*2;
    format.fmt.pix.sizeimage=format.fmt.pix.bytesperline*format.fmt.pix.height;
    printf("CAM_LAYOUT source=derived_packed_rgb565 stride=%lu bytes=%lu\n",
           (unsigned long)format.fmt.pix.bytesperline,(unsigned long)format.fmt.pix.sizeimage);
    CHECK_IO(ioctl(fd,VIDIOC_S_FMT,&format));
    printf("CAM_OUTPUT width=%lu height=%lu bytesperline=%lu sizeimage=%lu format=%08lx\n",(unsigned long)format.fmt.pix.width,(unsigned long)format.fmt.pix.height,(unsigned long)format.fmt.pix.bytesperline,(unsigned long)format.fmt.pix.sizeimage,(unsigned long)format.fmt.pix.pixelformat);
    if(format.fmt.pix.width>1920 || format.fmt.pix.height>1080 || format.fmt.pix.sizeimage>1920*1080*2 || format.fmt.pix.sizeimage!=format.fmt.pix.width*format.fmt.pix.height*2) goto fail;
    struct timeval timeout={.tv_sec=2};
    CHECK_IO(ioctl(fd,VIDIOC_S_DQBUF_TIMEOUT,&timeout));
    struct v4l2_requestbuffers req={.count=3,.type=type,.memory=V4L2_MEMORY_MMAP};
    CHECK_IO(ioctl(fd,VIDIOC_REQBUFS,&req));
    if(req.count!=3) goto fail;
    for(unsigned j=0;j<3;++j) {
        struct v4l2_buffer b={.index=j,.type=type,.memory=V4L2_MEMORY_MMAP};
        CHECK_IO(ioctl(fd,VIDIOC_QUERYBUF,&b));
        buffers[j]=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset);
        lengths[j]=b.length;
        if(!buffers[j] || buffers[j]==MAP_FAILED) goto fail;
        CHECK_IO(ioctl(fd,VIDIOC_QBUF,&b));
    }
    jpeg_encode_memory_alloc_cfg_t input_cfg={.buffer_direction=JPEG_ENC_ALLOC_INPUT_BUFFER};
    snapshot=jpeg_alloc_encoder_mem(format.fmt.pix.sizeimage,&input_cfg,&snapshot_size);
    if(!snapshot) goto fail;
    CHECK_IO(ioctl(fd,VIDIOC_STREAMON,&type)); streaming=true;
    int64_t started=esp_timer_get_time();
    unsigned captured=0, errors=0;
    for(unsigned j=0;j<90;++j) {
        struct v4l2_buffer b={.type=type,.memory=V4L2_MEMORY_MMAP};
        CHECK_IO(ioctl(fd,VIDIOC_DQBUF,&b));
        if(b.index>=3 || b.bytesused>lengths[b.index] || b.bytesused>snapshot_size) goto fail;
        if(b.flags&V4L2_BUF_FLAG_ERROR) errors++;
        else {
            if(b.bytesused!=format.fmt.pix.width*format.fmt.pix.height*2) goto fail;
            captured++;
            if(j>=87) printf("CAM_FRAME index=%u sequence=%lu bytes=%lu crc32=%08lx\n",j,(unsigned long)b.sequence,(unsigned long)b.bytesused,(unsigned long)crc(buffers[b.index],b.bytesused));
            if(j==89) memcpy(snapshot,buffers[b.index],b.bytesused);
        }
        CHECK_IO(ioctl(fd,VIDIOC_QBUF,&b));
    }
    int64_t elapsed=esp_timer_get_time()-started;
    CHECK_IO(ioctl(fd,VIDIOC_STREAMOFF,&type)); streaming=false;
    printf("CAM_CAPTURE captured=%u errors=%u elapsed_us=%lld fps=%.2f\n",captured,errors,(long long)elapsed,90e6/(double)elapsed);
    if(errors || captured!=90) goto fail;
    result=encode_frame(snapshot,format.fmt.pix.sizeimage,format.fmt.pix.width,format.fmt.pix.height);
fail:
    if(streaming) ioctl(fd,VIDIOC_STREAMOFF,&type);
    struct v4l2_requestbuffers release={.count=0,.type=type,.memory=V4L2_MEMORY_MMAP};
    ioctl(fd,VIDIOC_REQBUFS,&release);
    close(fd); free(snapshot);
    return result;
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("CAM_PROBE_START version=2 idf=%s flash_writes=none lcd=off radio=off sd=off\n",esp_get_idf_version());
    esp_err_t e=camera_init();
    if(e==ESP_OK) {e=capture_test(); esp_err_t de=esp_video_deinit(); if(e==ESP_OK) e=de; }
    if(xclk) {esp_cam_sensor_xclk_stop(xclk); esp_cam_sensor_xclk_free(xclk);}
    if(i2c) i2c_del_master_bus(i2c);
    gpio_set_level(GPIO_NUM_12,0);
    printf("CAM_RESULT result=%s heap_ok=%d free_internal=%u free_psram=%u\n",esp_err_to_name(e),heap_caps_check_integrity_all(true),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
