#define ACL_DROID_STOP_TIMEOUT_MS 200
#include <libavcodec/droidmediadec.c>

#include <stdio.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failure line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static DroidMediaCodecMetaData metadata;
static DroidMediaRect rectangle;
static DroidMediaCodecMetaData requested_metadata;

static bool reject_codec(DroidMediaCodecMetaData *meta, bool encoder)
{
    (void)encoder;
    requested_metadata = *meta;
    return false;
}

const DroidMediaAPI *ff_droidmedia_get_api(void)
{
    static const DroidMediaAPI api = { .is_supported = reject_codec };
    return &api;
}

static int test_requested_metadata(void)
{
    DroidMediaDecContext decoder = { 0 };
    AVCodecContext *context = avcodec_alloc_context3(NULL);
    CHECK(context);
    context->priv_data = &decoder;
    context->codec_id = AV_CODEC_ID_H264;
    context->width = 640;
    context->height = 360;
    CHECK(create_state(context) == AVERROR(ENOSYS));
    CHECK(requested_metadata.width == 640 && requested_metadata.height == 360);
    CHECK(requested_metadata.fps == -1 && requested_metadata.channels == -1 &&
          requested_metadata.sample_rate == -1);
    context->framerate = (AVRational){ 30, 1 };
    CHECK(create_state(context) == AVERROR(ENOSYS));
    CHECK(requested_metadata.fps == 30);
    context->framerate = (AVRational){ 1, 2 };
    CHECK(create_state(context) == AVERROR(ENOSYS));
    CHECK(requested_metadata.fps == -1);
    context->priv_data = NULL;
    avcodec_free_context(&context);
    return 0;
}

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int entered, release_callback, callback_done, stopped, destroyed, premature;
static pthread_t callback_thread;
static DroidMediaDecState *callback_state;
static uint8_t callback_bytes[24];
static DroidMediaCodecData callback_input = {
    .data = { callback_bytes, sizeof(callback_bytes) }, .ts = 1000000
};

static void callback_output_info(DroidMediaCodec *codec, DroidMediaCodecMetaData *out, DroidMediaRect *crop)
{
    (void)codec;
    pthread_mutex_lock(&gate);
    entered = 1;
    pthread_cond_broadcast(&changed);
    while (!release_callback)
        pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    out->width = 4;
    out->height = 4;
    out->stride = 4;
    out->slice_height = 4;
    out->hal_format = DM_COLOR_YUV420_PLANAR;
    crop->right = crop->bottom = 4;
}

static void *send_callback(void *unused)
{
    (void)unused;
    data_available(callback_state, &callback_input);
    pthread_mutex_lock(&gate);
    callback_done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}

static void stop_codec(DroidMediaCodec *codec)
{
    (void)codec;
    pthread_mutex_lock(&gate);
    if (!release_callback)
        premature = 1;
    stopped++;
    pthread_mutex_unlock(&gate);
    data_available(callback_state, &callback_input);
    signal_eos(callback_state);
    pthread_join(callback_thread, NULL);
}

static void destroy_codec(DroidMediaCodec *codec)
{
    (void)codec;
    destroyed++;
}

static void *close_decoder(void *opaque)
{
    close_state(opaque);
    return NULL;
}

