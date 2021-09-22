#include <stdio.h>
#include <stdint.h>
#include "naomi/video.h"
#include "naomi/audio.h"
#include "naomi/maple.h"
#include "naomi/system.h"

void main()
{
    maple_init();
    video_init_simple();

    // Display status, since loading the binary can take awhile.
    video_fill_screen(rgb(48, 48, 48));
    video_draw_text(20, 20, rgb(255, 255, 255), "Loading AICA binary...");
    video_wait_for_vblank();
    video_display();

    // Load the AICA binary itself.
    load_aica_binary(AICA_DEFAULT_BINARY, AICA_DEFAULT_BINARY_SIZE);

    char buffer[64];
    unsigned int counter = 0;
    while ( 1 )
    {
        // Draw a few simple things on the screen.
        video_fill_screen(rgb(48, 48, 48));

        // Display a liveness counter that goes up 60 times a second.
        sprintf(buffer, "Aliveness counter: %d (%08X)", counter++, *((volatile uint32_t *)((SOUNDRAM_BASE | UNCACHED_MIRROR) + 0xF100)));
        video_draw_text(20, 20, rgb(200, 200, 20), buffer);
        video_wait_for_vblank();
        video_display();
    }
}

void test()
{
    video_init_simple();

    video_fill_screen(rgb(48, 48, 48));
    video_draw_text(320 - 56, 236, rgb(255, 255, 255), "test mode stub");
    video_wait_for_vblank();
    video_display();

    while ( 1 ) { ; }
}