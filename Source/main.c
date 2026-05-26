#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

#include "3dsDesignLayer0_t3x.h"
#include "3dsDesignLayer1_t3x.h"
#include "SelectionPip_t3x.h"

#include "0_t3x.h"
#include "1_t3x.h"
#include "2_t3x.h"
#include "3_t3x.h"
#include "4_t3x.h"
#include "5_t3x.h"
#include "6_t3x.h"
#include "7_t3x.h"
#include "8_t3x.h"
#include "9_t3x.h"
#include "10_t3x.h"
#include "Br_t3x.h"
#include "R_t3x.h"
#include "G_t3x.h"
#include "B_t3x.h"

#include "CameraShutter_wav.h"
#include "OpenOptions_wav.h"
#include "OptionValueChange_wav.h"
#include "SavedFile_wav.h"

#define CAM_WIDTH 400
#define CAM_HEIGHT 240
#define CAM_FRAME_BYTES (CAM_WIDTH * CAM_HEIGHT * 2)

typedef struct {
    u16* data;
    u32 size;
    int channels;
    int samplerate;
} sound_t;

sound_t snd_shutter;
sound_t snd_open;
sound_t snd_change;
sound_t snd_save;

ndspWaveBuf waveBufShutter;
ndspWaveBuf waveBufUI;

static void load_wav(const u8* wav_data, u32 wav_size, sound_t* out) {
    u32 offset = 12;
    while (offset < wav_size) {
        if (memcmp(&wav_data[offset], "fmt ", 4) == 0) {
            out->channels = wav_data[offset + 10] | (wav_data[offset + 11] << 8);
            out->samplerate = wav_data[offset + 12] | (wav_data[offset + 13] << 8) | (wav_data[offset + 14] << 16) | (wav_data[offset + 15] << 24);
        } else if (memcmp(&wav_data[offset], "data", 4) == 0) {
            u32 data_size = wav_data[offset + 4] | (wav_data[offset + 5] << 8) | (wav_data[offset + 6] << 16) | (wav_data[offset + 7] << 24);
            out->size = data_size;
            out->data = linearAlloc(data_size);
            if (out->data) {
                memcpy(out->data, &wav_data[offset + 8], data_size);
                DSP_FlushDataCache(out->data, data_size);
            }
            break;
        }
        u32 chunk_size = wav_data[offset + 4] | (wav_data[offset + 5] << 8) | (wav_data[offset + 6] << 16) | (wav_data[offset + 7] << 24);
        offset += 8 + chunk_size;
    }
}

