#include "../../../aurora/shared_microg_worker.c"

#include <assert.h>

int test_worker_graphics(const megler_gfx_host *graphics)
{
    unsigned char pixels[64 * 32 * 4] = {0};
    unsigned char packet[FRAME_HEADER_SIZE + sizeof(pixels)];
    megler_run_config run = {0};
    megler_gfx_paint paint = {0};
    int sockets[2];
    void *canvas;
    ssize_t size;
    const uint16_t text[] = {'A', 'b'};

    configure_worker_graphics(&run, graphics);
    if (!run.gfx.canvas_new || !run.gfx.draw_text || !run.window.present_pixels)
        return 1;
    canvas = run.gfx.canvas_new(run.gfx.user, pixels, 64, 32, 64 * 4);
    if (!canvas) return 2;
    paint.color = 0xff000000;
    paint.text_size = 20;
    paint.text_scale_x = 1;
    paint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    paint.filter_mode = -1;
    run.gfx.draw_color(run.gfx.user, canvas, 0xffffffff, MEGLER_GFX_BLEND_SRC);
    if (pixels[0] != 255 || pixels[1] != 255 || pixels[2] != 255 || pixels[3] != 255)
        return 5;
    run.gfx.draw_text(run.gfx.user, canvas, text, 2, 2, 24, &paint, NULL, NULL);
    if (run.gfx.flush) run.gfx.flush(run.gfx.user, canvas);
    {
        size_t i;
        int dark = 0;
        for (i = 0; i < sizeof(pixels); i += 4)
            if (pixels[i] < 128 && pixels[i + 1] < 128 && pixels[i + 2] < 128)
                ++dark;
        if (!dark) return 6;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) return 3;
    frame_fd = sockets[1];
    control_fd = -1;
    active_ui_channel = 42;
    frame_sequence = 0;
    ++ui_generation;
    run.window.present_pixels(run.window.user, pixels, 64, 32, 64 * 4);
    size = recv(sockets[0], packet, sizeof(packet), MSG_DONTWAIT);
    close(sockets[0]);
    close(sockets[1]);
    frame_fd = -1;
    active_ui_channel = 0;
    run.gfx.canvas_free(run.gfx.user, canvas);
    if (size != (ssize_t)sizeof(packet) || memcmp(packet, "MGUI", 4) ||
        read64(packet + 8) != 42 || read64(packet + 16) != 1 ||
        read32(packet + 24) != 64 || read32(packet + 28) != 32 ||
        read32(packet + 32) != 64 * 4 || read32(packet + 36) != sizeof(pixels) ||
        read32(packet + 40) != 0 || read32(packet + 44) != sizeof(pixels) ||
        memcmp(packet + FRAME_HEADER_SIZE, pixels, sizeof(pixels)))
        return 4;
    return 0;
}