static int test_callback_shutdown(void)
{
    static const DroidMediaAPI api = {
        .get_output_info = callback_output_info, .stop = stop_codec, .destroy = destroy_codec
    };
    pthread_t close_thread;
    int observed_closing = 0;
    callback_state = av_mallocz(sizeof(*callback_state));
    CHECK(callback_state);
    callback_state->api = &api;
    callback_state->codec = (DroidMediaCodec *)&api;
    callback_state->refs = 1;
    CHECK(!pthread_mutex_init(&callback_state->lock, NULL));
    CHECK(!pthread_cond_init(&callback_state->cond, NULL));
    CHECK(!pthread_create(&callback_thread, NULL, send_callback, NULL));
    pthread_mutex_lock(&gate);
    while (!entered)
        pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    CHECK(!pthread_create(&close_thread, NULL, close_decoder, callback_state));
    for (int i = 0; i < 1000; i++) {
        int closing;
        pthread_mutex_lock(&callback_state->lock);
        closing = callback_state->closing;
        pthread_mutex_unlock(&callback_state->lock);
        if (closing) {
            observed_closing = 1;
            break;
        }
        usleep(1000);
    }
    CHECK(observed_closing);
    pthread_mutex_lock(&gate);
    CHECK(!stopped);
    release_callback = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    CHECK(!pthread_join(close_thread, NULL));
    CHECK(callback_done && stopped == 1 && destroyed == 1 && !premature);
    return 0;
}

static void *send_frame_and_eos(void *opaque)
{
    DroidMediaDecState *state = opaque;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return (void *)1;
    frame->pts = 1234;
    usleep(10000);
    if (queue_push(state, frame) < 0) {
        av_frame_free(&frame);
        return (void *)1;
    }
    usleep(10000);
    signal_eos(state);
    return NULL;
}

static void *finish_active_callback(void *opaque)
{
    DroidMediaDecState *state = opaque;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return (void *)1;
    frame->pts = 5678;
    usleep(10000);
    if (queue_push(state, frame) < 0) {
        av_frame_free(&frame);
        return (void *)1;
    }
    pthread_mutex_lock(&state->lock);
    state->data_callbacks--;
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->lock);
    return NULL;
}

static void no_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
}

