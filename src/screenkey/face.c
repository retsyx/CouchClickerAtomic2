#include "face.h"

struct rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/* Coordinates below use quarter-pixels for smooth, integer-only rendering. */
static struct rgb sample(int x, int y, bool happy)
{
	const struct rgb background = happy ? (struct rgb){30, 66, 49}
					   : (struct rgb){72, 38, 43};
	const struct rgb yellow = {250, 209, 69};
	const struct rgb ink = {69, 49, 24};
	const int dx = x - 256;
	const int dy = y - 256;
	const int eye_y = y - 200;
	const int left_eye_x = x - 196;
	const int right_eye_x = x - 316;
	const int mouth_y = y - (happy ? 228 : 416);
	const int mouth_radius_sq = dx * dx + mouth_y * mouth_y;

	if (dx * dx + dy * dy > 172 * 172) {
		return background;
	}
	if (left_eye_x * left_eye_x + eye_y * eye_y <= 17 * 17 ||
	    right_eye_x * right_eye_x + eye_y * eye_y <= 17 * 17) {
		return ink;
	}
	if (mouth_radius_sq >= 88 * 88 && mouth_radius_sq <= 104 * 104 &&
	    (happy ? mouth_y >= 28 : mouth_y <= -48)) {
		return ink;
	}
	return yellow;
}

void face_render_rows(uint8_t *pixels, bool happy, int first_row, int row_count)
{
	for (int row = 0; row < row_count; ++row) {
		const int y = first_row + row;

		for (int x = 0; x < FACE_WIDTH; ++x) {
			unsigned int r = 0;
			unsigned int g = 0;
			unsigned int b = 0;

			/* Four samples per pixel antialias the circle, eyes and mouth. */
			for (int sy = 1; sy <= 3; sy += 2) {
				for (int sx = 1; sx <= 3; sx += 2) {
					struct rgb color = sample(x * 4 + sx, y * 4 + sy, happy);

					r += color.r;
					g += color.g;
					b += color.b;
				}
			}

			const uint16_t color = ((r / 4U) >> 3U) << 11U |
					       ((g / 4U) >> 2U) << 5U | ((b / 4U) >> 3U);
			const unsigned int offset = (row * FACE_WIDTH + x) * FACE_BYTES_PER_PIXEL;

			pixels[offset] = color >> 8U;
			pixels[offset + 1U] = color & 0xffU;
		}
	}
}
