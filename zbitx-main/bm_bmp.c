#include "bm_bmp.h"

void setPixel(struct BM_BMP* pBmp, int col, int row, uint16_t color) {
    int ix = row * pBmp->width_in_bytes + col / (8 / pBmp->infohdr.biBitCount);
    if (ix >= pBmp->infohdr.biSizeImage) {
        ix = pBmp->infohdr.biSizeImage - 1;
    }

    uint8_t msk = 0xf0 >> (col & 0x1) * 4;
    uint8_t b = pBmp->imageBuffer[ix];
    uint8_t clr = (color << 4) >> (col & 0x1) * 4;
    pBmp->imageBuffer[ix] = (b & ~msk) | clr;
}


void setColor(struct BM_BMP *pBmp, uint8_t ix, uint8_t r, uint8_t g, uint8_t b) {
    pBmp->colors[ix].r = r;
    pBmp->colors[ix].g = g;
    pBmp->colors[ix].b = b;
    pBmp->colors[ix].t = 0;
}

struct BM_BMP* bm_create16bitBmp(uint32_t width, uint32_t height)
{
#define BM_NUM_COLORS 16
#define BM_BIT_COUNT 4

    if (sizeof(struct BM_BMPFILEHEADER) != 14 && sizeof(struct BM_BMPINFOHEADER) != 40)
    {
        printf("bitmap structures not packed properly\n");
        return NULL;
    }

    struct BM_BMP* pBmp = malloc(sizeof(struct BM_BMP));
    memset(pBmp, 0, sizeof(struct BM_BMP));
    pBmp->width_in_bytes = ((width * BM_BIT_COUNT + 31) / 32) * 4;    // for padding
    uint32_t imageSize = pBmp->width_in_bytes * height;
    pBmp->filehdr.bfSize = 54 + BM_NUM_COLORS * 4 + imageSize;//total file size
    pBmp->filehdr.bfOffBits = 54 + BM_NUM_COLORS * 4; //sizeof(filehdr) + sizeof(infohdr + colors
    memcpy(&pBmp->filehdr, "BM", 2);//bitmap signature
    pBmp->filehdr.bfSize = 54 + BM_NUM_COLORS * 4 + imageSize; //total file size
    pBmp->filehdr.bfOffBits = 54 + BM_NUM_COLORS * 4; //sizeof(filehdr) + sizeof(infohdr + colors

    pBmp->infohdr.biSize = 40; //sizeof(infohdr)
    pBmp->infohdr.biPlanes = 1; //number of planes is usually 1
    pBmp->infohdr.biWidth = width;
    pBmp->infohdr.biHeight = height;
    pBmp->infohdr.biBitCount = BM_BIT_COUNT;
    pBmp->infohdr.biSizeImage = imageSize;
    pBmp->infohdr.biClrUsed = BM_NUM_COLORS;
    struct BM_COLOR colors[BM_NUM_COLORS];
    pBmp->colorCount = BM_NUM_COLORS;
    pBmp->colors = (struct BM_COLOR*)malloc(sizeof(struct BM_COLOR) * BM_NUM_COLORS);
    pBmp->imageBuffer = malloc(imageSize);

    setColor(pBmp, 0, 255, 255, 255);
    setColor(pBmp, 1, 200, 10, 10);
    setColor(pBmp, 2, 20, 200, 20);
    setColor(pBmp, 3, 20, 20, 200);
    setColor(pBmp, 4, 250, 250, 150);
    setColor(pBmp, 14, 200, 200, 10);
    setColor(pBmp, 15, 10, 200, 200);

    for (int row = height - 1; row >= 0; row--)
    {
        for (int col = 0; col < width; col++)
        {

            int ix = row * pBmp->width_in_bytes + col / (8 / BM_BIT_COUNT);
            if (ix >= imageSize) {
                ix = imageSize - 1;
            }

            uint8_t msk = 0xf0 >> (col & 0x1) * 4;
            uint8_t b = pBmp->imageBuffer[ix];
            uint8_t clr = 0;

            if (((col / 10) & 1) == 0 && ((row / 10) & 1) == 1) {
                clr = 4;
            }
            if (((col / 10) & 1) == 1 && ((row / 10) & 1) == 0) {
                clr = 4;
            }

            clr = (clr << 4) >> (col & 0x1) * 4;
            pBmp->imageBuffer[ix] = (b & ~msk) | clr;

        }
    }

    return pBmp;
}

void bm_free(struct BM_BMP* pBmp)
{
    free(pBmp->colors);
    free(pBmp->imageBuffer);
    free(pBmp);
}

void bm_save(struct BM_BMP* pBmp, char* fileName)
{
    FILE* fout = fopen(fileName, "wb");
    fwrite(&pBmp->filehdr, sizeof(struct BM_BMPFILEHEADER), 1, fout);
    fwrite(&pBmp->infohdr, sizeof(struct BM_BMPINFOHEADER), 1, fout);
    fwrite(pBmp->colors, sizeof(struct BM_COLOR) * pBmp->infohdr.biClrUsed, 1, fout);
    fwrite(pBmp->imageBuffer, 1, pBmp->infohdr.biSizeImage, fout);
    fclose(fout);
}

struct BM_BMP* bm_load(char* fileName)
{
    if (sizeof(struct BM_BMPFILEHEADER) != 14 && sizeof(struct BM_BMPINFOHEADER) != 40)
    {
        printf("bitmap structures not packed properly\n");
        return NULL;
    }

    FILE* fptr;
    struct BM_BMP* pBmp;

#pragma warning(suppress : 4996)
    fptr = fopen(fileName, "rb");
    if (fptr == NULL)
    {
        printf("Cannot open file \n");
        exit(0);
    }
    pBmp = malloc(sizeof(struct BM_BMP));
    if (pBmp == NULL) {
        fclose(fptr);
        exit(0);
    }
    memset(pBmp, 0, sizeof(struct BM_BMP));

    fread(pBmp, 1, 54, fptr);

    if (strncmp((char*)pBmp, "BM", 2)) {
        printf("File format not supported");
        free(pBmp);
        pBmp = NULL;
    }
    else {
        printf("w:%d h:%d size:%d" , pBmp->infohdr.biWidth, pBmp->infohdr.biHeight, pBmp->infohdr.biSize);

        pBmp->width_in_bytes = ((pBmp->infohdr.biWidth * pBmp->infohdr.biBitCount + 31) / 32) * 4;
        uint32_t imageSize = pBmp->width_in_bytes * pBmp->infohdr.biHeight;
        pBmp->infohdr.biSizeImage = imageSize;

        pBmp->colorCount = pBmp->infohdr.biClrUsed;
        int colorsSize = sizeof(struct BM_COLOR) * pBmp->infohdr.biClrUsed;
        pBmp->colors = (struct BM_COLOR*)malloc(colorsSize);
        long l = fread((void*)pBmp->colors, 1, colorsSize, fptr);

        pBmp->imageBuffer = malloc(imageSize);
        l = fread((void *)pBmp->imageBuffer, 1, imageSize, fptr);
        if (l < imageSize) {
            printf("read error %d ", l - imageSize);
        }

    }

    fclose(fptr);
    return pBmp;
}