static int test_drain(void)
{
    DroidMediaDecState state = { 0 };
    pthread_condattr_t attr;
    pthread_t thread;
    void *result;
    uint8_t dummy = 0;
    AVFrame *frame = av_frame_alloc();
    CHECK(frame);
    CHECK(!pthread_mutex_init(&state.lock, NULL));
    CHECK(!pthread_condattr_init(&attr));
    CHECK(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    CHECK(!pthread_cond_init(&state.cond, &attr));
    CHECK(!pthread_condattr_destroy(&attr));
    CHECK(!pthread_create(&thread, NULL, send_frame_and_eos, &state));
    CHECK(wait_for_drain(&state, frame, 1000) == 0);
    CHECK(frame->pts == 1234);
    av_frame_unref(frame);
    CHECK(wait_for_drain(&state, frame, 1000) == AVERROR_EOF);
    CHECK(!pthread_join(thread, &result) && !result);
    state.eos = 1;
    state.data_callbacks = 1;
    CHECK(queue_pop(&state, frame) == AVERROR(EAGAIN));
    CHECK(!pthread_create(&thread, NULL, finish_active_callback, &state));
    CHECK(wait_for_drain(&state, frame, 1000) == 0);
    CHECK(frame->pts == 5678);
    av_frame_unref(frame);
    CHECK(!pthread_join(thread, &result) && !result);
    CHECK(wait_for_drain(&state, frame, 1000) == AVERROR_EOF);
    state.eos = 0;
    CHECK(wait_for_drain(&state, frame, 20) == AVERROR_EXTERNAL);
    CHECK(state.error == AVERROR(ETIMEDOUT));
    state.error = 0;
    for (int i = 0; i < 64; ++i) {
        AVFrame *queued = av_frame_alloc();
        CHECK(queued && queue_push(&state, queued) == 0);
    }
    AVFrame *overflow = av_frame_alloc();
    CHECK(overflow && queue_push(&state, overflow) == AVERROR(ENOBUFS));
    av_frame_free(&overflow);
    clear_queued_frames(&state);
    state.error = 0;
    AVFrame *large = av_frame_alloc();
    AVFrame *small = av_frame_alloc();
    CHECK(large && small);
    large->buf[0] = av_buffer_create(&dummy, 256 * 1024 * 1024, no_free, NULL, 0);
    small->buf[0] = av_buffer_create(&dummy, 1, no_free, NULL, 0);
    CHECK(large->buf[0] && small->buf[0]);
    CHECK(queue_push(&state, large) == 0);
    CHECK(queue_push(&state, small) == AVERROR(ENOBUFS));
    av_frame_free(&small);
    clear_queued_frames(&state);
    CHECK(state.queued_bytes == 0);
    pthread_cond_destroy(&state.cond);
    pthread_mutex_destroy(&state.lock);
    av_frame_free(&frame);
    return 0;
}

static DroidMediaBufferCallbacks input_callbacks[5];
static int input_count, input_stops, input_destroys;

static void input_queue(DroidMediaCodec *codec, DroidMediaCodecData *data,
                        DroidMediaBufferCallbacks *cb)
{
    (void)codec;
    (void)data;
    input_callbacks[input_count % 5] = *cb;
    input_count++;
}

static void input_stop(DroidMediaCodec *codec)
{
    (void)codec;
    input_stops++;
    for (int i = 0; i < 5; i++) {
        if (input_callbacks[i].unref) {
            input_callbacks[i].unref(input_callbacks[i].data);
            input_callbacks[i].unref = NULL;
        }
    }
}

static void input_destroy(DroidMediaCodec *codec)
{
    (void)codec;
    input_destroys++;
}

typedef struct InputWaiter {
    DroidMediaDecState *state;
    AVPacket *packet;
    int timeout_ms;
    int result;
} InputWaiter;

static void *queue_waiter(void *opaque)
{
    InputWaiter *waiter = opaque;
    waiter->result = queue_input(waiter->state, waiter->packet, 0, waiter->timeout_ms);
    return NULL;
}

static int test_input_backpressure(int action)
{
    static const DroidMediaAPI api = {
        .queue = input_queue, .stop = input_stop, .destroy = input_destroy
    };
    pthread_condattr_t attr;
    pthread_t thread;
    int waiting = 0;
    struct timespec begin, end;
    InputWaiter waiter = { 0 };
    DroidMediaDecState *state = av_mallocz(sizeof(*state));
    AVPacket *packet = av_packet_alloc();
    CHECK(state && packet && av_new_packet(packet, 1) == 0);
    input_count = input_stops = input_destroys = 0;
    memset(input_callbacks, 0, sizeof(input_callbacks));
    state->api = &api;
    state->codec = (DroidMediaCodec *)&api;
    state->refs = 1;
    CHECK(!pthread_mutex_init(&state->lock, NULL));
    CHECK(!pthread_condattr_init(&attr));
    CHECK(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    CHECK(!pthread_cond_init(&state->cond, &attr));
    CHECK(!pthread_condattr_destroy(&attr));
    CHECK(queue_input(state, packet, INT64_MAX, 1000) == AVERROR(EINVAL));
    CHECK(queue_input(state, packet, INT64_MIN, 1000) == AVERROR(EINVAL));
    CHECK(input_count == 0);
    for (int i = 0; i < 5; i++)
        CHECK(queue_input(state, packet, i, 1000) == 0);
    CHECK(input_count == 5 && state->input_inflight == 5);
    waiter.state = state;
    waiter.packet = packet;
    waiter.timeout_ms = action == 2 ? 100 : 1000;
    CHECK(!pthread_create(&thread, NULL, queue_waiter, &waiter));
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&state->lock);
        waiting = state->refs == 7;
        pthread_mutex_unlock(&state->lock);
        if (waiting)
            break;
        usleep(1000);
    }
    CHECK(waiting);
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &begin));
    if (action == 1) {
        close_state(state);
    } else {
        if (action == 0)
            codec_error(state, AVERROR_EXTERNAL);
        CHECK(!pthread_join(thread, NULL));
        CHECK(waiter.result == AVERROR_EXTERNAL);
        if (action == 2)
            CHECK(state->error == AVERROR(ETIMEDOUT));
        close_state(state);
    }
    if (action == 1) {
        CHECK(!pthread_join(thread, NULL));
        CHECK(waiter.result == AVERROR_EOF);
    }
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &end));
    CHECK((end.tv_sec - begin.tv_sec) * 1000000000LL +
          end.tv_nsec - begin.tv_nsec < 1000000000LL);
    CHECK(input_count == 5 && input_stops == 1 && input_destroys == 1);
    av_packet_free(&packet);
    return 0;
}

