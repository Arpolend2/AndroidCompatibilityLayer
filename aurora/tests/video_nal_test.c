#include "../../core/src/drivers/mg_java_media.c"
#include "../../core/tests/mg_codec_standin.h"

#include <assert.h>
#include <stdio.h>

static void count_nal(mg_mcodec *codec, const u8 *nal, size_t size, void *arg)
{
    int *count = (int *)arg;
    (void)codec;
    assert(nal && size);
    ++*count;
}

int main(void)
{
    u8 avcc[] = { 1, 0x42, 0, 0x1e, 0xff, 0xe1, 0, 2, 0x67, 1, 1, 0, 2, 0x68, 1 };
    const u8 annex[] = { 0, 0, 0, 1, 0x65, 1 };
    const u8 invalid[] = { 1, 0x65, 2, 0x41 };
    int width;
    mg_java_media_set_host(mg_codec_standin());
    for (width = 1; width <= 4; width *= 2) {
        u8 sample[8] = { 0 };
        size_t sizes[3] = { sizeof(avcc), 0, 0 };
        const u8 *csd[3] = { avcc, NULL, NULL };
        mg_mcodec *codec = mg_mcodec_create("video/avc");
        mg_mframe *frame;
        i64 info[3];
        avcc[4] = (u8)(0xfc | (width - 1));
        assert(codec && mg_mcodec_configure(codec, 64, 48, 0, 0, csd, sizes));
        sample[width - 1] = 2;
        sample[width] = 0x65;
        sample[width + 1] = 1;
        mg_mcodec_decode(codec, sample, (size_t)width + 2, 123, 4);
        frame = mg_mcodec_next_frame(codec, info);
        assert(frame && info[0] == 123);
        mg_mcodec_free_frame(frame);
        mg_mcodec_flush(codec);
        mg_mcodec_decode(codec, annex, sizeof(annex), 456, 4);
        frame = mg_mcodec_next_frame(codec, info);
        assert(frame && info[0] == 456);
        mg_mcodec_free_frame(frame);
        mg_mcodec_flush(codec);
        avcc[4] = 0xfd;
        mg_mcodec_decode(codec, avcc, sizeof(avcc), 0, 2);
        sample[0] = 0;
        sample[1] = 2;
        sample[2] = 0x65;
        sample[3] = 1;
        mg_mcodec_decode(codec, sample, 4, 789, 4);
        frame = mg_mcodec_next_frame(codec, info);
        assert(frame && info[0] == 789);
        mg_mcodec_free_frame(frame);
        mg_mcodec_destroy(codec);
    }
    {
        mg_mcodec codec = { 0 };
        u8 hvcc[23] = { 1 };
        const u8 sample[] = { 0, 2, 0x26, 1 };
        int count = 0;
        codec.kind = MEGLER_CODEC_H265;
        hvcc[21] = 0xfd;
        assert(each_record_nal(&codec, hvcc, sizeof(hvcc), count_nal, &count));
        assert(codec.nal_length_size == 2);
        hvcc[21] = 0xff;
        hvcc[22] = 1;
        assert(each_record_nal(&codec, hvcc, sizeof(hvcc), count_nal, &count));
        assert(codec.nal_length_size == 2);
        each_nal(&codec, sample, sizeof(sample), count_nal, &count);
        assert(count == 1);
        codec.nal_length_size = 1;
        count = 0;
        each_nal(&codec, invalid, sizeof(invalid), count_nal, &count);
        assert(count == 0);
        {
            const u8 multiple[] = { 1, 0x26, 2, 0x02, 1 };
            each_nal(&codec, multiple, sizeof(multiple), count_nal, &count);
            assert(count == 2);
        }
    }
    mg_java_media_set_host(NULL);
    puts("video NAL prefix handling passed");
    return 0;
}
