/* Standalone Opera harness: run the working core on Linux and log what the
   host asks the drive for. Used purely as an oracle to compare against. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include "native_backend_xbus.h"
void* xbus_cdrom_plugin(int proc_, void* data_);
}

extern "C" { int DRAM_VRAM_UNSET_CFG_dummy; }
#define DRAM_VRAM_UNSET_CFG 0
#define DRAM_VRAM_STOCK_CFG 0x29

extern "C" {
extern uint8_t* ROM1;
extern uint8_t* NVRAM;
int   opera_mem_init(int mem_cfg);
int   opera_mem_cfg(void);
void  opera_mem_rom1_clear(void);
void  opera_mem_rom1_byteswap32_if_le(void);
typedef void* (*opera_ext_interface_t)(int, void*);
int   opera_3do_init(opera_ext_interface_t cb);
void  opera_3do_process_frame(void);
int   opera_vdlp_set_video_buffer(void* buf);
void  opera_cdrom_set_callbacks(uint32_t(*)(void), void(*)(const uint32_t), void(*)(void*));
void  opera_cdrom_set_track_callbacks(uint32_t(*)(void), int(*)(uint32_t,uint8_t*,uint8_t*,uint32_t*,uint32_t*));
void  opera_cdrom_set_audio_callbacks(int(*)(uint32_t,uint32_t), int(*)(uint32_t,uint32_t), void(*)(uint8_t), void(*)(void));
uint32_t opera_dsp_loop(void);
}

/* Disc access, straight off a flat 2048-byte-per-sector image. */
static FILE*    g_disc = NULL;
static uint32_t g_sectors = 0;
static uint32_t g_sector = 0;
static uint32_t disc_size(void) { return g_sectors; }
static void disc_set_sector(const uint32_t s) { g_sector = s; }
static void disc_read_sector(void* buf) {
    memset(buf, 0, 2048);
    if (!g_disc) return;
    fprintf(stderr, "ORACLE read LBA %u\n", g_sector);
    fseek(g_disc, (long)g_sector * 2048, SEEK_SET);
    fread(buf, 1, 2048, g_disc);
}
static uint32_t disc_tracks(void) { return 1; }
static int disc_track_info(uint32_t i, uint8_t* num, uint8_t* audio,
                           uint32_t* start, uint32_t* count) {
    if (i != 0) return 0;
    *num = 1; *audio = 0; *start = 0; *count = g_sectors;
    return 1;
}
static int  a_range(uint32_t a, uint32_t b) { (void)a;(void)b; return 0; }
static int  a_track(uint32_t a, uint32_t b) { (void)a;(void)b; return 0; }
static void a_pause(uint8_t p) { (void)p; }
static void a_stop(void) { }

static void* cb(int command, void* data) {
    (void)data;
    if (command == 2) opera_dsp_loop();
    return NULL;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: harness <bios> <disc> [frames]\n"); return 1; }
    int frames = argc > 3 ? atoi(argv[3]) : 400;

    g_disc = fopen(argv[2], "rb");
    if (!g_disc) { fprintf(stderr, "cannot open disc\n"); return 1; }
    fseek(g_disc, 0, SEEK_END);
    g_sectors = (uint32_t)(ftell(g_disc) / 2048);
    fprintf(stderr, "disc: %u sectors\n", g_sectors);

    opera_cdrom_set_callbacks(disc_size, disc_set_sector, disc_read_sector);
    opera_cdrom_set_track_callbacks(disc_tracks, disc_track_info);
    opera_cdrom_set_audio_callbacks(a_range, a_track, a_pause, a_stop);

    opera_xbus_init(xbus_cdrom_plugin);

    if (opera_mem_cfg() == DRAM_VRAM_UNSET_CFG)
        opera_mem_init(DRAM_VRAM_STOCK_CFG);

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "no bios\n"); return 1; }
    opera_mem_rom1_clear();
    size_t n = fread(ROM1, 1, 1024*1024, f);
    fclose(f);
    opera_mem_rom1_byteswap32_if_le();
    fprintf(stderr, "bios %zu bytes\n", n);

    static uint8_t video[1024*1024];
    opera_vdlp_set_video_buffer(video);

    if (opera_3do_init(cb) != 0) { fprintf(stderr, "opera_3do_init failed\n"); return 1; }

    opera_xbus_device_load(0, argv[2]);
    fprintf(stderr, "disc: %s\n", argv[2]);

    for (int i = 0; i < frames; ++i) opera_3do_process_frame();
    fprintf(stderr, "ran %d frames\n", frames);
    return 0;
}
extern "C" { int g_dma_n = 0; int g_cmd_n = 0; }