static int test_input_resume(void)
{
    static const DroidMediaAPI api = {
        .queue = input_queue, .stop = input_stop, .destroy = input_destroy
    };
    pthread_condattr_t attr;
    DroidMediaDecState *state = av_mallocz(sizeof(*state));
    AVPacket *packet = av_packet_alloc();
    CHECK(state && packet && av_new_packet(packet, 1) == 0);
    input_count = input_stops = input_destroys = 0;
    memset(input_callbacks, 0, sizeof(input_callbacks));
    state->api = &api;
    state->codec = (DroidMediaCodec *)&api;
    state->refs = 1;
    CHECK(!pthread_mutex_init(&state->lock, NULL));
    CHECK(!pthread_condattr_init(&attr));
    CHECK(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    CHECK(!pthread_cond_init(&state->cond, &attr));
    CHECK(!pthread_condattr_destroy(&attr));
    for (int i = 0; i < 5; i++)
        CHECK(queue_input(state, packet, i, 1000) == 0);
    input_callbacks[0].unref(input_callbacks[0].data);
    input_callbacks[0].unref = NULL;
    CHECK(queue_input(state, packet, 6, 1000) == 0);
    CHECK(input_count == 6 && state->input_inflight == 5);
    close_state(state);
    CHECK(input_stops == 1 && input_destroys == 1);
    av_packet_free(&packet);
    return 0;
}

static pthread_mutex_t hung_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hung_cond = PTHREAD_COND_INITIALIZER;
static int hung_entered, hung_release, hung_returned;

static void hung_stop(DroidMediaCodec *codec)
{
    (void)codec;
    pthread_mutex_lock(&hung_lock);
    hung_entered = 1;
    pthread_cond_broadcast(&hung_cond);
    while (!hung_release)
        pthread_cond_wait(&hung_cond, &hung_lock);
    hung_returned = 1;
    pthread_cond_broadcast(&hung_cond);
    pthread_mutex_unlock(&hung_lock);
}

static int test_hung_stop(void)
{
    static const DroidMediaAPI api = { .stop = hung_stop, .destroy = input_destroy };
    struct timespec begin, end;
    DroidMediaDecState *state = av_mallocz(sizeof(*state));
    CHECK(state);
    state->api = &api;
    state->codec = (DroidMediaCodec *)&api;
    state->refs = 1;
    CHECK(!pthread_mutex_init(&state->lock, NULL));
    CHECK(!pthread_cond_init(&state->cond, NULL));
    input_destroys = 0;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &begin));
    close_state(state);
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &end));
    CHECK((end.tv_sec - begin.tv_sec) * 1000000000LL +
          end.tv_nsec - begin.tv_nsec < 1000000000LL);
    CHECK(!droidmedia_enabled());
    pthread_mutex_lock(&hung_lock);
    CHECK(hung_entered && !hung_returned && !input_destroys);
    hung_release = 1;
    pthread_cond_broadcast(&hung_cond);
    while (!hung_returned)
        pthread_cond_wait(&hung_cond, &hung_lock);
    pthread_mutex_unlock(&hung_lock);
    return 0;
}

static void output_info(DroidMediaCodec *codec, DroidMediaCodecMetaData *out, DroidMediaRect *crop)
{
    (void)codec;
    *out = metadata;
    *crop = rectangle;
}