static void play_sound(sound_t* snd, int channel) {
    if (!snd || !snd->data) return;

    ndspWaveBuf* wb = (channel == 0) ? &waveBufShutter : &waveBufUI;

    ndspChnReset(channel);
    ndspChnSetFormat(channel, snd->channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(channel, (float)snd->samplerate);

    memset(wb, 0, sizeof(ndspWaveBuf));
    wb->data_vaddr = snd->data;
    wb->nsamples = snd->size / (snd->channels * sizeof(s16));
    wb->looping = false;
    wb->status = NDSP_WBUF_FREE;

    ndspChnWaveBufAdd(channel, wb);
}

u8* cam_buf = NULL;
u32 cam_transfer_unit = 0;

C2D_SpriteSheet sheet0, sheet1, sheet_pip;
C2D_Image img0, img1, img_pip;

C2D_SpriteSheet sheet_nums[11];
C2D_Image img_nums[11];
C2D_SpriteSheet sheet_sort[4];
C2D_Image img_sort[4];

C3D_RenderTarget* bottom_target;

int menu_open = 0;
int has_capture = 0;
int selected_camera = 0;
int contrast = 5;
int blur_strength = 1;
int menu_index = 0;
int sort_metric = 0;
int sort_threshold_min = 0;
int sort_threshold_max = 10;
int top_dirty_frames = 0;
int sort_accum_x = 0;
int sort_accum_y = 0;

#define MENU_ITEMS 5
#define SORT_METRIC_COUNT 4
#define CONTRAST_MIN 0
#define CONTRAST_MAX 10
#define BLUR_MIN 1
#define BLUR_MAX 9
#define SORT_STICK_DEADZONE 45
#define SORT_STICK_STEP 80
#define MENU_REPEAT_DELAY 20
#define MENU_REPEAT_RATE 3
#define SVG_SCALE 3.779527f

static const float pip_y[] = {
    17.778524f * SVG_SCALE,
    27.503549f * SVG_SCALE,
    37.904486f * SVG_SCALE,
    44.146845f * SVG_SCALE,
    50.269063f * SVG_SCALE
};

static u16* selected_image(void);
static void present_processing_frame(int phase);
static void present_saving_frame(int phase);

static inline u16 cam_pixel(const u16* rgb, int x, int y) {
    return rgb[y * CAM_WIDTH + x];
}

static inline u8 clamp_channel(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (u8)value;
}

static inline u8 apply_contrast(u8 value) {
    return clamp_channel((((int)value - 128) * (contrast * 20)) / 100 + 128);
}

static inline void rgb565_channels(u16 pixel, int* r, int* g, int* b) {
    *r = ((pixel & 0x1F) * 255) / 31;
    *g = (((pixel >> 5) & 0x3F) * 255) / 63;
    *b = (((pixel >> 11) & 0x1F) * 255) / 31;
}

static inline int sort_value(u16 pixel) {
    int r, g, b;
    rgb565_channels(pixel, &r, &g, &b);

    if (sort_metric == 1) return r;
    if (sort_metric == 2) return g;
    if (sort_metric == 3) return b;
    if (sort_metric == 0) return (r * 77 + g * 150 + b * 29) >> 8;
    return 0;
}

static inline int sort_in_threshold(u16 pixel) {
    int value = sort_value(pixel);
    int t_min = (sort_threshold_min * 255) / 10;
    int t_max = (sort_threshold_max * 255) / 10;
    return value >= t_min && value <= t_max;
}

static inline int should_swap(u16 a, u16 b, int ascending) {
    if (!sort_in_threshold(a) || !sort_in_threshold(b)) return 0;

    int av = sort_value(a);
    int bv = sort_value(b);
    return ascending ? av > bv : bv > av;
}

static inline void swap_pixels(u16* a, u16* b) {
    u16 tmp = *a;
    *a = *b;
    *b = tmp;
}

static void fb_put_rgb(u8* fb, int draw_x, int draw_y, u8 r, u8 g, u8 b) {
    if (draw_x < 0 || draw_x >= 400 || draw_y < 0 || draw_y >= 240) return;
    u32 v = (draw_y + draw_x * 240) * 3;
    fb[v] = r;
    fb[v + 1] = g;
    fb[v + 2] = b;
}

static void fb_fill_rect(u8* fb, int x, int y, int w, int h, u8 r, u8 g, u8 b) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            fb_put_rgb(fb, px, CAM_HEIGHT - 1 - py, r, g, b);
        }
    }
}

static u8 glyph_row(char c, int row) {
    static const u8 P[7] = {30, 17, 17, 30, 16, 16, 16};
    static const u8 A[7] = {14, 17, 17, 31, 17, 17, 17};
    static const u8 R[7] = {30, 17, 17, 30, 20, 18, 17};
    static const u8 O[7] = {14, 17, 17, 17, 17, 17, 14};
    static const u8 C[7] = {14, 17, 16, 16, 16, 17, 14};
    static const u8 E[7] = {31, 16, 16, 30, 16, 16, 31};
    static const u8 S[7] = {15, 16, 16, 14, 1, 1, 30};
    static const u8 I[7] = {14, 4, 4, 4, 4, 4, 14};
    static const u8 N[7] = {17, 25, 21, 19, 17, 17, 17};
    static const u8 G[7] = {14, 17, 16, 23, 17, 17, 14};
    static const u8 V[7] = {17, 17, 17, 17, 17, 10, 4};
    static const u8 DOT[7] = {0, 0, 0, 0, 0, 12, 12};
    const u8* glyph = NULL;

    switch (c) {
        case 'P': glyph = P; break;
        case 'A': glyph = A; break;
        case 'R': glyph = R; break;
        case 'O': glyph = O; break;
        case 'C': glyph = C; break;
        case 'E': glyph = E; break;
        case 'S': glyph = S; break;
        case 'I': glyph = I; break;
        case 'N': glyph = N; break;
        case 'G': glyph = G; break;
        case 'V': glyph = V; break;
        case '.': glyph = DOT; break;
        default: return 0;
    }

    return glyph[row];
}

