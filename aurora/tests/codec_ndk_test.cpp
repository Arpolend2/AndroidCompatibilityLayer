#include "../../core/src/ndk/mg_media.h"
#include "../../core/src/bionic/mg_fd.h"
#include "../../core/src/drivers/mg_java.h"
#include "../codec_audio.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void *sym(const char *name) {
    const mg_sym *s = mg_media_table();
    for (; s->name; ++s) if (!strcmp(name, s->name)) return s->addr;
    return nullptr;
}
template<typename T> T get(const char *name) { return reinterpret_cast<T>(sym(name)); }
struct Info { i32 offset, size; i64 time; u32 flags; };
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    const char *videoMime = argc == 3 && !std::strcmp(argv[2], "hevc") ? "video/hevc" : "video/avc";
    auto exnew = get<void *(*)()>("AMediaExtractor_new");
    auto setfd = get<int (*)(void *, int, i64, i64)>("AMediaExtractor_setDataSourceFd");
    auto count = get<size_t (*)(void *)>("AMediaExtractor_getTrackCount");
    auto trackfmt = get<void *(*)(void *, size_t)>("AMediaExtractor_getTrackFormat");
    auto select = get<int (*)(void *, size_t)>("AMediaExtractor_selectTrack");
    auto unselect = get<int (*)(void *, size_t)>("AMediaExtractor_unselectTrack");
    auto track = get<int (*)(void *)>("AMediaExtractor_getSampleTrackIndex");
    auto sample = get<gssize (*)(void *, u8 *, size_t)>("AMediaExtractor_readSampleData");
    auto time = get<i64 (*)(void *)>("AMediaExtractor_getSampleTime");
    auto advance = get<u8 (*)(void *)>("AMediaExtractor_advance");
    auto seek = get<int (*)(void *, i64, int)>("AMediaExtractor_seekTo");
    auto exdelete = get<int (*)(void *)>("AMediaExtractor_delete");
    auto getstr = get<u8 (*)(void *, const char *, const char **)>("AMediaFormat_getString");
    auto fmtdelete = get<int (*)(void *)>("AMediaFormat_delete");
    auto create = get<void *(*)(const char *)>("AMediaCodec_createDecoderByType");
    auto configure = get<int (*)(void *, void *, void *, void *, u32)>("AMediaCodec_configure");
    auto start = get<int (*)(void *)>("AMediaCodec_start");
    auto flush = get<int (*)(void *)>("AMediaCodec_flush");
    auto in = get<gssize (*)(void *, i64)>("AMediaCodec_dequeueInputBuffer");
    auto getinput = get<u8 *(*)(void *, size_t, size_t *)>("AMediaCodec_getInputBuffer");
    auto queue = get<int (*)(void *, size_t, goff, size_t, u64, u32)>("AMediaCodec_queueInputBuffer");
    auto output = get<gssize (*)(void *, Info *, i64)>("AMediaCodec_dequeueOutputBuffer");
    auto release = get<int (*)(void *, size_t, u8)>("AMediaCodec_releaseOutputBuffer");
    auto destroy = get<int (*)(void *)>("AMediaCodec_delete");
    megler_codec_host codecHost = auroraMediaCodecCallbacks();
    mg_java_media_set_host(&codecHost);
    mg_java_media_set_checked_drain(auroraMediaCodecDrainChecked);
    mg_media_set_demux_host(auroraDemuxCallbacks());
    int fd = mg_fd_open_host(argv[1], MG_O_READ);
    struct stat st{};
    CHECK(fd >= 0 && !stat(argv[1], &st));
    void *ex = exnew();
    CHECK(setfd(ex, fd, 0, st.st_size) == 0 && count(ex) == 2);
    void *fmt = nullptr, *audioFmt = nullptr;
    size_t videoIndex = 0, audioIndex = 0;
    for (size_t i = 0; i < count(ex); ++i) {
        void *f = trackfmt(ex, i);
        const char *mime = nullptr;
        if (getstr(f, "mime", &mime) && !strcmp(mime, videoMime)) {
            fmt = f;
            videoIndex = i;
            CHECK(select(ex, i) == 0);
        } else if (getstr(f, "mime", &mime) && !strcmp(mime, "audio/mp4a-latm")) {
            audioFmt = f;
            audioIndex = i;
        } else fmtdelete(f);
    }
    CHECK(fmt && audioFmt);
    void *codec = create(videoMime);
    CHECK(codec && configure(codec, fmt, nullptr, nullptr, 0) == 0 && start(codec) == 0);
    int frames = 0, formats = 0;
    i64 previous = -1;
    bool ended = false;
    auto drain = [&]() {
        for (;;) {
            Info info{};
            gssize n = output(codec, &info, 0);
            if (n == -1) return true;
            if (n == -2) { formats++; continue; }
            if (n < 0) return false;
            if (info.flags & 4) ended = true;
            else {
                if (info.size != 128 * 72 * 4 || info.time < previous) return false;
                previous = info.time;
                frames++;
            }
            if (release(codec, n, 0) != 0) return false;
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        while (track(ex) >= 0) {
            CHECK(in(codec, 0) == 0);
            size_t cap = 0;
            u8 *buf = getinput(codec, 0, &cap);
            gssize n = sample(ex, buf, cap);
            CHECK(n > 0 && queue(codec, 0, 0, n, (u64)time(ex), 0) == 0);
            CHECK(drain());
            advance(ex);
        }
        CHECK(queue(codec, 0, 0, 0, 0, 4) == 0);
        CHECK(drain());
        CHECK(frames == (pass ? 5 : 10) && formats == 1 && ended);
        if (!pass) {
            CHECK(flush(codec) == 0 && seek(ex, 500000, 0) == 0);
            frames = 0;
            ended = false;
            previous = -1;
        }
    }
    destroy(codec);
    CHECK(unselect(ex, videoIndex) == 0 && select(ex, audioIndex) == 0 && seek(ex, 0, 0) == 0);
    codec = create("audio/mp4a-latm");
    CHECK(codec && configure(codec, audioFmt, nullptr, nullptr, 0) == 0 && start(codec) == 0);
    int pcmFrames = 0, audioFormats = 0;
    ended = false;
    auto drainAudio = [&]() {
        for (;;) {
            Info info{};
            gssize n = output(codec, &info, 0);
            if (n == -1) return true;
            if (n == -2) { audioFormats++; continue; }
            if (n < 0) return false;
            if (info.flags & 4) ended = true;
            else {
                if (info.size <= 0 || info.size % 2) return false;
                pcmFrames += info.size / 2;
            }
            if (release(codec, n, 0) != 0) return false;
        }
    };
    while (track(ex) >= 0) {
        CHECK(in(codec, 0) == 0);
        size_t cap = 0;
        u8 *buf = getinput(codec, 0, &cap);
        gssize n = sample(ex, buf, cap);
        CHECK(n > 0 && queue(codec, 0, 0, n, (u64)time(ex), 0) == 0);
        CHECK(drainAudio());
        advance(ex);
    }
    CHECK(queue(codec, 0, 0, 0, 0, 4) == 0 && drainAudio());
    CHECK(pcmFrames >= 48000 && pcmFrames <= 52000 && audioFormats == 1 && ended);
    destroy(codec);
    fmtdelete(audioFmt);
    fmtdelete(fmt);
    exdelete(ex);
    mg_fd_close(fd);
    return 0;
}
