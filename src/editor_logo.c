#include "editor_logo.h"
#include "cui_internal.h"
#include "ced_logo_data.h"

cui_texture ced_editor_logo_create(cui_ctx *ui) {
    int width, height;
    unsigned char *pixels = cui__png_decode(ced_logo_png, (int)sizeof ced_logo_png, &width, &height);
    if (!pixels) return 0;
    cui_texture logo = cui_image_create(ui, width, height, pixels);
    cui__png_free(pixels);
    return logo;
}