static void fb_draw_char(u8* fb, char c, int x, int y, int scale, u8 r, u8 g, u8 b) {
    for (int row = 0; row < 7; row++) {
        u8 bits = glyph_row(c, row);
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                fb_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, r, g, b);
            }
        }
    }
}

static void fb_draw_text(u8* fb, const char* text, int x, int y, int scale, u8 r, u8 g, u8 b) {
    int cx = x;
    while (*text) {
        if (*text != ' ') fb_draw_char(fb, *text, cx, y, scale, r, g, b);
        cx += 6 * scale;
        text++;
    }
}

static void write_rgb565_to_fb(void* fb, const u16* img) {
    u8* fb_8 = (u8*)fb;

    for (u16 j = 0; j < CAM_HEIGHT; j++) {
        for (u16 i = 0; i < CAM_WIDTH; i++) {
            u16 draw_y = CAM_HEIGHT - 1 - j;
            u16 data = cam_pixel(img, i, j);
            fb_put_rgb(fb_8, i, draw_y,
                       apply_contrast((data & 0x1F) << 3),
                       apply_contrast(((data >> 5) & 0x3F) << 2),
                       apply_contrast(((data >> 11) & 0x1F) << 3));
        }
    }
}

static void write_le16(FILE* f, u16 value) {
    fputc(value & 0xFF, f);
    fputc((value >> 8) & 0xFF, f);
}

static void write_le32(FILE* f, u32 value) {
    fputc(value & 0xFF, f);
    fputc((value >> 8) & 0xFF, f);
    fputc((value >> 16) & 0xFF, f);
    fputc((value >> 24) & 0xFF, f);
}

static int save_image_to_sd(void) {
    if (!has_capture) return 0;

    present_saving_frame(0);

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/KwaCam", 0777);
    mkdir("sdmc:/3ds/KwaCam/Captures", 0777);

    char path[128];
    FILE* f = NULL;
    int limit = 1000;
    for (int i = 0; i < limit; i++) {
        snprintf(path, sizeof(path), "sdmc:/3ds/KwaCam/Captures/KWACAM%03d.BMP", i);
        f = fopen(path, "rb");
        if (f) {
            fclose(f);
            f = NULL;
            continue;
        }

        f = fopen(path, "wb");
        break;
    }

    if (!f) {
        // Limit reached, overwrite the last one
        snprintf(path, sizeof(path), "sdmc:/3ds/KwaCam/Captures/KWACAM%03d.BMP", limit - 1);
        f = fopen(path, "wb");
    }

    if (!f) {
        return 0;
    }

    int row_bytes = CAM_WIDTH * 3;
    int row_pad = (4 - (row_bytes & 3)) & 3;
    u32 pixel_bytes = (row_bytes + row_pad) * CAM_HEIGHT;
    u32 file_size = 54 + pixel_bytes;

    fputc('B', f);
    fputc('M', f);
    write_le32(f, file_size);
    write_le16(f, 0);
    write_le16(f, 0);
    write_le32(f, 54);

    write_le32(f, 40);
    write_le32(f, CAM_WIDTH);
    write_le32(f, (u32)(-CAM_HEIGHT));
    write_le16(f, 1);
    write_le16(f, 24);
    write_le32(f, 0);
    write_le32(f, pixel_bytes);
    write_le32(f, 2835);
    write_le32(f, 2835);
    write_le32(f, 0);
    write_le32(f, 0);

    const u16* img = selected_image();
    for (int y = 0; y < CAM_HEIGHT; y++) {
        for (int x = 0; x < CAM_WIDTH; x++) {
            u16 p = img[y * CAM_WIDTH + x];
            u8 r = apply_contrast(((p >> 11) & 0x1F) << 3);
            u8 g = apply_contrast(((p >> 5) & 0x3F) << 2);
            u8 b = apply_contrast((p & 0x1F) << 3);
            if (fputc(b, f) == EOF || fputc(g, f) == EOF || fputc(r, f) == EOF) {
                fclose(f);
                return 0;
            }
        }
        for (int i = 0; i < row_pad; i++) {
            if (fputc(0, f) == EOF) {
                fclose(f);
                return 0;
            }
        }
    }

    fclose(f);
    return 1;
}

