#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

typedef unsigned char kz_pixel_t;

int CLAHE(kz_pixel_t *pImage,
          unsigned int uiXRes,
          unsigned int uiYRes,
          kz_pixel_t Min,
          kz_pixel_t Max,
          unsigned int uiNrX,
          unsigned int uiNrY,
          unsigned int uiNrBins,
          float fCliplimit);

static int read_token(FILE *fp, char *buffer, size_t size)
{
    int ch;
    size_t pos = 0;

    do
    {
        ch = fgetc(fp);
        if (ch == '#')
        {
            do
            {
                ch = fgetc(fp);
            } while (ch != EOF && ch != '\n');
        }
    } while (ch != EOF && isspace(ch));

    if (ch == EOF)
    {
        return -1;
    }

    do
    {
        if (pos + 1 >= size)
        {
            return -1;
        }
        buffer[pos++] = (char)ch;
        ch = fgetc(fp);
    } while (ch != EOF && !isspace(ch));

    buffer[pos] = '\0';
    return 0;
}

static unsigned char clamp_u8(int value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return (unsigned char)value;
}

static int load_ppm(const char *path, unsigned char **pixels, int *width, int *height)
{
    FILE *fp;
    char token[64];
    long pixel_count;

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        fprintf(stderr, "clahe_ppm: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (read_token(fp, token, sizeof(token)) != 0 || strcmp(token, "P6") != 0)
    {
        fprintf(stderr, "clahe_ppm: %s is not binary PPM P6\n", path);
        fclose(fp);
        return -1;
    }

    if (read_token(fp, token, sizeof(token)) != 0)
    {
        fclose(fp);
        return -1;
    }
    *width = atoi(token);

    if (read_token(fp, token, sizeof(token)) != 0)
    {
        fclose(fp);
        return -1;
    }
    *height = atoi(token);

    if (read_token(fp, token, sizeof(token)) != 0 || atoi(token) != 255)
    {
        fprintf(stderr, "clahe_ppm: unsupported max value in %s\n", path);
        fclose(fp);
        return -1;
    }

    if (*width <= 0 || *height <= 0)
    {
        fclose(fp);
        return -1;
    }

    pixel_count = (long)*width * (long)*height;
    *pixels = (unsigned char *)malloc((size_t)pixel_count * 3U);
    if (*pixels == NULL)
    {
        fclose(fp);
        return -1;
    }

    if (fread(*pixels, 3U, (size_t)pixel_count, fp) != (size_t)pixel_count)
    {
        fprintf(stderr, "clahe_ppm: failed to read pixel data from %s\n", path);
        free(*pixels);
        *pixels = NULL;
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

static int save_ppm(const char *path, const unsigned char *pixels, int width, int height)
{
    FILE *fp;
    long pixel_count = (long)width * (long)height;

    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        fprintf(stderr, "clahe_ppm: failed to write %s: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    if (fwrite(pixels, 3U, (size_t)pixel_count, fp) != (size_t)pixel_count)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

static int has_extension(const char *path, const char *extension)
{
    size_t path_len = strlen(path);
    size_t ext_len = strlen(extension);

    if (path_len < ext_len)
    {
        return 0;
    }

    return strcmp(path + path_len - ext_len, extension) == 0;
}

static int save_image(const char *path,
                      const unsigned char *pixels,
                      int width,
                      int height,
                      int jpeg_quality)
{
    if (has_extension(path, ".jpg") || has_extension(path, ".jpeg"))
    {
        if (!stbi_write_jpg(path, width, height, 3, pixels, jpeg_quality))
        {
            fprintf(stderr, "clahe_ppm: failed to write JPEG %s\n", path);
            return -1;
        }
        return 0;
    }

    return save_ppm(path, pixels, width, height);
}

static int apply_luminance_clahe(unsigned char *pixels,
                                 int width,
                                 int height,
                                 unsigned int tiles_x,
                                 unsigned int tiles_y,
                                 float clip_limit)
{
    long pixel_count = (long)width * (long)height;
    unsigned char *before;
    unsigned char *after;
    long i;
    int status;

    if (width % (int)tiles_x != 0 || height % (int)tiles_y != 0)
    {
        fprintf(stderr,
                "clahe_ppm: image %dx%d must be divisible by tile grid %ux%u\n",
                width,
                height,
                tiles_x,
                tiles_y);
        return -1;
    }

    before = (unsigned char *)malloc((size_t)pixel_count);
    after = (unsigned char *)malloc((size_t)pixel_count);
    if (before == NULL || after == NULL)
    {
        free(before);
        free(after);
        return -1;
    }

    for (i = 0; i < pixel_count; i++)
    {
        int r = pixels[i * 3 + 0];
        int g = pixels[i * 3 + 1];
        int b = pixels[i * 3 + 2];
        before[i] = clamp_u8((77 * r + 150 * g + 29 * b + 128) >> 8);
        after[i] = before[i];
    }

    status = CLAHE(after, (unsigned int)width, (unsigned int)height,
                   0, 255, tiles_x, tiles_y, 256, clip_limit);
    if (status != 0)
    {
        fprintf(stderr, "clahe_ppm: CLAHE failed status=%d\n", status);
        free(before);
        free(after);
        return -1;
    }

    for (i = 0; i < pixel_count; i++)
    {
        int delta = (int)after[i] - (int)before[i];
        pixels[i * 3 + 0] = clamp_u8((int)pixels[i * 3 + 0] + delta);
        pixels[i * 3 + 1] = clamp_u8((int)pixels[i * 3 + 1] + delta);
        pixels[i * 3 + 2] = clamp_u8((int)pixels[i * 3 + 2] + delta);
    }

    free(before);
    free(after);
    return 0;
}

int main(int argc, char **argv)
{
    const char *input_path;
    const char *output_path;
    unsigned char *pixels = NULL;
    int width = 0;
    int height = 0;
    unsigned int tiles_x = 8;
    unsigned int tiles_y = 8;
    float clip_limit = 2.0f;
    int jpeg_quality = 95;
    int result = 1;

    if (argc < 3)
    {
        fprintf(stderr,
                "Usage: %s input.ppm output.ppm|output.jpg [clip_limit] [tiles_x] [tiles_y] [jpeg_quality]\n",
                argv[0]);
        return 2;
    }

    input_path = argv[1];
    output_path = argv[2];

    if (argc > 3)
    {
        clip_limit = (float)atof(argv[3]);
    }
    if (argc > 4)
    {
        tiles_x = (unsigned int)atoi(argv[4]);
    }
    if (argc > 5)
    {
        tiles_y = (unsigned int)atoi(argv[5]);
    }
    if (argc > 6)
    {
        jpeg_quality = atoi(argv[6]);
        if (jpeg_quality < 1 || jpeg_quality > 100)
        {
            fprintf(stderr, "clahe_ppm: jpeg_quality must be 1..100\n");
            return 2;
        }
    }

    if (load_ppm(input_path, &pixels, &width, &height) != 0)
    {
        return 1;
    }

    if (apply_luminance_clahe(pixels, width, height, tiles_x, tiles_y, clip_limit) != 0)
    {
        goto done;
    }

    if (save_image(output_path, pixels, width, height, jpeg_quality) != 0)
    {
        goto done;
    }

    printf("clahe_ppm: processed %dx%d clip=%.3f tiles=%ux%u jpeg_quality=%d\n",
           width,
           height,
           clip_limit,
           tiles_x,
           tiles_y,
           jpeg_quality);
    result = 0;

done:
    free(pixels);
    return result;
}
