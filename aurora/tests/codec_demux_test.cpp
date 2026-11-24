#include "../codec_audio.h"
#include "../../core/src/ndk/mg_media.h"

#include <cstdio>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const mg_demux_host *host = auroraDemuxCallbacks();
    void *demux = host->open(argv[1]);
    CHECK(demux);
    CHECK(host->track_count(demux) == 2);
    int video = -1, audio = -1;
    for (int i = 0; i < host->track_count(demux); ++i) {
        mg_demux_track track{};
        CHECK(host->track(demux, i, &track));
        if (!std::strcmp(track.mime, "video/avc")) {
            video = i;
            CHECK(track.width == 128 && track.height == 72 && track.config_size > 0);
        } else if (!std::strcmp(track.mime, "audio/mp4a-latm")) {
            audio = i;
            CHECK(track.sample_rate == 48000 && track.channels == 1 && track.config_size > 0);
        }
    }
    CHECK(video >= 0 && audio >= 0);
    int videoPackets = 0, audioPackets = 0, keys = 0, reordered = 0;
    i64 previous = -1;
    mg_demux_sample sample{};
    int rc;
    while ((rc = host->read(demux, &sample)) > 0) {
        CHECK(sample.data && sample.size > 0 && sample.size <= 1048576);
        if (sample.track == video) {
            if (previous >= 0 && sample.time_us < previous) reordered++;
            previous = sample.time_us;
            videoPackets++;
            if (sample.flags & 1) keys++;
        } else if (sample.track == audio) audioPackets++;
        else CHECK(false);
    }
    CHECK(rc == 0 && videoPackets == 10 && audioPackets > 0 && keys >= 2 && reordered > 0);
    CHECK(host->seek(demux, 600000, 0));
    bool found = false;
    while ((rc = host->read(demux, &sample)) > 0) {
        if (sample.track == video) {
            CHECK((sample.flags & 1) && sample.time_us <= 600000);
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(!host->seek(demux, 0, 3));
    host->close(demux);
    return 0;
}