static void draw_top_image(void) {
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) return;

    memset(fb, 0, 400 * 240 * 3);
    if (has_capture) write_rgb565_to_fb(fb, selected_image());
}

static void present_status_frame(const char* text, int text_x, int phase) {
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) return;

    memset(fb, 0, 400 * 240 * 3);
    if (has_capture) write_rgb565_to_fb(fb, selected_image());

    fb_fill_rect(fb, 116, 94, 168, 52, 8, 8, 8);
    fb_fill_rect(fb, 118, 96, 164, 48, 24, 24, 24);
    fb_draw_text(fb, text, text_x, 108, 2, 235, 235, 235);

    for (int i = 0; i < 4; i++) {
        u8 val = (i == (phase & 3)) ? 245 : 90;
        fb_fill_rect(fb, 176 + i * 14, 130, 8, 8, val, val, val);
    }

    gfxFlushBuffers();
    gspWaitForVBlank();
    gfxSwapBuffers();
}

static void present_processing_frame(int phase) {
    present_status_frame("PROCESSING...", 134, phase);
}

static void present_saving_frame(int phase) {
    present_status_frame("SAVING...", 146, phase);
}

void init_cameras(void) {
    camInit();
    CAMU_SetSize(SELECT_OUT1_OUT2, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_OUT1_OUT2, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(SELECT_OUT1_OUT2, FRAME_RATE_30);
    CAMU_SetNoiseFilter(SELECT_OUT1_OUT2, true);
    CAMU_SetAutoExposure(SELECT_OUT1_OUT2, true);
    CAMU_SetAutoWhiteBalance(SELECT_OUT1_OUT2, true);
    CAMU_SetBrightnessSynchronization(true);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_SetTrimming(PORT_CAM2, false);
    CAMU_Activate(SELECT_OUT1_OUT2);
}
static Result capture_camera(u8* buf, int camera) {
    u32 port = camera ? PORT_CAM2 : PORT_CAM1;
    u32 select = camera ? SELECT_OUT2 : SELECT_OUT1;

    CAMU_SetTransferBytes(port, cam_transfer_unit, CAM_WIDTH, CAM_HEIGHT);
    CAMU_Activate(select);

    Handle camReceiveEvent = 0;
    Result r;

    CAMU_ClearBuffer(port);
    r = CAMU_StartCapture(port);
    if (R_FAILED(r)) return r;

    r = CAMU_SetReceiving(&camReceiveEvent, buf, port, CAM_FRAME_BYTES, (s16)cam_transfer_unit);
    if (R_FAILED(r)) {
        CAMU_StopCapture(port);
        if (camReceiveEvent) svcCloseHandle(camReceiveEvent);
        return r;
    }

    r = svcWaitSynchronization(camReceiveEvent, 1000000000ULL);
    if (R_FAILED(r)) {
        CAMU_StopCapture(port);
        svcCloseHandle(camReceiveEvent);
        return r;
    }

    CAMU_StopCapture(port);

    svcCloseHandle(camReceiveEvent);

    return 0;
}

static u16* selected_image(void) {
    return (u16*)cam_buf;
}

static void blur_pass(const u16* src, u16* dst) {
    memcpy(dst, src, CAM_FRAME_BYTES);

    for (int y = 1; y < CAM_HEIGHT - 1; y++) {
        for (int x = 1; x < CAM_WIDTH - 1; x++) {
            int r = 0;
            int g = 0;
            int b = 0;

            for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    u16 p = src[(y + oy) * CAM_WIDTH + x + ox];
                    r += p & 0x1F;
                    g += (p >> 5) & 0x3F;
                    b += (p >> 11) & 0x1F;
                }
            }

            r /= 9;
            g /= 9;
            b /= 9;
            dst[y * CAM_WIDTH + x] = (u16)(r | (g << 5) | (b << 11));
        }
    }
}

static void apply_kernel_blur(void) {
    if (!has_capture) return;

    u16* tmp = malloc(CAM_FRAME_BYTES);
    if (!tmp) return;

    for (int pass = 0; pass < blur_strength; pass++) {
        present_processing_frame(pass);
        blur_pass(selected_image(), tmp);
        memcpy(selected_image(), tmp, CAM_FRAME_BYTES);
    }

    present_processing_frame(blur_strength);
    free(tmp);
    top_dirty_frames = 2;
}

