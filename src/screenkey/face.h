#ifndef CCA_SCREENKEY_FACE_H
#define CCA_SCREENKEY_FACE_H

#include <stdbool.h>
#include <stdint.h>

#define FACE_WIDTH 128
#define FACE_HEIGHT 128
#define FACE_BYTES_PER_PIXEL 2

/*
 * Render rows [first_row, first_row + row_count) of the happy or sad face.
 * pixels must hold FACE_WIDTH * row_count RGB565 pixels, which are written
 * most-significant byte first, as the ST7735 consumes them.
 */
void face_render_rows(uint8_t *pixels, bool happy, int first_row, int row_count);

#endif
