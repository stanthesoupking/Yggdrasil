#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "yggdrasil.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

#define MANDELBROT_ORIGIN_X -0.5
#define MANDELBROT_ORIGIN_Y 0.0
#define MANDELBROT_ZOOM 2.0
//#define MANDELBROT_ORIGIN_X -0.56
//#define MANDELBROT_ORIGIN_Y 0.54
//#define MANDELBROT_ZOOM 0.04

#define MANDELBROT_ITERATIONS 512
#define MANDELBROT_SIZE 16384
#define MANDELBROT_TILE_SIZE 1024
#if (MANDELBROT_SIZE % MANDELBROT_TILE_SIZE) > 0
#error MANDELBROT_SIZE must be a multiple of MANDELBROT_TILE_SIZE
#endif

typedef struct Mandelbrot_Tile {
	unsigned int tile_x;
	unsigned int tile_y;
	unsigned char* pixel_data;
} Mandelbrot_Tile;
void _mb_tile(Ygg_Context* context, Mandelbrot_Tile tile) {
	for (unsigned int y = 0; y < MANDELBROT_TILE_SIZE; ++y) {
		for (unsigned int x = 0; x < MANDELBROT_TILE_SIZE; ++x) {
			
			unsigned int image_x = (tile.tile_x * MANDELBROT_TILE_SIZE) + x;
			unsigned int image_y = (tile.tile_y * MANDELBROT_TILE_SIZE) + y;
			
			unsigned int iter = 0;
			{
				double c_x = MANDELBROT_ORIGIN_X + ((image_x / (double)MANDELBROT_SIZE) - 0.5) * MANDELBROT_ZOOM;
				double c_y = MANDELBROT_ORIGIN_Y + ((image_y / (double)MANDELBROT_SIZE) - 0.5) * MANDELBROT_ZOOM;
				double x_n = 0;
				double y_n = 0;
				double x_n_1;
				double y_n_1;
				for(unsigned int i = 0; i < MANDELBROT_ITERATIONS; ++i) {
					x_n_1 = pow(x_n, 2) - pow(y_n, 2) + c_x;
					y_n_1 = 2 * x_n * y_n + c_y;
					if (pow(x_n_1, 2) + pow(y_n_1, 2) > 4.0 ){
						iter = i;
						break;
					} else {
						x_n = x_n_1;
						y_n = y_n_1;
					}
				}
			}
			
			unsigned char* pixel = tile.pixel_data + image_x + (image_y * MANDELBROT_SIZE);
			*pixel = (unsigned char)((iter / (float)MANDELBROT_ITERATIONS) * 255.0f);
		}
	}
}
ygg_fiber_declare_in(mb_tile, _mb_tile, Mandelbrot_Tile);

void _mb_write(Ygg_Context* context, unsigned char* pixel_data) {
	stbi_write_bmp("output.bmp", MANDELBROT_SIZE, MANDELBROT_SIZE, 1, pixel_data);
}
ygg_fiber_declare_in(mb_write, _mb_write, unsigned char*);

void _mb_gen(Ygg_Context* context) {
	unsigned char* pixel_data = malloc(MANDELBROT_SIZE * MANDELBROT_SIZE);
	memset(pixel_data, 0x00, MANDELBROT_SIZE * MANDELBROT_SIZE);
	
	printf("Kicking off workers\n");
	Ygg_Counter_Handle counter = ygg_counter_new(ygg_context_coordinator(context));
	unsigned int tiles_size = MANDELBROT_SIZE / MANDELBROT_TILE_SIZE;
	for (unsigned int tile_y = 0; tile_y < tiles_size; ++tile_y) {
		for (unsigned int tile_x = 0; tile_x < tiles_size; ++tile_x) {
			Mandelbrot_Tile tile = (Mandelbrot_Tile) {
				.tile_x = tile_x,
				.tile_y = tile_y,
				.pixel_data = pixel_data,
			};
			Ygg_Fiber_Handle fiber = mb_tile_async(context, Ygg_Priority_Normal, tile);
			ygg_counter_await_completion(counter, fiber);
		}
	}
	printf("Waiting for worker completion\n");
	ygg_counter_wait(counter, context);
	ygg_counter_release(counter);
	
	printf("Writing to file\n");
	mb_write_sync(context, Ygg_Priority_Normal, pixel_data);
	
	printf("Finished\n");
	free(pixel_data);
}
ygg_fiber_declare(mb_gen, _mb_gen);

int main(int argc, const char * argv[]) {
	Ygg_Coordinator_Parameters parameters = {
		.thread_count = 8,
		.maximum_fibers = 4096,
		.maximum_counters = 1024,
		.maximum_intermediaries = 1024,
		.queue_capacity = 1024,
	};
	
	Ygg_Coordinator* coordinator = ygg_coordinator_new(parameters);
	Ygg_Context* context = ygg_blocking_context_new(coordinator);
	mb_gen_sync(context, Ygg_Priority_Normal);
	ygg_blocking_context_destroy(context);
	ygg_coordinator_destroy(coordinator);
	
	return 0;
}