static void sort_vertical(u16* img, int bright_down) {
    if (bright_down) {
        for (int y = CAM_HEIGHT - 2; y >= 0; y--) {
            for (int x = 0; x < CAM_WIDTH; x++) {
                u16* a = &img[y * CAM_WIDTH + x];
                u16* b = &img[(y + 1) * CAM_WIDTH + x];
                if (should_swap(*a, *b, 1)) swap_pixels(a, b);
            }
        }
    } else {
        for (int y = 0; y < CAM_HEIGHT - 1; y++) {
            for (int x = 0; x < CAM_WIDTH; x++) {
                u16* a = &img[y * CAM_WIDTH + x];
                u16* b = &img[(y + 1) * CAM_WIDTH + x];
                if (should_swap(*a, *b, 0)) swap_pixels(a, b);
            }
        }
    }
}

static void sort_horizontal(u16* img, int bright_right) {
    if (bright_right) {
        for (int y = 0; y < CAM_HEIGHT; y++) {
            for (int x = CAM_WIDTH - 2; x >= 0; x--) {
                u16* a = &img[y * CAM_WIDTH + x];
                u16* b = &img[y * CAM_WIDTH + x + 1];
                if (should_swap(*a, *b, 1)) swap_pixels(a, b);
            }
        }
    } else {
        for (int y = 0; y < CAM_HEIGHT; y++) {
            for (int x = 0; x < CAM_WIDTH - 1; x++) {
                u16* a = &img[y * CAM_WIDTH + x];
                u16* b = &img[y * CAM_WIDTH + x + 1];
                if (should_swap(*a, *b, 0)) swap_pixels(a, b);
            }
        }
    }
}

static int step_pixel_sorter(u32 keys, circlePosition stick) {
    if (!has_capture) return 0;
    if (!(keys & KEY_Y)) return 0;

    if (keys & KEY_DDOWN) {
        sort_accum_x = 0;
        sort_accum_y = 0;
        sort_vertical(selected_image(), 1);
        return 1;
    }
    if (keys & KEY_DUP) {
        sort_accum_x = 0;
        sort_accum_y = 0;
        sort_vertical(selected_image(), 0);
        return 1;
    }
    if (keys & KEY_DLEFT) {
        sort_accum_x = 0;
        sort_accum_y = 0;
        sort_horizontal(selected_image(), 0);
        return 1;
    }
    if (keys & KEY_DRIGHT) {
        sort_accum_x = 0;
        sort_accum_y = 0;
        sort_horizontal(selected_image(), 1);
        return 1;
    }

    int dx = stick.dx;
    int dy = stick.dy;
    int abs_x = dx < 0 ? -dx : dx;
    int abs_y = dy < 0 ? -dy : dy;

    if (abs_x < SORT_STICK_DEADZONE && abs_y < SORT_STICK_DEADZONE) {
        sort_accum_x = 0;
        sort_accum_y = 0;
        return 0;
    }

    if (abs_x < SORT_STICK_DEADZONE) dx = 0;
    if (abs_y < SORT_STICK_DEADZONE) dy = 0;

    sort_accum_x += dx;
    sort_accum_y += dy;

    int stepped = 0;
    while (sort_accum_x >= SORT_STICK_STEP) {
        sort_horizontal(selected_image(), 1);
        sort_accum_x -= SORT_STICK_STEP;
        stepped = 1;
    }
    while (sort_accum_x <= -SORT_STICK_STEP) {
        sort_horizontal(selected_image(), 0);
        sort_accum_x += SORT_STICK_STEP;
        stepped = 1;
    }
    while (sort_accum_y >= SORT_STICK_STEP) {
        sort_vertical(selected_image(), 0);
        sort_accum_y -= SORT_STICK_STEP;
        stepped = 1;
    }
    while (sort_accum_y <= -SORT_STICK_STEP) {
        sort_vertical(selected_image(), 1);
        sort_accum_y += SORT_STICK_STEP;
        stepped = 1;
    }

    return stepped;
}

static void draw_number_glyph(int num, float x, float y) {
    if (num < 0 || num > 10) return;
    C2D_Image img = img_nums[num];
    float w = img.subtex->width;
    float h = img.subtex->height;
    C2D_DrawImageAt(img, floorf(x - w / 2.0f), floorf(y - h / 2.0f), 0.5f, NULL, 1.0f, 1.0f);
}

