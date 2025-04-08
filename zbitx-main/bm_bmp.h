#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>


#pragma pack(push, 1)
struct BM_BMPFILEHEADER {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};

struct BM_BMPINFOHEADER {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

struct BM_COLOR {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t t;
};

struct BM_BMP {
    struct BM_BMPFILEHEADER filehdr;
    struct BM_BMPINFOHEADER infohdr;
    uint32_t width_in_bytes;
    uint32_t colorCount;
    struct BM_COLOR* colors;
    uint8_t* imageBuffer;
};


#pragma pack(pop)

struct BM_BMP* bm_create16bitBmp(uint32_t width, uint32_t height);
void bm_free(struct BM_BMP* pBmp);
void bm_save(struct BM_BMP* pBmp, char* fileName);
void setPixel(struct BM_BMP* pBmp, int col, int row, uint16_t color);
void setColor(struct BM_BMP* pBmp, uint8_t ix, uint8_t r, uint8_t g, uint8_t b);
struct BM_BMP* bm_load(char* fileName);