int main(void)
{
    DroidMediaAPI api = { .get_output_info = output_info };
    DroidMediaDecState state = { .api = &api, .pkt_timebase = { 1, 1000000 } };
    uint8_t bytes[256];
    DroidMediaCodecData input = { .data = { bytes, sizeof(bytes) }, .ts = 2000000 };
    AVFrame *frame = av_frame_alloc();
    int x, y;
    CHECK(frame);

    memset(bytes, 0, sizeof(bytes));
    metadata.width = 8; metadata.height = 6; metadata.stride = 8; metadata.slice_height = 6;
    metadata.hal_format = DM_COLOR_YUV420_SEMIPLANAR;
    rectangle = (DroidMediaRect){ 2, 2, 7, 5 };
    for (y = 0; y < 6; ++y)
        for (x = 0; x < 8; ++x) bytes[y * 8 + x] = y * 8 + x;
    for (y = 0; y < 3; ++y)
        for (x = 0; x < 4; ++x) {
            bytes[48 + y * 8 + x * 2] = 100 + y * 4 + x;
            bytes[49 + y * 8 + x * 2] = 150 + y * 4 + x;
        }
    input.data.size = 72;
    CHECK(copy_software_frame(&state, &input, frame) == 0);
    CHECK(frame->width == 5 && frame->height == 3 && frame->pts == 2000);
    CHECK(frame->data[0][0] == 18 && frame->data[0][2 * frame->linesize[0] + 4] == 38);
    CHECK(frame->data[1][0] == 105 && frame->data[2][0] == 155);
    CHECK(frame->data[1][frame->linesize[1] + 2] == 111);
    av_frame_unref(frame);
    input.data.size = 71;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    input.data.size = 72;
    rectangle.left = -2;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    rectangle.left = 1;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    rectangle.left = 2; rectangle.right = 9;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    rectangle = (DroidMediaRect){ 0, 0, 0, 0 };
    metadata.stride = 7;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    metadata.stride = 8;
    CHECK(copy_software_frame(&state, NULL, frame) < 0);

    memset(bytes, 0, sizeof(bytes));
    metadata.width = 7; metadata.height = 5; metadata.stride = 8; metadata.slice_height = 5;
    metadata.hal_format = DM_COLOR_YUV420_PLANAR;
    input.data.size = 64;
    bytes[40] = 77;
    bytes[52] = 177;
    CHECK(copy_software_frame(&state, &input, frame) == 0);
    CHECK(frame->width == 7 && frame->height == 5);
    CHECK(frame->data[1][0] == 77 && frame->data[2][0] == 177);
    av_frame_unref(frame);
    input.data.size = 63;
    CHECK(copy_software_frame(&state, &input, frame) < 0);

    memset(bytes, 0, sizeof(bytes));
    metadata.width = 9; metadata.height = 5; metadata.stride = 16; metadata.slice_height = 6;
    metadata.hal_format = DM_COLOR_YV12;
    input.data.size = 192;
    bytes[96] = 201;
    bytes[144] = 101;
    CHECK(copy_software_frame(&state, &input, frame) == 0);
    CHECK(frame->width == 9 && frame->height == 5);
    CHECK(frame->data[1][0] == 101 && frame->data[2][0] == 201);
    av_frame_unref(frame);
    input.data.size = 191;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    metadata.stride = 10;
    CHECK(copy_software_frame(&state, &input, frame) < 0);
    av_frame_free(&frame);
    CHECK(test_requested_metadata() == 0);
    CHECK(test_callback_shutdown() == 0);
    CHECK(test_drain() == 0);
    CHECK(test_input_backpressure(0) == 0);
    CHECK(test_input_backpressure(1) == 0);
    CHECK(test_input_backpressure(2) == 0);
    CHECK(test_input_resume() == 0);
    CHECK(test_hung_stop() == 0);
    puts("droidmedia copy, callback shutdown, drain and backpressure: OK");
    return 0;
}