static void draw_sort_glyph(int mode, float x, float y) {
    if (mode < 0 || mode > 3) return;
    C2D_Image img = img_sort[mode];
    float w = img.subtex->width;
    float h = img.subtex->height;
    C2D_DrawImageAt(img, floorf(x - w / 2.0f), floorf(y - h / 2.0f), 0.5f, NULL, 1.0f, 1.0f);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    gfxInitDefault();

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    sheet0 = C2D_SpriteSheetLoadFromMem(_3dsDesignLayer0_t3x, _3dsDesignLayer0_t3x_size);
    sheet1 = C2D_SpriteSheetLoadFromMem(_3dsDesignLayer1_t3x, _3dsDesignLayer1_t3x_size);
    sheet_pip = C2D_SpriteSheetLoadFromMem(SelectionPip_t3x, SelectionPip_t3x_size);

    sheet_nums[0] = C2D_SpriteSheetLoadFromMem(_0_t3x, _0_t3x_size);
    sheet_nums[1] = C2D_SpriteSheetLoadFromMem(_1_t3x, _1_t3x_size);
    sheet_nums[2] = C2D_SpriteSheetLoadFromMem(_2_t3x, _2_t3x_size);
    sheet_nums[3] = C2D_SpriteSheetLoadFromMem(_3_t3x, _3_t3x_size);
    sheet_nums[4] = C2D_SpriteSheetLoadFromMem(_4_t3x, _4_t3x_size);
    sheet_nums[5] = C2D_SpriteSheetLoadFromMem(_5_t3x, _5_t3x_size);
    sheet_nums[6] = C2D_SpriteSheetLoadFromMem(_6_t3x, _6_t3x_size);
    sheet_nums[7] = C2D_SpriteSheetLoadFromMem(_7_t3x, _7_t3x_size);
    sheet_nums[8] = C2D_SpriteSheetLoadFromMem(_8_t3x, _8_t3x_size);
    sheet_nums[9] = C2D_SpriteSheetLoadFromMem(_9_t3x, _9_t3x_size);
    sheet_nums[10] = C2D_SpriteSheetLoadFromMem(_10_t3x, _10_t3x_size);
    for (int i = 0; i < 11; i++) img_nums[i] = C2D_SpriteSheetGetImage(sheet_nums[i], 0);

    sheet_sort[0] = C2D_SpriteSheetLoadFromMem(Br_t3x, Br_t3x_size);
    sheet_sort[1] = C2D_SpriteSheetLoadFromMem(R_t3x, R_t3x_size);
    sheet_sort[2] = C2D_SpriteSheetLoadFromMem(G_t3x, G_t3x_size);
    sheet_sort[3] = C2D_SpriteSheetLoadFromMem(B_t3x, B_t3x_size);
    for (int i = 0; i < 4; i++) img_sort[i] = C2D_SpriteSheetGetImage(sheet_sort[i], 0);

    if (!sheet0 || !sheet1 || !sheet_pip) {
        svcBreak(USERBREAK_PANIC);
    }

    img0 = C2D_SpriteSheetGetImage(sheet0, 0);
    img1 = C2D_SpriteSheetGetImage(sheet1, 0);
    img_pip = C2D_SpriteSheetGetImage(sheet_pip, 0);

    gfxSetDoubleBuffering(GFX_TOP, true);

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    load_wav(CameraShutter_wav, CameraShutter_wav_size, &snd_shutter);
    load_wav(OpenOptions_wav, OpenOptions_wav_size, &snd_open);
    load_wav(OptionValueChange_wav, OptionValueChange_wav_size, &snd_change);
    load_wav(SavedFile_wav, SavedFile_wav_size, &snd_save);

    init_cameras();

    CAMU_GetMaxBytes(&cam_transfer_unit, CAM_WIDTH, CAM_HEIGHT);
    if (cam_transfer_unit > 32768) cam_transfer_unit = 32768;

    cam_buf = malloc(CAM_FRAME_BYTES);

    if (!cam_buf) {
        camExit();
        gfxExit();
        return 1;
    }

    memset(cam_buf, 0, CAM_FRAME_BYTES);


    int menu_repeat_key = 0;
    int menu_repeat_frames = 0;
    bool touch_held = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        circlePosition stick;
        hidCircleRead(&stick);
        touchPosition touch;
        hidTouchRead(&touch);
        int closed_menu_with_x = 0;

        if (!(kHeld & KEY_TOUCH)) {
            touch_held = false;
        }

        if (kDown & KEY_TOUCH) {
            if (!menu_open) {
                // Main screen buttons
                if (touch.px >= 18 && touch.px <= 152 && touch.py >= 30 && touch.py <= 104) {
                    kDown |= KEY_A;
                } else if (touch.px >= 167 && touch.px <= 301 && touch.py >= 30 && touch.py <= 104) {
                    menu_open = 1;
                    menu_repeat_key = 0;
                    menu_repeat_frames = 0;
                    play_sound(&snd_open, 1);
                    touch_held = true;
                } else if (touch.px >= 18 && touch.px <= 152 && touch.py >= 108 && touch.py <= 182) {
                    kDown |= KEY_Y;
                }
            }
        }

        if (kDown & (KEY_START | KEY_B)) {
            menu_open = !menu_open;
            menu_repeat_key = 0;
            menu_repeat_frames = 0;
            play_sound(&snd_open, 1);
        }

        if (menu_open) {
            if (kDown & KEY_Y) {
                menu_open = 0;
                closed_menu_with_x = 1;
                menu_repeat_key = 0;
                menu_repeat_frames = 0;
                play_sound(&snd_open, 1);
            }

            if (kDown & KEY_DUP) {
                menu_index--;
                if (menu_index < 0) menu_index = MENU_ITEMS - 1;
                menu_repeat_key = 0;
                menu_repeat_frames = 0;
                play_sound(&snd_change, 1);
            }
            if (kDown & KEY_DDOWN) {
                menu_index++;
                if (menu_index >= MENU_ITEMS) menu_index = 0;
                menu_repeat_key = 0;
                menu_repeat_frames = 0;
                play_sound(&snd_change, 1);
            }

            int adjust = 0;
            if ((kDown & KEY_TOUCH) && !touch_held) {
                bool hit_button = false;
                for (int i = 0; i < MENU_ITEMS; i++) {
                    if (touch.py >= pip_y[i] - 15 && touch.py <= pip_y[i] + 15) {
                        if (touch.px >= 200 && touch.px <= 235) {
                            menu_index = i;
                            adjust = -1;
                            hit_button = true;
                            touch_held = true;
                            break;
                        } else if (touch.px >= 245 && touch.px <= 280) {
                            menu_index = i;
                            adjust = 1;
                            hit_button = true;
                            touch_held = true;
                            break;
                        }
                    }
                }

                if (!hit_button) {
                    /* Close menu if tapping outside the options frame [34, 41, 287, 217] */
                    if (!(touch.px >= 34 && touch.px <= 287 && touch.py >= 41 && touch.py <= 217)) {
                        menu_open = 0;
                        closed_menu_with_x = 1; /* reuse this to prevent sorting in the same frame */
                        menu_repeat_key = 0;
                        menu_repeat_frames = 0;
                        play_sound(&snd_open, 1);
                        touch_held = true;
                    }
                }
            }

            if (adjust == 0) {
                if (kDown & KEY_DLEFT) {
                    adjust = -1;
                    menu_repeat_key = KEY_DLEFT;
                    menu_repeat_frames = 0;
                } else if (kDown & KEY_DRIGHT) {
                    adjust = 1;
                    menu_repeat_key = KEY_DRIGHT;
                    menu_repeat_frames = 0;
                } else {
                    int held_key = 0;
                    int held_adjust = 0;

                    if (kHeld & KEY_DLEFT) {
                        held_key = KEY_DLEFT;
                        held_adjust = -1;
                    } else if (kHeld & KEY_DRIGHT) {
                        held_key = KEY_DRIGHT;
                        held_adjust = 1;
                    }

                    if (held_key) {
                        if (menu_repeat_key != held_key) {
                            menu_repeat_key = held_key;
                            menu_repeat_frames = 0;
                        } else {
                            menu_repeat_frames++;
                            if (menu_repeat_frames >= MENU_REPEAT_DELAY &&
                                ((menu_repeat_frames - MENU_REPEAT_DELAY) % MENU_REPEAT_RATE) == 0) {
                                adjust = held_adjust;
                            }
                        }
                    } else {
                        menu_repeat_key = 0;
                        menu_repeat_frames = 0;
                    }
                }
            }

            if (adjust != 0) {
                play_sound(&snd_change, 1);
            }

            if (adjust < 0) {
                if (menu_index == 0 && contrast > CONTRAST_MIN) {
                    contrast--;
                    top_dirty_frames = 2;
                } else if (menu_index == 1 && blur_strength > BLUR_MIN) {
                    blur_strength--;
                } else if (menu_index == 2) {
                    sort_metric--;
                    if (sort_metric < 0) sort_metric = SORT_METRIC_COUNT - 1;
                } else if (menu_index == 3 && sort_threshold_min > 0) {
                    sort_threshold_min--;
                } else if (menu_index == 4 && sort_threshold_max > 0) {
                    sort_threshold_max--;
                    if (sort_threshold_max < sort_threshold_min) sort_threshold_min = sort_threshold_max;
                }
            }
            if (adjust > 0) {
                if (menu_index == 0 && contrast < CONTRAST_MAX) {
                    contrast++;
                    top_dirty_frames = 2;
                } else if (menu_index == 1 && blur_strength < BLUR_MAX) {
                    blur_strength++;
                } else if (menu_index == 2) {
                    sort_metric++;
                    if (sort_metric >= SORT_METRIC_COUNT) sort_metric = 0;
                } else if (menu_index == 3 && sort_threshold_min < 10) {
                    sort_threshold_min++;
                    if (sort_threshold_min > sort_threshold_max) sort_threshold_max = sort_threshold_min;
                } else if (menu_index == 4 && sort_threshold_max < 10) {
                    sort_threshold_max++;
                }
            }
        } else {
            menu_repeat_key = 0;
            menu_repeat_frames = 0;
        }

        if (kDown & KEY_A) {
            selected_camera = (kHeld & KEY_R) ? 1 : 0;
            /* Show capture status only on the bottom two lines */
            gfxFlushBuffers();
            gspWaitForVBlank();

            Result cap = capture_camera(cam_buf, selected_camera);
            if (R_SUCCEEDED(cap)) {
                play_sound(&snd_shutter, 0);
                has_capture = 1;
                top_dirty_frames = 2;
            }
        }

        if (kDown & KEY_X) {
            apply_kernel_blur();
        }

        if (!menu_open && !closed_menu_with_x && step_pixel_sorter(kHeld, stick)) {
            top_dirty_frames = 1;
        }

        if (kDown & KEY_SELECT) {
            if (save_image_to_sd()) {
                play_sound(&snd_save, 1);
            }
            top_dirty_frames = 2;
        }

        // Render bottom screen UI
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(bottom_target, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(bottom_target);
        C2D_DrawImageAt(menu_open ? img1 : img0, 0, 0, 0.0f, NULL, 1.0f, 1.0f);
        if (menu_open) {
            C2D_DrawImageAt(img_pip, 38.0f, floorf(pip_y[menu_index] - 6.0f), 0.5f, NULL, 1.0f, 1.0f);

            float gx = 241.0f;
            draw_number_glyph(contrast, gx, pip_y[0]);
            draw_number_glyph(blur_strength, gx, pip_y[1]);
            draw_sort_glyph(sort_metric, gx, pip_y[2]);
            draw_number_glyph(sort_threshold_min, gx, pip_y[3]);
            draw_number_glyph(sort_threshold_max, gx, pip_y[4]);
        }
        C3D_FrameEnd(0);

        if (top_dirty_frames > 0) {
            draw_top_image();
            gfxFlushBuffers();
            gfxSwapBuffers();
            top_dirty_frames--;
        } else {
            gspWaitForVBlank();
        }
    }

    C2D_SpriteSheetFree(sheet0);
    C2D_SpriteSheetFree(sheet1);
    C2D_SpriteSheetFree(sheet_pip);
    for (int i = 0; i < 11; i++) C2D_SpriteSheetFree(sheet_nums[i]);
    for (int i = 0; i < 4; i++) C2D_SpriteSheetFree(sheet_sort[i]);

    C2D_Fini();
    C3D_Fini();

    ndspExit();
    linearFree(snd_shutter.data);
    linearFree(snd_open.data);
    linearFree(snd_change.data);
    linearFree(snd_save.data);

    free(cam_buf);
    camExit();
    gfxExit();
    return 0;
}
