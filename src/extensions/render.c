#include "manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool decode_rgba(const char *encoded, unsigned char *pixels, size_t size) {
    for (size_t at = 0; at < size; at += 3, encoded += 4) {
        int a = base64_digit(encoded[0]), b = base64_digit(encoded[1]);
        int c = base64_digit(encoded[2]), d = base64_digit(encoded[3]);
        size_t left = size - at;
        if (a < 0 || b < 0 ||
            (left >= 2 ? c < 0 : encoded[2] != '=' || (b & 15)) ||
            (left >= 3 ? d < 0 : encoded[3] != '=' || (left == 2 && (c & 3)))) return false;
        pixels[at] = (unsigned char)((a << 2) | (b >> 4));
        if (left >= 2) pixels[at + 1] = (unsigned char)((b << 4) | (c >> 2));
        if (left >= 3) pixels[at + 2] = (unsigned char)((c << 6) | d);
    }
    return true;
}

bool ced_extension_image_decode(const cJSON *value, CedExtensionImage *image, char *error) {
    static const char *const fields[] = {"width", "height", "rgba", "interactive", NULL};
    if (!ced_ext_json_fields(value, fields, error)) return false;
    uint64_t width, height;
    const cJSON *interactive = cJSON_GetObjectItemCaseSensitive(value, "interactive");
    const char *encoded = ced_ext_json_string(value, "rgba");
    if (!ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(value, "width"), &width) || !width || width > CED_EXTENSION_RENDER_EDGE ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(value, "height"), &height) || !height || height > CED_EXTENSION_RENDER_EDGE ||
        !encoded || (interactive && !cJSON_IsBool(interactive))) {
        snprintf(error, CED_ERROR, "Invalid extension preview dimensions or fields.");
        return false;
    }
    size_t size = (size_t)(width * height * 4);
    if (strlen(encoded) != 4 * ((size + 2) / 3)) {
        snprintf(error, CED_ERROR, "Incorrect extension preview pixel length.");
        return false;
    }
    if (!ced_rpc_reserve(size)) {
        snprintf(error, CED_ERROR, "ResourceLimit: extension preview memory.");
        return false;
    }
    unsigned char *pixels = malloc(size);
    if (!pixels || !decode_rgba(encoded, pixels, size)) {
        snprintf(error, CED_ERROR, "%s", pixels ? "Invalid base64 preview pixels." : "Could not allocate extension preview.");
        free(pixels);
        ced_rpc_release(size);
        return false;
    }
    *image = (CedExtensionImage){pixels, (int)width, (int)height, cJSON_IsTrue(interactive)};
    return true;
}

void ced_extension_image_free(CedExtensionImage *image) {
    if (image->pixels) ced_rpc_release((size_t)image->width * (size_t)image->height * 4);
    free(image->pixels);
    *image = (CedExtensionImage){0};
}
