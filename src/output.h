#ifndef CED_OUTPUT_H
#define CED_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

/* One bounded buffer shared by the window and its workers. */
#define CED_OUTPUT_CAP (64 * 1024)
void ced_output_write(const char *source, const char *bytes, size_t length);
void ced_output_text(const char *source, const char *text);
void ced_output_copy(char out[CED_OUTPUT_CAP], uint64_t *revision);
void ced_output_clear(void);

#endif
