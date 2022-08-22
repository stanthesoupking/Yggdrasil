#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "yggdrasil.h"
#include "SDL.h"

static inline Ygg_Fiber_Handle pretend_work_async(Ygg_Context*, Ygg_Priority);
void _pretend_work(Ygg_Context* context) {
	usleep(100000);
	pretend_work_async(context, Ygg_Priority_Normal);
}
ygg_fiber_declare(pretend_work, _pretend_work);

int main(int argc, const char * argv[]) {
	Ygg_Coordinator_Parameters parameters = {
		.thread_count = 8,
		.maximum_fibers = 4096,
		.maximum_counters = 1024,
		.maximum_intermediaries = 1024,
		.queue_capacity = 1024,
		.instrumentation_enabled = true,
	};
		
	Ygg_Coordinator* coordinator = ygg_coordinator_new(parameters);
	Ygg_Context* context = ygg_blocking_context_new(coordinator);
	
	for (unsigned int i = 0; i < 4; ++i) {
		pretend_work_async(context, Ygg_Priority_Normal);
	}
		
	SDL_Init(0);
	SDL_Window* window = SDL_CreateWindow("Yggdrasil Instrument Example", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	
	bool running = true;
	while (running) {
		SDL_PumpEvents();
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				running = false;
			}
		}
		
		SDL_Surface* surface = SDL_GetWindowSurface(window);
		SDL_LockSurface(surface);
		memset(surface->pixels, 0x00, surface->h * surface->w * 4);
		ygg_coordinator_draw_instrument(coordinator, surface->pixels, surface->w, surface->h, surface->w * 4);
		SDL_UnlockSurface(surface);
		
		SDL_UpdateWindowSurface(window);
	}
	
	SDL_DestroyWindow(window);
	
	ygg_blocking_context_destroy(context);
	ygg_coordinator_destroy(coordinator);
	
	return 0;
}
