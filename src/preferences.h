#ifndef CED_PREFERENCES_H
#define CED_PREFERENCES_H

#include <stdbool.h>

#define CED_FONT_SIZE_DEFAULT 14
#define CED_FONT_SIZE_MIN 8
#define CED_FONT_SIZE_MAX 32

int ced_font_size_load(void);
bool ced_font_size_save(int size);

#endif
