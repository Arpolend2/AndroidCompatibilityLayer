#include "preview_gpu_canvas.h"
#include "preview_gpu_metrics.h"
#include "preview_gpu_pixels.h"
#include "store_worker_teardown.h"

#include "../core/tests/mg_gfx_standin.h"

#include <QBrush>
#include <QAtomicInt>
#include <QColor>
#include <QConicalGradient>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QLinearGradient>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>
#include <QThread>
#include <QTransform>
#include <QVector>

#include <climits>
#include <cmath>
#include <cstring>
#include <pthread.h>
#include <sys/syscall.h>

struct PreviewGpuOffscreenContexts {
    struct Owner {
        QByteArray name;
        const char *origin;
        const char *state;
    };
    QMutex mutex;
    QHash<long, Owner> owners;
};

namespace {

struct ThreadFramebuffer {
    GLuint id = 0;
    GLuint stencilBuffer = 0;
    QOpenGLPaintDevice *device = nullptr;
    GLuint filterId = 0;
    GLuint filterStencilBuffer = 0;
    GLuint filterTexture = 0;
    QOpenGLPaintDevice *filterDevice = nullptr;
};

struct ThreadContext {
    PreviewGpuCanvasHost *owner = nullptr;
    QOpenGLContext *context = nullptr;
    QHash<quint64, ThreadFramebuffer> framebuffers;
    GLuint filterProgram = 0;
    GLint filterTintLocation = -1;
    GLint filterOpacityLocation = -1;
    GLint filterModeLocation = -1;
    GLint filterTextureLocation = -1;
    // The guest thread can outlive the host on an unexpected exit.
    std::shared_ptr<PreviewGpuOffscreenContexts> offscreenContexts;
    long tid = 0;
    bool wasBound = false;

    void clear()
    {
        if (offscreenContexts) {
            QMutexLocker lock(&offscreenContexts->mutex);
            offscreenContexts->owners[tid].state = "releasing";
        }
        for (auto it = framebuffers.begin(); it != framebuffers.end(); ++it) {
            delete it->device;
            delete it->filterDevice;
        }
        framebuffers.clear();
        if (context && filterProgram && QOpenGLContext::currentContext() == context)
            context->functions()->glDeleteProgram(filterProgram);
        filterProgram = 0;
        filterTintLocation = filterOpacityLocation = filterModeLocation = filterTextureLocation = -1;
        if (context && QOpenGLContext::currentContext() == context)
            context->doneCurrent();
        delete context;
        context = nullptr;
        owner = nullptr;
        if (offscreenContexts) {
            {
                QMutexLocker lock(&offscreenContexts->mutex);
                offscreenContexts->owners.remove(tid);
            }
            offscreenContexts.reset();
        }
        wasBound = false;
    }

    ~ThreadContext()
    {
        clear();
    }
};

thread_local ThreadContext threadContext;

void clearGlErrors(QOpenGLFunctions *gl)
{
    while (gl->glGetError() != GL_NO_ERROR) {}
}

bool ensureFilterProgram()
{
    if (threadContext.filterProgram)
        return true;
    if (!threadContext.context)
        return false;

    QOpenGLFunctions *gl = threadContext.context->functions();
    static const char *vertexSource =
        "attribute vec2 a_position;\n"
        "varying vec2 v_texCoord;\n"
        "void main() { v_texCoord = a_position * 0.5 + 0.5; gl_Position = vec4(a_position, 0.0, 1.0); }\n";
    static const char *fragmentSource =
        "precision highp float;\n"
        "uniform sampler2D u_mask;\n"
        "uniform vec4 u_tint;\n"
        "uniform float u_opacity;\n"
        "uniform int u_filterMode;\n"
        "varying vec2 v_texCoord;\n"
        "float overlayChannel(float s, float d, float sa, float da) {\n"
        "  float both = s * (1.0 - da) + d * (1.0 - sa);\n"
        "  if (2.0 * d <= da) return both + 2.0 * s * d;\n"
        "  return both + sa * da - 2.0 * (da - d) * (sa - s);\n"
        "}\n"
        "vec4 blendPremultiplied(int mode, vec4 s, vec4 d) {\n"
        "  float sa = s.a, da = d.a;\n"
        "  vec4 both = s * (1.0 - da) + d * (1.0 - sa);\n"
        "  vec4 x = s * da, y = d * sa;\n"
        "  if (mode == 0) return vec4(0.0);\n"
        "  if (mode == 1) return s;\n"
        "  if (mode == 2) return d;\n"
        "  if (mode == 3) return s + d * (1.0 - sa);\n"
        "  if (mode == 4) return d + s * (1.0 - da);\n"
        "  if (mode == 5) return x;\n"
        "  if (mode == 6) return y;\n"
        "  if (mode == 7) return s * (1.0 - da);\n"
        "  if (mode == 8) return d * (1.0 - sa);\n"
        "  if (mode == 9) return x + d * (1.0 - sa);\n"
        "  if (mode == 10) return y + s * (1.0 - da);\n"
        "  if (mode == 11) return both;\n"
        "  if (mode == 12) return min(s + d, vec4(1.0));\n"
        "  if (mode == 13) return s * d;\n"
        "  if (mode == 14) return s + d - s * d;\n"
        "  if (mode == 15) return vec4(overlayChannel(s.r, d.r, sa, da), overlayChannel(s.g, d.g, sa, da),\n"
        "                             overlayChannel(s.b, d.b, sa, da), overlayChannel(sa, da, sa, da));\n"
        "  if (mode == 16) return both + min(x, y);\n"
        "  if (mode == 17) return both + max(x, y);\n"
        "  return s + d * (1.0 - sa);\n"
        "}\n"
        "void main() { vec4 source = texture2D(u_mask, v_texCoord); "
        "gl_FragColor = blendPremultiplied(u_filterMode, u_tint, source) * u_opacity; }\n";
    const auto compile = [gl](GLenum type, const char *source) -> GLuint {
        const GLuint shader = gl->glCreateShader(type);
        if (!shader)
            return 0;
        gl->glShaderSource(shader, 1, &source, nullptr);
        gl->glCompileShader(shader);
        GLint compiled = GL_FALSE;
        gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE)
            return shader;
        GLint length = 0;
        gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        QByteArray log(qMax(1, length), '\0');
        gl->glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
        qWarning("GPU Canvas color-filter shader compilation failed: %s", log.constData());
        gl->glDeleteShader(shader);
        return 0;
    };

    clearGlErrors(gl);
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = vertex ? compile(GL_FRAGMENT_SHADER, fragmentSource) : 0;
    if (!vertex || !fragment) {
        if (vertex)
            gl->glDeleteShader(vertex);
        clearGlErrors(gl);
        return false;
    }

    const GLuint program = gl->glCreateProgram();
    if (program) {
        gl->glAttachShader(program, vertex);
        gl->glAttachShader(program, fragment);
        gl->glBindAttribLocation(program, 0, "a_position");
        gl->glLinkProgram(program);
    }
    gl->glDeleteShader(vertex);
    gl->glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    if (program)
        gl->glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!program || linked != GL_TRUE || gl->glGetError() != GL_NO_ERROR) {
        if (program)
            gl->glDeleteProgram(program);
        qWarning("GPU Canvas color-filter shader could not be linked");
        clearGlErrors(gl);
        return false;
    }

    const GLint tint = gl->glGetUniformLocation(program, "u_tint");
    const GLint opacity = gl->glGetUniformLocation(program, "u_opacity");
    const GLint mode = gl->glGetUniformLocation(program, "u_filterMode");
    const GLint texture = gl->glGetUniformLocation(program, "u_mask");
    if (tint < 0 || opacity < 0 || mode < 0 || texture < 0) {
        gl->glDeleteProgram(program);
        qWarning("GPU Canvas color-filter shader uniforms are unavailable");
        return false;
    }
    threadContext.filterProgram = program;
    threadContext.filterTintLocation = tint;
    threadContext.filterOpacityLocation = opacity;
    threadContext.filterModeLocation = mode;
    threadContext.filterTextureLocation = texture;
    return true;
}

void warnCpuFallback(const char *reason)
{
    static QAtomicInt warned = Q_BASIC_ATOMIC_INITIALIZER(0);
    if (warned.testAndSetOrdered(0, 1))
        qWarning("Experimental GPU Canvas falling back to CPU: %s", reason);
}

QTransform transformFrom(const float *matrix)
{
    if (!matrix)
        return QTransform();
    // The host matrix is row-major; QTransform's constructor interleaves its first two rows.
    return QTransform(matrix[0], matrix[3], matrix[1], matrix[4], matrix[2], matrix[5]);
}

QPainterPath pathFrom(const float *data, int count, int fill)
{
    QPainterPath path;
    path.setFillRule(fill == MEGLER_GFX_EVEN_ODD ? Qt::OddEvenFill : Qt::WindingFill);
    if (!data || count <= 0)
        return path;

    int i = 0;
    while (i < count) {
        const int verb = static_cast<int>(data[i++]);
        switch (verb) {
        case MEGLER_GFX_PATH_MOVE:
            if (i + 1 >= count)
                return path;
            path.moveTo(data[i], data[i + 1]);
            i += 2;
            break;
        case MEGLER_GFX_PATH_LINE:
            if (i + 1 >= count)
                return path;
            path.lineTo(data[i], data[i + 1]);
            i += 2;
            break;
        case MEGLER_GFX_PATH_QUAD:
            if (i + 3 >= count)
                return path;
            path.quadTo(data[i], data[i + 1], data[i + 2], data[i + 3]);
            i += 4;
            break;
        case MEGLER_GFX_PATH_CUBIC:
            if (i + 5 >= count)
                return path;
            path.cubicTo(data[i], data[i + 1], data[i + 2], data[i + 3], data[i + 4], data[i + 5]);
            i += 6;
            break;
        case MEGLER_GFX_PATH_CLOSE:
            path.closeSubpath();
            break;
        default:
            return path;
        }
    }
    return path;
}

QPainter::CompositionMode compositionMode(int blend)
{
    switch (blend) {
    case MEGLER_GFX_BLEND_CLEAR: return QPainter::CompositionMode_Clear;
    case MEGLER_GFX_BLEND_SRC: return QPainter::CompositionMode_Source;
    case MEGLER_GFX_BLEND_DST: return QPainter::CompositionMode_Destination;
    case MEGLER_GFX_BLEND_DST_OVER: return QPainter::CompositionMode_DestinationOver;
    case MEGLER_GFX_BLEND_SRC_IN: return QPainter::CompositionMode_SourceIn;
    case MEGLER_GFX_BLEND_DST_IN: return QPainter::CompositionMode_DestinationIn;
    case MEGLER_GFX_BLEND_SRC_OUT: return QPainter::CompositionMode_SourceOut;
    case MEGLER_GFX_BLEND_DST_OUT: return QPainter::CompositionMode_DestinationOut;
    case MEGLER_GFX_BLEND_SRC_ATOP: return QPainter::CompositionMode_SourceAtop;
    case MEGLER_GFX_BLEND_DST_ATOP: return QPainter::CompositionMode_DestinationAtop;
    case MEGLER_GFX_BLEND_XOR: return QPainter::CompositionMode_Xor;
    case MEGLER_GFX_BLEND_PLUS: return QPainter::CompositionMode_Plus;
    case MEGLER_GFX_BLEND_MODULATE: return QPainter::CompositionMode_Multiply;
    case MEGLER_GFX_BLEND_SCREEN: return QPainter::CompositionMode_Screen;
    case MEGLER_GFX_BLEND_OVERLAY: return QPainter::CompositionMode_Overlay;
    case MEGLER_GFX_BLEND_DARKEN: return QPainter::CompositionMode_Darken;
    case MEGLER_GFX_BLEND_LIGHTEN: return QPainter::CompositionMode_Lighten;
    default:
        break;
    }
    return QPainter::CompositionMode_SourceOver;
}

struct GpuBlendFactors {
    GLenum sourceRgb;
    GLenum destinationRgb;
    GLenum sourceAlpha;
    GLenum destinationAlpha;
};

bool gpuBlendFactors(int blend, GpuBlendFactors *factors)
{
    if (!factors)
        return false;
    factors->sourceRgb = GL_ONE;
    factors->destinationRgb = GL_ONE_MINUS_SRC_ALPHA;
    factors->sourceAlpha = GL_ONE;
    factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
    switch (blend) {
    case MEGLER_GFX_BLEND_SRC_OVER:
        return true;
    case MEGLER_GFX_BLEND_DST_OVER:
        factors->sourceRgb = factors->sourceAlpha = GL_ONE_MINUS_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE;
        return true;
    case MEGLER_GFX_BLEND_DST_OUT:
        factors->sourceRgb = factors->sourceAlpha = GL_ZERO;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_SRC_ATOP:
        factors->sourceRgb = factors->sourceAlpha = GL_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_XOR:
        factors->sourceRgb = factors->sourceAlpha = GL_ONE_MINUS_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_PLUS:
        factors->sourceRgb = factors->sourceAlpha = GL_ONE;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE;
        return true;
    case MEGLER_GFX_BLEND_SCREEN:
        factors->sourceRgb = factors->sourceAlpha = GL_ONE;
        factors->destinationRgb = GL_ONE_MINUS_SRC_COLOR;
        factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
        return true;
    default:
        return false;
    }
}

bool filterScalesWithSource(int mode)
{
    switch (mode) {
    case MEGLER_GFX_BLEND_CLEAR:
    case MEGLER_GFX_BLEND_DST:
    case MEGLER_GFX_BLEND_SRC_IN:
    case MEGLER_GFX_BLEND_DST_IN:
    case MEGLER_GFX_BLEND_DST_OUT:
    case MEGLER_GFX_BLEND_SRC_ATOP:
    case MEGLER_GFX_BLEND_MODULATE:
        return true;
    default:
        return false;
    }
}

QGradient::Spread gradientSpread(int tile)
{
    if (tile == MEGLER_GFX_TILE_REPEAT)
        return QGradient::RepeatSpread;
    if (tile == MEGLER_GFX_TILE_MIRROR)
        return QGradient::ReflectSpread;
    return QGradient::PadSpread;
}

void addStops(QGradient *gradient, const uint32_t *colors, const float *positions, int count)
{
    if (!gradient || !colors || count <= 0)
        return;
    for (int i = 0; i < count; ++i) {
        const qreal position = positions ? qBound<qreal>(0.0, positions[i], 1.0)
                                         : (count == 1 ? 0.0 : qreal(i) / (count - 1));
        gradient->setColorAt(position, QColor::fromRgba(colors[i]));
    }
}

}

struct PreviewGpuCanvasHost::Canvas {
    struct State {
        QTransform matrix;
        QPainterPath clip;
        bool hasClip = false;
        qreal opacity = 1.0;
    };

    quint64 id = 0;
    GLuint texture = 0;
    void *pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    void *cpuCanvas = nullptr;
    bool cpuMirrorCurrent = true;
    QVector<State> states;
};

struct PreviewGpuCanvasHost::Shader {
    QMutex mutex;
    QBrush brush;
    void *cpuShader = nullptr;
};

PreviewGpuCanvasHost::PreviewGpuCanvasHost()
    : m_cpu(mg_gfx_standin())
    , m_surface(nullptr)
    , m_present(nullptr)
    , m_release(nullptr)
    , m_presentPixels(nullptr)
    , m_presentUser(nullptr)
    , m_nextCanvasId(1)
    , m_metrics(std::make_shared<PreviewGpuMetrics>())
{
}

PreviewGpuCanvasHost::~PreviewGpuCanvasHost()
{
    if (m_traceTeardown) {
        if (m_offscreenContexts) {
            QHash<long, PreviewGpuOffscreenContexts::Owner> owners;
            {
                QMutexLocker lock(&m_offscreenContexts->mutex);
                owners = m_offscreenContexts->owners;
            }
            char stage[120];
            std::snprintf(stage, sizeof(stage), "GPU Canvas live drawing contexts=%d",
                          owners.size());
            traceStoreWorkerTeardown(stage);
            int remaining = 16;
            for (auto it = owners.constBegin(); it != owners.constEnd() && remaining-- > 0; ++it) {
                const auto &owner = it.value();
                std::snprintf(stage, sizeof(stage), "GPU Canvas context owner tid=%ld thread=%s first=%s state=%s",
                              it.key(), owner.name.constData(), owner.origin, owner.state);
                traceStoreWorkerTeardown(stage);
            }
        }
        traceStoreWorkerTeardown(m_surface ? "destroying GPU Canvas offscreen surface" : "no GPU Canvas offscreen surface");
    }
    delete m_surface;
    m_surface = nullptr;
    if (m_traceTeardown) traceStoreWorkerTeardown("GPU Canvas surface cleanup complete");
}

bool PreviewGpuCanvasHost::setMetricsEnabled(const std::function<bool()> &enabled)
{
    QMutexLocker lock(&m_mutex);
    if (m_surface || !m_canvases.isEmpty())
        return false;
    m_metrics = std::make_shared<PreviewGpuMetrics>(enabled);
    return true;
}

bool PreviewGpuCanvasHost::prepare(QOpenGLContext *shareContext, Present presentCallback, Release releaseCallback, void *presentUser)
{
    return prepareTexturePresentation(shareContext, presentCallback, releaseCallback, presentUser, false);
}

bool PreviewGpuCanvasHost::prepareRemotePresentation(QOpenGLContext *shareContext, Present presentCallback,
                                                     Release releaseCallback, void *presentUser)
{
    return prepareTexturePresentation(shareContext, presentCallback, releaseCallback, presentUser, true);
}

bool PreviewGpuCanvasHost::prepareTexturePresentation(QOpenGLContext *shareContext, Present presentCallback,
                                                      Release releaseCallback, void *presentUser, bool offscreenWorker)
{
    if (!shareContext || !shareContext->isValid() || !presentCallback || !releaseCallback)
        return false;
    if (m_surface)
        return m_shareContext == shareContext && m_present == presentCallback && m_release == releaseCallback &&
               m_presentUser == presentUser && bool(m_offscreenContexts) == offscreenWorker;
    if (!prepareContext(shareContext))
        return false;
    m_present = presentCallback;
    m_release = releaseCallback;
    m_presentUser = presentUser;
    if (offscreenWorker)
        m_offscreenContexts = std::make_shared<PreviewGpuOffscreenContexts>();
    return true;
}

bool PreviewGpuCanvasHost::prepareCpuPresentation(QOpenGLContext *shareContext, PresentPixels presentCallback, void *presentUser)
{
    if (!shareContext || !shareContext->isValid() || !presentCallback)
        return false;
    if (m_surface)
        return m_shareContext == shareContext && m_presentPixels == presentCallback && m_presentUser == presentUser;
    if (!prepareContext(shareContext))
        return false;
    m_presentPixels = presentCallback;
    m_presentUser = presentUser;
    m_offscreenContexts = std::make_shared<PreviewGpuOffscreenContexts>();
    return true;
}

bool PreviewGpuCanvasHost::prepareContext(QOpenGLContext *shareContext)
{
    QOffscreenSurface *surface = new QOffscreenSurface(shareContext->screen());
    surface->setFormat(shareContext->format());
    surface->create();
    if (!surface->isValid()) {
        delete surface;
        return false;
    }

    QOpenGLContext probe;
    probe.setFormat(shareContext->format());
    probe.setShareContext(shareContext);
    if (!probe.create() || !probe.makeCurrent(surface) || probe.shareGroup() != shareContext->shareGroup()) {
        if (QOpenGLContext::currentContext() == &probe)
            probe.doneCurrent();
        delete surface;
        return false;
    }
    QOpenGLFunctions *gl = probe.functions();
    gl->initializeOpenGLFunctions();
    const GLubyte *vendor = gl->glGetString(GL_VENDOR);
    const GLubyte *renderer = gl->glGetString(GL_RENDERER);
    const GLubyte *version = gl->glGetString(GL_VERSION);
    qInfo().noquote() << "Experimental GPU Canvas driver:"
                      << "vendor=" << (vendor ? reinterpret_cast<const char *>(vendor) : "unavailable")
                      << "renderer=" << (renderer ? reinterpret_cast<const char *>(renderer) : "unavailable")
                      << "version=" << (version ? reinterpret_cast<const char *>(version) : "unavailable");
    probe.doneCurrent();

    m_shareContext = shareContext;
    m_surface = surface;
    return true;
}

void PreviewGpuCanvasHost::install(megler_gfx_host *host)
{
    if (!host || !m_cpu)
        return;
    host->user = this;
    host->canvas_new = canvasNew;
    host->canvas_free = canvasFree;
    host->canvas_discard = canvasDiscard;
    host->flush = flush;
    host->touched = touched;
    host->save = save;
    host->save_layer = saveLayer;
    host->save_layer_blur = saveLayerBlur;
    host->restore = restore;
    host->set_matrix = setMatrix;
    host->clip_rect = clipRect;
    host->clip_path = clipPath;
    host->draw_color = drawColor;
    host->draw_rect = drawRect;
    host->draw_round_rect = drawRoundRect;
    host->draw_oval = drawOval;
    host->draw_path = drawPath;
    host->draw_bitmap = drawBitmap;
    host->shader_linear = shaderLinear;
    host->shader_radial = shaderRadial;
    host->shader_sweep = shaderSweep;
    host->shader_bitmap = shaderBitmap;
    host->shader_matrix = shaderMatrix;
    host->shader_free = shaderFree;
    host->present = present;
    if (m_offscreenContexts) host->release_thread = releaseThread;
}

void MEGLER_CB PreviewGpuCanvasHost::releaseThread(void *user)
{
    const bool ownsContext = threadContext.owner == user && threadContext.context;
    const bool current = ownsContext && QOpenGLContext::currentContext() == threadContext.context;
    const long tid = ::syscall(SYS_gettid);
    char stage[100];
    std::snprintf(stage, sizeof(stage), "GPU Canvas release thread tid=%ld context=%d current=%d",
                  tid, int(ownsContext), int(current));
    traceStoreWorkerTeardown(stage);
    if (threadContext.owner == user) {
        threadContext.clear();
        std::snprintf(stage, sizeof(stage), "GPU Canvas release thread complete tid=%ld", tid);
        traceStoreWorkerTeardown(stage);
    }
}

bool PreviewGpuCanvasHost::ensureCurrent(const char *origin)
{
    QOpenGLContext *share = m_shareContext.data();
    if (!share || !m_surface || !m_surface->isValid())
        return false;

    if (threadContext.owner != this) {
        threadContext.clear();
        threadContext.owner = this;
    }
    if (!threadContext.context) {
        QOpenGLContext *context = new QOpenGLContext;
        context->setFormat(share->format());
        context->setShareContext(share);
        if (!context->create() || context->shareGroup() != share->shareGroup()) {
            delete context;
            threadContext.owner = nullptr;
            return false;
        }
        threadContext.context = context;
        if (m_offscreenContexts) {
            threadContext.offscreenContexts = m_offscreenContexts;
            threadContext.tid = ::syscall(SYS_gettid);
            char name[16] = {};
            pthread_getname_np(pthread_self(), name, sizeof(name));
            QMutexLocker lock(&m_offscreenContexts->mutex);
            m_offscreenContexts->owners.insert(threadContext.tid, {QByteArray(name), origin, "unbound"});
        }
    }
    if (QOpenGLContext::currentContext() != threadContext.context
        && !threadContext.context->makeCurrent(m_surface))
        return false;
    if (threadContext.offscreenContexts && !threadContext.wasBound) {
        QMutexLocker lock(&threadContext.offscreenContexts->mutex);
        threadContext.offscreenContexts->owners[threadContext.tid].state = "was-bound";
        threadContext.wasBound = true;
    }
    threadContext.context->functions()->initializeOpenGLFunctions();
    return true;
}

bool PreviewGpuCanvasHost::ensureFramebuffer(Canvas *canvas, const char *origin)
{
    if (!canvas || !ensureCurrent(origin))
        return false;
    if (threadContext.framebuffers.contains(canvas->id))
        return true;

    QOpenGLContext *context = threadContext.context;
    QOpenGLFunctions *gl = context->functions();
    ThreadFramebuffer framebuffer;
    GLint previous = 0;
    GLint previousRenderbuffer = 0;
    clearGlErrors(gl);
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    gl->glGenFramebuffers(1, &framebuffer.id);
    gl->glGenRenderbuffers(1, &framebuffer.stencilBuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvas->texture, 0);
    gl->glBindRenderbuffer(GL_RENDERBUFFER, framebuffer.stencilBuffer);
    gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, canvas->width, canvas->height);
    gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, framebuffer.stencilBuffer);
    const bool complete = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                          gl->glGetError() == GL_NO_ERROR;
    gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    if (!complete) {
        gl->glDeleteRenderbuffers(1, &framebuffer.stencilBuffer);
        gl->glDeleteFramebuffers(1, &framebuffer.id);
        warnCpuFallback("could not create a complete framebuffer");
        return false;
    }

    framebuffer.device = new QOpenGLPaintDevice(canvas->width, canvas->height);
    threadContext.framebuffers.insert(canvas->id, framebuffer);
    return true;
}

bool PreviewGpuCanvasHost::ensureFilterFramebuffer(Canvas *canvas)
{
    if (!canvas || !ensureFramebuffer(canvas, "filter-mask"))
        return false;
    ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    if (framebuffer.filterId)
        return true;

    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previousFramebuffer = 0;
    GLint previousTexture = 0;
    GLint previousRenderbuffer = 0;
    clearGlErrors(gl);
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    gl->glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    gl->glGenTextures(1, &framebuffer.filterTexture);
    gl->glBindTexture(GL_TEXTURE_2D, framebuffer.filterTexture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, canvas->width, canvas->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
    gl->glGenFramebuffers(1, &framebuffer.filterId);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.filterId);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebuffer.filterTexture, 0);
    gl->glGenRenderbuffers(1, &framebuffer.filterStencilBuffer);
    gl->glBindRenderbuffer(GL_RENDERBUFFER, framebuffer.filterStencilBuffer);
    gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, canvas->width, canvas->height);
    gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  framebuffer.filterStencilBuffer);
    const bool complete = framebuffer.filterTexture && framebuffer.filterId && framebuffer.filterStencilBuffer &&
                          gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                          gl->glGetError() == GL_NO_ERROR;
    gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
    gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    if (!complete) {
        if (framebuffer.filterId)
            gl->glDeleteFramebuffers(1, &framebuffer.filterId);
        if (framebuffer.filterStencilBuffer)
            gl->glDeleteRenderbuffers(1, &framebuffer.filterStencilBuffer);
        if (framebuffer.filterTexture)
            gl->glDeleteTextures(1, &framebuffer.filterTexture);
        clearGlErrors(gl);
        warnCpuFallback("could not create a complete color-filter framebuffer");
        return false;
    }

    framebuffer.filterDevice = new QOpenGLPaintDevice(canvas->width, canvas->height);
    threadContext.framebuffers.insert(canvas->id, framebuffer);
    return true;
}

bool PreviewGpuCanvasHost::readback(Canvas *canvas, PreviewGpuReadbackReason reason)
{
    if (canvas && canvas->cpuMirrorCurrent) {
        m_metrics->recordReadbackCacheHit(reason);
        return true;
    }
    const PreviewGpuMetrics::Sample sample = m_metrics->beginReadback(reason);
    QElapsedTimer timer;
    QElapsedTimer phaseTimer;
    if (sample.sampled)
        timer.start();
    if (!ensureFramebuffer(canvas, "readback")) {
        if (sample.sampled)
            m_metrics->finishReadback(sample, static_cast<quint64>(timer.nsecsElapsed() / 1000), 0, 0);
        return false;
    }
    QOpenGLFunctions *gl = threadContext.context->functions();
    const ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    QByteArray rgba(canvas->width * canvas->height * 4, char(0));
    GLint previous = 0;
    clearGlErrors(gl);
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);
    if (sample.sampled)
        phaseTimer.start();
    gl->glFinish();
    gl->glReadPixels(0, 0, canvas->width, canvas->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum error = gl->glGetError();
    const quint64 glUs = sample.sampled ? static_cast<quint64>(phaseTimer.nsecsElapsed() / 1000) : 0;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    if (error != GL_NO_ERROR) {
        qWarning("GPU Canvas readback failed (OpenGL error 0x%x)", static_cast<unsigned int>(error));
        if (sample.sampled)
            m_metrics->finishReadback(sample, static_cast<quint64>(timer.nsecsElapsed() / 1000), glUs, 0);
        return false;
    }
    if (sample.sampled)
        phaseTimer.restart();
    copyReadbackRgbaToBgra(reinterpret_cast<const uint8_t *>(rgba.constData()),
                          static_cast<uint8_t *>(canvas->pixels),
                          canvas->width, canvas->height, canvas->stride);
    if (sample.sampled)
        m_metrics->finishReadback(sample, static_cast<quint64>(timer.nsecsElapsed() / 1000), glUs,
                                  static_cast<quint64>(phaseTimer.nsecsElapsed() / 1000));
    canvas->cpuMirrorCurrent = true;
    return true;
}

bool PreviewGpuCanvasHost::upload(Canvas *canvas)
{
    if (!canvas || !ensureCurrent("upload"))
        return false;
    QByteArray rgba(canvas->width * canvas->height * 4, char(0));
    for (int y = 0; y < canvas->height; ++y) {
        const uchar *src = static_cast<const uchar *>(canvas->pixels) + (canvas->height - 1 - y) * canvas->stride;
        uchar *dst = reinterpret_cast<uchar *>(rgba.data()) + y * canvas->width * 4;
        for (int x = 0; x < canvas->width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previous = 0;
    clearGlErrors(gl);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    gl->glBindTexture(GL_TEXTURE_2D, canvas->texture);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvas->width, canvas->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constData());
    const GLenum error = gl->glGetError();
    gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    if (error != GL_NO_ERROR)
        qWarning("GPU Canvas upload failed (OpenGL error 0x%x)", static_cast<unsigned int>(error));
    else
        canvas->cpuMirrorCurrent = true;
    return error == GL_NO_ERROR;
}

bool PreviewGpuCanvasHost::drawFiltered(Canvas *canvas, const megler_gfx_paint *paint,
                                        const QRectF &deviceBounds,
                                        const std::function<void(QPainter &)> &drawSource)
{
    if (!canvas || !paint)
        return false;
    GpuBlendFactors blendFactors;
    if (!gpuBlendFactors(paint->blend, &blendFactors)) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedBlend);
        return false;
    }
    if (paint->blend != MEGLER_GFX_BLEND_SRC_OVER && !filterScalesWithSource(paint->filter_mode)) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedBlend);
        return false;
    }
    if (!ensureFilterFramebuffer(canvas) || !ensureFilterProgram()) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }

    QRectF clippedBounds = deviceBounds.intersected(QRectF(0, 0, canvas->width, canvas->height));
    const Canvas::State &state = canvas->states.last();
    if (state.hasClip)
        clippedBounds = clippedBounds.intersected(state.clip.boundingRect());
    if (clippedBounds.isEmpty())
        return true;
    const int scissorLeft = qBound(0, static_cast<int>(std::floor(clippedBounds.left())), canvas->width);
    const int scissorTop = qBound(0, static_cast<int>(std::floor(clippedBounds.top())), canvas->height);
    const int scissorRight = qBound(0, static_cast<int>(std::ceil(clippedBounds.right())), canvas->width);
    const int scissorBottom = qBound(0, static_cast<int>(std::ceil(clippedBounds.bottom())), canvas->height);
    if (scissorLeft >= scissorRight || scissorTop >= scissorBottom)
        return true;

    ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previousFramebuffer = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.filterId);

    QPainter maskPainter;
    if (!maskPainter.begin(framebuffer.filterDevice)) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }
    maskPainter.setRenderHint(QPainter::Antialiasing, false);
    maskPainter.setCompositionMode(QPainter::CompositionMode_Clear);
    maskPainter.fillRect(QRectF(scissorLeft, scissorTop, scissorRight - scissorLeft, scissorBottom - scissorTop),
                         Qt::transparent);
    maskPainter.setCompositionMode(QPainter::CompositionMode_Source);
    drawSource(maskPainter);
    maskPainter.end();
    const GLenum maskError = gl->glGetError();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    if (maskError != GL_NO_ERROR) {
        qWarning("GPU Canvas color-filter mask failed (OpenGL error 0x%x)", static_cast<unsigned int>(maskError));
        clearGlErrors(gl);
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }

    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);
    QPainter painter;
    if (!painter.begin(framebuffer.device)) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }
    painter.beginNativePainting();

    GLint previousProgram = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture = 0;
    GLint previousArrayBuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousBlendSrcRgb = 0;
    GLint previousBlendDstRgb = 0;
    GLint previousBlendSrcAlpha = 0;
    GLint previousBlendDstAlpha = 0;
    GLint previousBlendEquationRgb = 0;
    GLint previousBlendEquationAlpha = 0;
    GLint previousScissorBox[4] = {};
    GLint attributeSize = 0;
    GLint attributeType = 0;
    GLint attributeNormalized = 0;
    GLint attributeStride = 0;
    GLint attributeBuffer = 0;
    GLint attributeEnabled = 0;
    void *attributePointer = nullptr;
    GLboolean previousColorMask[4] = {};
    const GLboolean blendEnabled = gl->glIsEnabled(GL_BLEND);
    const GLboolean scissorEnabled = gl->glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean stencilEnabled = gl->glIsEnabled(GL_STENCIL_TEST);
    const GLboolean depthEnabled = gl->glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullEnabled = gl->glIsEnabled(GL_CULL_FACE);
    const GLboolean ditherEnabled = gl->glIsEnabled(GL_DITHER);
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    gl->glGetIntegerv(GL_VIEWPORT, previousViewport);
    gl->glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
    gl->glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
    gl->glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
    gl->glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
    gl->glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
    gl->glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
    gl->glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
    gl->glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attributeSize);
    gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attributeType);
    gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attributeNormalized);
    gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attributeStride);
    gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attributeBuffer);
    gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attributeEnabled);
    gl->glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attributePointer);

    const GLfloat vertices[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    gl->glViewport(0, 0, canvas->width, canvas->height);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(scissorLeft, canvas->height - scissorBottom,
                  scissorRight - scissorLeft, scissorBottom - scissorTop);
    gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_DITHER);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl->glBlendFuncSeparate(blendFactors.sourceRgb, blendFactors.destinationRgb,
                            blendFactors.sourceAlpha, blendFactors.destinationAlpha);
    gl->glUseProgram(threadContext.filterProgram);
    gl->glBindTexture(GL_TEXTURE_2D, framebuffer.filterTexture);
    const QColor tint = QColor::fromRgba(paint->filter_color);
    const GLfloat tintAlpha = static_cast<GLfloat>(tint.alphaF());
    gl->glUniform4f(threadContext.filterTintLocation,
                    static_cast<GLfloat>(tint.redF()) * tintAlpha,
                    static_cast<GLfloat>(tint.greenF()) * tintAlpha,
                    static_cast<GLfloat>(tint.blueF()) * tintAlpha,
                    tintAlpha);
    gl->glUniform1f(threadContext.filterOpacityLocation, static_cast<GLfloat>(canvas->states.last().opacity));
    gl->glUniform1i(threadContext.filterModeLocation, paint->filter_mode);
    gl->glUniform1i(threadContext.filterTextureLocation, 0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);

    GLenum error = gl->glGetError();
    if (error == GL_NO_ERROR)
        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawError = gl->glGetError();

    if (attributeBuffer)
        gl->glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attributeBuffer));
    else
        gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glVertexAttribPointer(0, attributeSize, static_cast<GLenum>(attributeType),
                              static_cast<GLboolean>(attributeNormalized), attributeStride, attributePointer);
    if (attributeEnabled)
        gl->glEnableVertexAttribArray(0);
    else
        gl->glDisableVertexAttribArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    gl->glUseProgram(static_cast<GLuint>(previousProgram));
    gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    gl->glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    gl->glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    gl->glScissor(previousScissorBox[0], previousScissorBox[1],
                  previousScissorBox[2], previousScissorBox[3]);
    gl->glBlendFuncSeparate(static_cast<GLenum>(previousBlendSrcRgb), static_cast<GLenum>(previousBlendDstRgb),
                            static_cast<GLenum>(previousBlendSrcAlpha), static_cast<GLenum>(previousBlendDstAlpha));
    gl->glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                                static_cast<GLenum>(previousBlendEquationAlpha));
    gl->glColorMask(previousColorMask[0], previousColorMask[1], previousColorMask[2], previousColorMask[3]);
    const auto restoreEnabled = [gl](GLenum capability, GLboolean enabled) {
        if (enabled)
            gl->glEnable(capability);
        else
            gl->glDisable(capability);
    };
    restoreEnabled(GL_BLEND, blendEnabled);
    restoreEnabled(GL_SCISSOR_TEST, scissorEnabled);
    restoreEnabled(GL_STENCIL_TEST, stencilEnabled);
    restoreEnabled(GL_DEPTH_TEST, depthEnabled);
    restoreEnabled(GL_CULL_FACE, cullEnabled);
    restoreEnabled(GL_DITHER, ditherEnabled);
    painter.endNativePainting();
    painter.end();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));

    if (error != GL_NO_ERROR) {
        qWarning("GPU Canvas color-filter setup failed before drawing (OpenGL error 0x%x)",
                 static_cast<unsigned int>(error));
        clearGlErrors(gl);
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }
    canvas->cpuMirrorCurrent = false;
    if (drawError != GL_NO_ERROR) {
        qWarning("GPU Canvas color-filter draw reported OpenGL error 0x%x", static_cast<unsigned int>(drawError));
        m_metrics->recordFilterDrawError();
    }
    return true;
}

bool PreviewGpuCanvasHost::drawFilteredPath(Canvas *canvas, const QPainterPath &path,
                                            const megler_gfx_paint *paint, void *shader)
{
    if (!canvas || !paint)
        return false;
    if (paint->filter_mode < MEGLER_GFX_BLEND_CLEAR || paint->filter_mode > MEGLER_GFX_BLEND_LIGHTEN) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedMode);
        return false;
    }
    if (paint->blend == MEGLER_GFX_BLEND_DST)
        return true;

    const Canvas::State &state = canvas->states.last();
    QRectF deviceBounds = state.matrix.mapRect(path.boundingRect());
    if (paint->style != MEGLER_GFX_FILL) {
        const QTransform &matrix = state.matrix;
        const qreal scale = std::sqrt(matrix.m11() * matrix.m11() + matrix.m12() * matrix.m12() +
                                      matrix.m21() * matrix.m21() + matrix.m22() * matrix.m22());
        const qreal strokeWidth = paint->stroke_width > 0 ? paint->stroke_width : 1.0;
        const qreal extent = strokeWidth * qMax<qreal>(1.0, paint->miter_limit) * scale * 0.5 + 1.0;
        deviceBounds.adjust(-extent, -extent, extent, extent);
    }
    return drawFiltered(canvas, paint, deviceBounds, [canvas, path, paint, shader](QPainter &painter) {
        const Canvas::State &state = canvas->states.last();
        painter.setRenderHint(QPainter::Antialiasing,
                              (paint->flags & MEGLER_GFX_ANTI_ALIAS) != 0 || state.hasClip);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, (paint->flags & MEGLER_GFX_FILTER_BITMAP) != 0);
        painter.setWorldTransform(state.matrix);
        painter.setOpacity(shader ? qreal(qAlpha(paint->color)) / 255.0 : 1.0);
        if (state.hasClip) {
            bool invertible = false;
            const QTransform inverse = state.matrix.inverted(&invertible);
            if (invertible)
                painter.setClipPath(inverse.map(state.clip), Qt::ReplaceClip);
            else
                painter.setClipRect(QRectF());
        }

        QBrush brush(QColor::fromRgba(paint->color));
        if (shader) {
            Shader *gpuShader = static_cast<Shader *>(shader);
            QMutexLocker lock(&gpuShader->mutex);
            brush = gpuShader->brush;
        }
        QPen pen(brush, qMax<qreal>(0.0, paint->stroke_width));
        pen.setCapStyle(paint->cap == MEGLER_GFX_CAP_ROUND ? Qt::RoundCap
                        : paint->cap == MEGLER_GFX_CAP_SQUARE ? Qt::SquareCap : Qt::FlatCap);
        pen.setJoinStyle(paint->join == MEGLER_GFX_JOIN_ROUND ? Qt::RoundJoin
                         : paint->join == MEGLER_GFX_JOIN_BEVEL ? Qt::BevelJoin : Qt::MiterJoin);
        pen.setMiterLimit(qMax<qreal>(1.0, paint->miter_limit));
        painter.setPen(paint->style == MEGLER_GFX_FILL ? Qt::NoPen : pen);
        painter.setBrush(paint->style == MEGLER_GFX_STROKE ? Qt::NoBrush : brush);
        painter.drawPath(path);
    });
}

bool PreviewGpuCanvasHost::drawFilteredBitmap(Canvas *canvas, const megler_gfx_image *image, const float *src,
                                              const float *dst, const megler_gfx_paint *paint)
{
    if (!canvas || !image || !image->pixels || !src || !dst || !paint)
        return false;
    if (paint->filter_mode >= MEGLER_GFX_BLEND_CLEAR &&
        paint->filter_mode <= MEGLER_GFX_BLEND_LIGHTEN &&
        paint->blend == MEGLER_GFX_BLEND_DST)
        return true;
    if (image->pixels == canvas->pixels) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::SelfBitmap);
        return false;
    }
    if (image->width <= 0 || image->height <= 0 || image->width > INT_MAX / 4 ||
        image->stride < image->width * 4 || !(dst[2] > dst[0]) || !(dst[3] > dst[1])) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::InvalidBitmap);
        return false;
    }
    if (paint->filter_mode < MEGLER_GFX_BLEND_CLEAR || paint->filter_mode > MEGLER_GFX_BLEND_LIGHTEN) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedMode);
        return false;
    }
    const QRectF deviceBounds = canvas->states.last().matrix.mapRect(
        QRectF(dst[0], dst[1], dst[2] - dst[0], dst[3] - dst[1]));
    return drawFiltered(canvas, paint, deviceBounds, [canvas, image, src, dst, paint](QPainter &painter) {
        const Canvas::State &state = canvas->states.last();
        const QImage source(static_cast<const uchar *>(image->pixels), image->width, image->height, image->stride,
                            QImage::Format_ARGB32_Premultiplied);
        painter.setRenderHint(QPainter::Antialiasing,
                              (paint->flags & MEGLER_GFX_ANTI_ALIAS) != 0 || state.hasClip);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, (paint->flags & MEGLER_GFX_FILTER_BITMAP) != 0);
        painter.setWorldTransform(state.matrix);
        painter.setOpacity(qreal(qAlpha(paint->color)) / 255.0);
        if (state.hasClip) {
            bool invertible = false;
            const QTransform inverse = state.matrix.inverted(&invertible);
            if (invertible)
                painter.setClipPath(inverse.map(state.clip), Qt::ReplaceClip);
            else
                painter.setClipRect(QRectF());
        }
        painter.drawImage(QRectF(dst[0], dst[1], dst[2] - dst[0], dst[3] - dst[1]), source,
                          QRectF(src[0], src[1], src[2] - src[0], src[3] - src[1]));
    });
}

void PreviewGpuCanvasHost::removeFramebuffer(Canvas *canvas)
{
    if (!canvas || threadContext.owner != this || !threadContext.context)
        return;
    QHash<quint64, ThreadFramebuffer>::iterator it = threadContext.framebuffers.find(canvas->id);
    if (it == threadContext.framebuffers.end())
        return;
    if (QOpenGLContext::currentContext() != threadContext.context && !ensureCurrent("free-framebuffer"))
        return;
    QOpenGLFunctions *gl = threadContext.context->functions();
    if (it->id)
        gl->glDeleteFramebuffers(1, &it->id);
    if (it->filterId)
        gl->glDeleteFramebuffers(1, &it->filterId);
    if (it->stencilBuffer)
        gl->glDeleteRenderbuffers(1, &it->stencilBuffer);
    if (it->filterStencilBuffer)
        gl->glDeleteRenderbuffers(1, &it->filterStencilBuffer);
    if (it->filterTexture)
        gl->glDeleteTextures(1, &it->filterTexture);
    delete it->device;
    delete it->filterDevice;
    threadContext.framebuffers.erase(it);
}

PreviewGpuCanvasHost::Canvas *PreviewGpuCanvasHost::findCanvas(void *canvas) const
{
    QMutexLocker lock(&m_mutex);
    return m_canvases.value(canvas, nullptr);
}

void *PreviewGpuCanvasHost::cpuShader(void *handle) const
{
    return handle ? static_cast<Shader *>(handle)->cpuShader : nullptr;
}

void PreviewGpuCanvasHost::clip(Canvas *canvas, const QPainterPath &path, int op)
{
    if (!canvas || canvas->states.isEmpty())
        return;
    Canvas::State &state = canvas->states.last();
    const QPainterPath devicePath = state.matrix.map(path);
    if (op == MEGLER_GFX_CLIP_DIFFERENCE) {
        if (state.hasClip)
            state.clip = state.clip.subtracted(devicePath);
        else {
            QPainterPath bounds;
            bounds.addRect(QRectF(0, 0, canvas->width, canvas->height));
            state.clip = bounds.subtracted(devicePath);
        }
    } else if (state.hasClip) {
        state.clip = state.clip.intersected(devicePath);
    } else {
        state.clip = devicePath;
    }
    state.hasClip = true;
}

void PreviewGpuCanvasHost::paintPath(Canvas *canvas, const QPainterPath &path, const megler_gfx_paint *paint, void *shader)
{
    if (!canvas || !paint || !ensureFramebuffer(canvas, "paint-path"))
        return;
    ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previous = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);

    QPainter painter;
    if (!painter.begin(framebuffer.device)) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
        return;
    }
    const Canvas::State &state = canvas->states.last();
    painter.setRenderHint(QPainter::Antialiasing, (paint->flags & MEGLER_GFX_ANTI_ALIAS) != 0 || state.hasClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, (paint->flags & MEGLER_GFX_FILTER_BITMAP) != 0);
    painter.setCompositionMode(compositionMode(paint->blend));
    painter.setWorldTransform(state.matrix);
    painter.setOpacity(state.opacity);
    if (state.hasClip) {
        bool invertible = false;
        const QTransform inverse = state.matrix.inverted(&invertible);
        if (!invertible) {
            painter.setClipRect(QRectF());
        } else {
            painter.setClipPath(inverse.map(state.clip), Qt::ReplaceClip);
        }
    }

    QBrush brush(QColor::fromRgba(paint->color));
    if (shader) {
        Shader *gpuShader = static_cast<Shader *>(shader);
        QMutexLocker lock(&gpuShader->mutex);
        brush = gpuShader->brush;
        painter.setOpacity(state.opacity * qreal(qAlpha(paint->color)) / 255.0);
    }
    QPen pen(brush, qMax<qreal>(0.0, paint->stroke_width));
    pen.setCapStyle(paint->cap == MEGLER_GFX_CAP_ROUND ? Qt::RoundCap
                    : paint->cap == MEGLER_GFX_CAP_SQUARE ? Qt::SquareCap : Qt::FlatCap);
    pen.setJoinStyle(paint->join == MEGLER_GFX_JOIN_ROUND ? Qt::RoundJoin
                     : paint->join == MEGLER_GFX_JOIN_BEVEL ? Qt::BevelJoin : Qt::MiterJoin);
    pen.setMiterLimit(qMax<qreal>(1.0, paint->miter_limit));
    painter.setPen(paint->style == MEGLER_GFX_FILL ? Qt::NoPen : pen);
    painter.setBrush(paint->style == MEGLER_GFX_STROKE ? Qt::NoBrush : brush);
    painter.drawPath(path);
    painter.end();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    canvas->cpuMirrorCurrent = false;
}

void *MEGLER_CB PreviewGpuCanvasHost::canvasNew(void *user, void *pixels, int width, int height, int stride)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self || !pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 ||
        height > INT_MAX / (width * 4) || stride < width * 4)
        return self && self->m_cpu->canvas_new ? self->m_cpu->canvas_new(self->m_cpu->user, pixels, width, height, stride) : nullptr;

    void *cpuCanvas = self->m_cpu->canvas_new ? self->m_cpu->canvas_new(self->m_cpu->user, pixels, width, height, stride) : nullptr;
    const bool ready = self->ensureCurrent("canvas-new");
    if (!cpuCanvas || !ready) {
        if (!ready)
            warnCpuFallback("shared OpenGL context is unavailable");
        if (cpuCanvas && self->m_cpu->canvas_free)
            self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        return self->m_cpu->canvas_new(self->m_cpu->user, pixels, width, height, stride);
    }

    GLint maxTextureSize = 0;
    threadContext.context->functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (width > maxTextureSize || height > maxTextureSize) {
        warnCpuFallback("bitmap exceeds the GPU's maximum texture size");
        self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        return self->m_cpu->canvas_new(self->m_cpu->user, pixels, width, height, stride);
    }

    Canvas *canvas = new Canvas;
    canvas->pixels = pixels;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = stride;
    canvas->cpuCanvas = cpuCanvas;
    canvas->states.append(Canvas::State());
    {
        QMutexLocker lock(&self->m_mutex);
        canvas->id = self->m_nextCanvasId++;
    }

    QByteArray rgba(width * height * 4, char(0));
    for (int y = 0; y < height; ++y) {
        const uchar *src = static_cast<const uchar *>(pixels) + (height - 1 - y) * stride;
        uchar *dst = reinterpret_cast<uchar *>(rgba.data()) + y * width * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }

    QOpenGLFunctions *gl = threadContext.context->functions();
    clearGlErrors(gl);
    gl->glGenTextures(1, &canvas->texture);
    gl->glBindTexture(GL_TEXTURE_2D, canvas->texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constData());
    if (!canvas->texture || gl->glGetError() != GL_NO_ERROR || !self->ensureFramebuffer(canvas, "canvas-new")) {
        if (canvas->texture)
            gl->glDeleteTextures(1, &canvas->texture);
        warnCpuFallback("GPU texture allocation failed");
        self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        delete canvas;
        return self->m_cpu->canvas_new(self->m_cpu->user, pixels, width, height, stride);
    }
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_canvases.insert(canvas, canvas);
    }
    return canvas;
}

void MEGLER_CB PreviewGpuCanvasHost::canvasFree(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self || !handle)
        return;
    Canvas *canvas = self->findCanvas(handle);
    if (!canvas) {
        if (self->m_cpu->canvas_free)
            self->m_cpu->canvas_free(self->m_cpu->user, handle);
        return;
    }
    bool idle = false;
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_canvases.remove(handle);
        idle = self->m_canvases.isEmpty();
    }
    self->removeFramebuffer(canvas);
    const bool released = !self->m_release || self->m_release(self->m_presentUser, canvas->texture);
    if (released && self->ensureCurrent("canvas-free") && canvas->texture)
        threadContext.context->functions()->glDeleteTextures(1, &canvas->texture);
    else if (!released)
        qWarning("GPU Canvas texture could not be released from the Qt scene graph; retaining it to avoid use-after-free");
    if (self->m_cpu->canvas_free)
        self->m_cpu->canvas_free(self->m_cpu->user, canvas->cpuCanvas);
    delete canvas;
    if (idle && threadContext.owner == self)
        threadContext.clear();
}

void MEGLER_CB PreviewGpuCanvasHost::canvasDiscard(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self || !handle)
        return;
    Canvas *canvas = self->findCanvas(handle);
    if (!canvas) {
        if (self->m_cpu->canvas_discard)
            self->m_cpu->canvas_discard(self->m_cpu->user, handle);
        else if (self->m_cpu->canvas_free)
            self->m_cpu->canvas_free(self->m_cpu->user, handle);
        return;
    }
    bool idle = false;
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_canvases.remove(handle);
        idle = self->m_canvases.isEmpty();
    }
    self->removeFramebuffer(canvas);
    const bool released = !self->m_release || self->m_release(self->m_presentUser, canvas->texture);
    if (released && self->ensureCurrent("canvas-discard") && canvas->texture)
        threadContext.context->functions()->glDeleteTextures(1, &canvas->texture);
    else if (!released)
        qWarning("GPU Canvas texture could not be released from the Qt scene graph; retaining it to avoid use-after-free");
    if (self->m_cpu->canvas_discard)
        self->m_cpu->canvas_discard(self->m_cpu->user, canvas->cpuCanvas);
    else if (self->m_cpu->canvas_free)
        self->m_cpu->canvas_free(self->m_cpu->user, canvas->cpuCanvas);
    delete canvas;
    if (idle && threadContext.owner == self)
        threadContext.clear();
}

void MEGLER_CB PreviewGpuCanvasHost::flush(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->flush)
            self->m_cpu->flush(self->m_cpu->user, handle);
        return;
    }
    if (self->readback(canvas, PreviewGpuReadbackReason::Flush) && self->m_cpu->flush)
        self->m_cpu->flush(self->m_cpu->user, canvas->cpuCanvas);
}

void MEGLER_CB PreviewGpuCanvasHost::touched(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->touched)
            self->m_cpu->touched(self->m_cpu->user, handle);
        return;
    }
    canvas->cpuMirrorCurrent = false;
    if (self->m_cpu->touched)
        self->m_cpu->touched(self->m_cpu->user, canvas->cpuCanvas);
    self->upload(canvas);
}

void MEGLER_CB PreviewGpuCanvasHost::save(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->save)
            self->m_cpu->save(self->m_cpu->user, handle);
        return;
    }
    canvas->states.append(canvas->states.last());
    if (self->m_cpu->save)
        self->m_cpu->save(self->m_cpu->user, canvas->cpuCanvas);
}

void MEGLER_CB PreviewGpuCanvasHost::saveLayer(void *user, void *handle, const float *bounds, int alpha, int blend)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->save_layer)
            self->m_cpu->save_layer(self->m_cpu->user, handle, bounds, alpha, blend);
        return;
    }
    Canvas::State state = canvas->states.last();
    state.opacity *= qBound(0, alpha, 255) / 255.0;
    canvas->states.append(state);
    if (bounds) {
        QPainterPath boundsPath;
        boundsPath.addRect(QRectF(bounds[0], bounds[1], bounds[2] - bounds[0], bounds[3] - bounds[1]));
        self->clip(canvas, boundsPath, MEGLER_GFX_CLIP_INTERSECT);
    }
    if (self->m_cpu->save_layer)
        self->m_cpu->save_layer(self->m_cpu->user, canvas->cpuCanvas, bounds, alpha, blend);
}

void MEGLER_CB PreviewGpuCanvasHost::saveLayerBlur(void *user, void *handle, const float *bounds, int alpha,
                                                   float radiusX, float radiusY, bool clamp)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->save_layer_blur)
            self->m_cpu->save_layer_blur(self->m_cpu->user, handle, bounds, alpha, radiusX, radiusY, clamp);
        return;
    }
    Canvas::State state = canvas->states.last();
    state.opacity *= qBound(0, alpha, 255) / 255.0;
    canvas->states.append(state);
    if (bounds) {
        QPainterPath boundsPath;
        boundsPath.addRect(QRectF(bounds[0], bounds[1], bounds[2] - bounds[0], bounds[3] - bounds[1]));
        self->clip(canvas, boundsPath, MEGLER_GFX_CLIP_INTERSECT);
    }
    if (self->m_cpu->save_layer_blur) {
        self->m_cpu->save_layer_blur(self->m_cpu->user, canvas->cpuCanvas, bounds, alpha, radiusX, radiusY, clamp);
    } else if (self->m_cpu->save_layer) {
        self->m_cpu->save_layer(self->m_cpu->user, canvas->cpuCanvas, bounds, alpha, MEGLER_GFX_BLEND_SRC_OVER);
    }
}

void MEGLER_CB PreviewGpuCanvasHost::restore(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->restore)
            self->m_cpu->restore(self->m_cpu->user, handle);
        return;
    }
    if (canvas->states.size() > 1)
        canvas->states.removeLast();
    if (self->m_cpu->restore)
        self->m_cpu->restore(self->m_cpu->user, canvas->cpuCanvas);
}

void MEGLER_CB PreviewGpuCanvasHost::setMatrix(void *user, void *handle, const float *matrix)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->set_matrix)
            self->m_cpu->set_matrix(self->m_cpu->user, handle, matrix);
        return;
    }
    canvas->states.last().matrix = transformFrom(matrix);
    if (self->m_cpu->set_matrix)
        self->m_cpu->set_matrix(self->m_cpu->user, canvas->cpuCanvas, matrix);
}

void MEGLER_CB PreviewGpuCanvasHost::clipRect(void *user, void *handle, float left, float top, float right, float bottom, int op)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->clip_rect)
            self->m_cpu->clip_rect(self->m_cpu->user, handle, left, top, right, bottom, op);
        return;
    }
    QPainterPath path;
    path.addRect(QRectF(left, top, right - left, bottom - top));
    self->clip(canvas, path, op);
    if (self->m_cpu->clip_rect)
        self->m_cpu->clip_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, op);
}

void MEGLER_CB PreviewGpuCanvasHost::clipPath(void *user, void *handle, const float *path, int count, int fill, int op)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->clip_path)
            self->m_cpu->clip_path(self->m_cpu->user, handle, path, count, fill, op);
        return;
    }
    self->clip(canvas, pathFrom(path, count, fill), op);
    if (self->m_cpu->clip_path)
        self->m_cpu->clip_path(self->m_cpu->user, canvas->cpuCanvas, path, count, fill, op);
}

void MEGLER_CB PreviewGpuCanvasHost::drawColor(void *user, void *handle, uint32_t color, int blend)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_color)
            self->m_cpu->draw_color(self->m_cpu->user, handle, color, blend);
        return;
    }
    if (!self->ensureFramebuffer(canvas, "draw-color"))
        return;
    const ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previous = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);
    QPainter painter(framebuffer.device);
    const Canvas::State &state = canvas->states.last();
    painter.setRenderHint(QPainter::Antialiasing, state.hasClip);
    painter.resetTransform();
    painter.setOpacity(state.opacity);
    if (state.hasClip) {
        painter.setClipPath(state.clip, Qt::ReplaceClip);
    }
    painter.setCompositionMode(compositionMode(blend));
    painter.fillRect(QRectF(0, 0, canvas->width, canvas->height), QColor::fromRgba(color));
    painter.end();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    canvas->cpuMirrorCurrent = false;
}

void MEGLER_CB PreviewGpuCanvasHost::drawRect(void *user, void *handle, float left, float top, float right, float bottom,
                                             const megler_gfx_paint *paint, void *shader)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_rect)
            self->m_cpu->draw_rect(self->m_cpu->user, handle, left, top, right, bottom, paint, self->cpuShader(shader));
        return;
    }
    if (paint && paint->filter_mode >= 0) {
        QPainterPath path;
        path.addRect(QRectF(left, top, right - left, bottom - top));
        if (self->drawFilteredPath(canvas, path, paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            canvas->cpuMirrorCurrent = false;
            self->m_cpu->draw_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, paint, self->cpuShader(shader));
            self->upload(canvas);
        }
        return;
    }
    QPainterPath path;
    path.addRect(QRectF(left, top, right - left, bottom - top));
    self->paintPath(canvas, path, paint, shader);
}

void MEGLER_CB PreviewGpuCanvasHost::drawRoundRect(void *user, void *handle, float left, float top, float right, float bottom,
                                                  float rx, float ry, const megler_gfx_paint *paint, void *shader)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_round_rect)
            self->m_cpu->draw_round_rect(self->m_cpu->user, handle, left, top, right, bottom, rx, ry, paint, self->cpuShader(shader));
        return;
    }
    if (paint && paint->filter_mode >= 0) {
        QPainterPath path;
        const QRectF rect(left, top, right - left, bottom - top);
        if (rx > 0 && ry > 0)
            path.addRoundedRect(rect, rx, ry, Qt::AbsoluteSize);
        else
            path.addRect(rect);
        if (self->drawFilteredPath(canvas, path, paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            canvas->cpuMirrorCurrent = false;
            self->m_cpu->draw_round_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, rx, ry, paint, self->cpuShader(shader));
            self->upload(canvas);
        }
        return;
    }
    const QRectF rect(left, top, right - left, bottom - top);
    QPainterPath path;
    if (rx > 0 && ry > 0)
        path.addRoundedRect(rect, rx, ry, Qt::AbsoluteSize);
    else
        path.addRect(rect);
    self->paintPath(canvas, path, paint, shader);
}

void MEGLER_CB PreviewGpuCanvasHost::drawOval(void *user, void *handle, float left, float top, float right, float bottom,
                                             const megler_gfx_paint *paint, void *shader)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_oval)
            self->m_cpu->draw_oval(self->m_cpu->user, handle, left, top, right, bottom, paint, self->cpuShader(shader));
        return;
    }
    if (paint && paint->filter_mode >= 0) {
        QPainterPath path;
        path.addEllipse(QRectF(left, top, right - left, bottom - top));
        if (self->drawFilteredPath(canvas, path, paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            canvas->cpuMirrorCurrent = false;
            self->m_cpu->draw_oval(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, paint, self->cpuShader(shader));
            self->upload(canvas);
        }
        return;
    }
    QPainterPath path;
    path.addEllipse(QRectF(left, top, right - left, bottom - top));
    self->paintPath(canvas, path, paint, shader);
}

void MEGLER_CB PreviewGpuCanvasHost::drawPath(void *user, void *handle, const float *path, int count, int fill,
                                             const megler_gfx_paint *paint, void *shader)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_path)
            self->m_cpu->draw_path(self->m_cpu->user, handle, path, count, fill, paint, self->cpuShader(shader));
        return;
    }
    if (paint && paint->filter_mode >= 0) {
        if (self->drawFilteredPath(canvas, pathFrom(path, count, fill), paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            canvas->cpuMirrorCurrent = false;
            self->m_cpu->draw_path(self->m_cpu->user, canvas->cpuCanvas, path, count, fill, paint, self->cpuShader(shader));
            self->upload(canvas);
        }
        return;
    }
    self->paintPath(canvas, pathFrom(path, count, fill), paint, shader);
}

void MEGLER_CB PreviewGpuCanvasHost::drawBitmap(void *user, void *handle, const megler_gfx_image *image,
                                               const float *src, const float *dst, const megler_gfx_paint *paint)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_bitmap)
            self->m_cpu->draw_bitmap(self->m_cpu->user, handle, image, src, dst, paint);
        return;
    }
    if (!image || !image->pixels || !src || !dst || !paint)
        return;
    if (paint->filter_mode >= 0) {
        if (self->drawFilteredBitmap(canvas, image, src, dst, paint))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            canvas->cpuMirrorCurrent = false;
            self->m_cpu->draw_bitmap(self->m_cpu->user, canvas->cpuCanvas, image, src, dst, paint);
            self->upload(canvas);
        }
        return;
    }
    if (image->pixels == canvas->pixels)
        self->readback(canvas, PreviewGpuReadbackReason::Self);
    if (!self->ensureFramebuffer(canvas, "draw-bitmap"))
        return;
    const QImage source(static_cast<const uchar *>(image->pixels), image->width, image->height, image->stride,
                        QImage::Format_ARGB32_Premultiplied);
    const ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previous = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);
    QPainter painter(framebuffer.device);
    const Canvas::State &state = canvas->states.last();
    painter.setRenderHint(QPainter::Antialiasing, (paint->flags & MEGLER_GFX_ANTI_ALIAS) != 0 || state.hasClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, (paint->flags & MEGLER_GFX_FILTER_BITMAP) != 0);
    painter.setWorldTransform(state.matrix);
    painter.setOpacity(state.opacity * qreal(qAlpha(paint->color)) / 255.0);
    if (state.hasClip) {
        bool invertible = false;
        const QTransform inverse = state.matrix.inverted(&invertible);
        if (invertible)
            painter.setClipPath(inverse.map(state.clip), Qt::ReplaceClip);
        else
            painter.setClipRect(QRectF());
    }
    painter.setCompositionMode(compositionMode(paint->blend));
    painter.drawImage(QRectF(dst[0], dst[1], dst[2] - dst[0], dst[3] - dst[1]), source,
                      QRectF(src[0], src[1], src[2] - src[0], src[3] - src[1]));
    painter.end();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    canvas->cpuMirrorCurrent = false;
}

void *MEGLER_CB PreviewGpuCanvasHost::shaderLinear(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors,
                                                  const float *positions, int count, int tile)
{
    if (!colors || count <= 0)
        return nullptr;
    Shader *shader = new Shader;
    QLinearGradient gradient(x0, y0, x1, y1);
    gradient.setSpread(gradientSpread(tile));
    addStops(&gradient, colors, positions, count);
    shader->brush = QBrush(gradient);
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (self && self->m_cpu->shader_linear)
        shader->cpuShader = self->m_cpu->shader_linear(self->m_cpu->user, x0, y0, x1, y1, colors, positions, count, tile);
    return shader;
}

void *MEGLER_CB PreviewGpuCanvasHost::shaderRadial(void *user, float cx, float cy, float radius, const uint32_t *colors,
                                                  const float *positions, int count, int tile)
{
    if (!colors || count <= 0 || radius <= 0)
        return nullptr;
    Shader *shader = new Shader;
    QRadialGradient gradient(cx, cy, radius);
    gradient.setSpread(gradientSpread(tile));
    addStops(&gradient, colors, positions, count);
    shader->brush = QBrush(gradient);
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (self && self->m_cpu->shader_radial)
        shader->cpuShader = self->m_cpu->shader_radial(self->m_cpu->user, cx, cy, radius, colors, positions, count, tile);
    return shader;
}

void *MEGLER_CB PreviewGpuCanvasHost::shaderSweep(void *user, float cx, float cy, const uint32_t *colors,
                                                 const float *positions, int count)
{
    if (!colors || count <= 0)
        return nullptr;
    Shader *shader = new Shader;
    QConicalGradient gradient(cx, cy, 0);
    addStops(&gradient, colors, positions, count);
    shader->brush = QBrush(gradient);
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (self && self->m_cpu->shader_sweep)
        shader->cpuShader = self->m_cpu->shader_sweep(self->m_cpu->user, cx, cy, colors, positions, count);
    return shader;
}

void *MEGLER_CB PreviewGpuCanvasHost::shaderBitmap(void *user, const megler_gfx_image *image, int tile)
{
    if (!image || !image->pixels || image->width <= 0 || image->height <= 0)
        return nullptr;
    const QImage source(static_cast<const uchar *>(image->pixels), image->width, image->height, image->stride,
                        QImage::Format_ARGB32_Premultiplied);
    Shader *shader = new Shader;
    shader->brush = QBrush(source.copy());
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (self && self->m_cpu->shader_bitmap)
        shader->cpuShader = self->m_cpu->shader_bitmap(self->m_cpu->user, image, tile);
    return shader;
}

void MEGLER_CB PreviewGpuCanvasHost::shaderMatrix(void *user, void *handle, const float *matrix)
{
    Shader *shader = static_cast<Shader *>(handle);
    if (!shader)
        return;
    QMutexLocker lock(&shader->mutex);
    shader->brush.setTransform(transformFrom(matrix));
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (self && self->m_cpu->shader_matrix && shader->cpuShader)
        self->m_cpu->shader_matrix(self->m_cpu->user, shader->cpuShader, matrix);
}

void MEGLER_CB PreviewGpuCanvasHost::shaderFree(void *user, void *handle)
{
    Shader *shader = static_cast<Shader *>(handle);
    if (!shader)
        return;
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (self && self->m_cpu->shader_free && shader->cpuShader)
        self->m_cpu->shader_free(self->m_cpu->user, shader->cpuShader);
    delete shader;
}

bool MEGLER_CB PreviewGpuCanvasHost::present(void *user, void *handle, int, int, int, int)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self)
        return false;
    const PreviewGpuMetrics::Sample sample = self->m_metrics->beginPresent();
    QElapsedTimer timer;
    if (sample.sampled)
        timer.start();
    Canvas *canvas = self->findCanvas(handle);
    if (!canvas || (!self->m_present && !self->m_presentPixels) || !self->ensureCurrent("present")) {
        if (sample.enabled)
            self->m_metrics->finishPresent(false, sample, sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0, 0);
        return false;
    }
    if (self->m_presentPixels) {
        const bool read = self->readback(canvas, PreviewGpuReadbackReason::Present);
        const quint64 producerUs = sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0;
        if (sample.sampled && read)
            timer.restart();
        const bool result = read && self->m_presentPixels(self->m_presentUser, canvas->pixels, canvas->width,
                                                         canvas->height, canvas->stride);
        if (sample.enabled)
            self->m_metrics->finishPresent(result, sample, producerUs, 0, read,
                sample.sampled && read ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0);
        return result;
    }
    QOpenGLFunctions *gl = threadContext.context->functions();
    clearGlErrors(gl);
    gl->glFinish();
    if (gl->glGetError() != GL_NO_ERROR) {
        if (sample.enabled)
            self->m_metrics->finishPresent(false, sample, sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0, 0);
        return false;
    }
    const quint64 producerUs = sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0;
    if (sample.sampled)
        timer.restart();
    const bool result = self->m_present(self->m_presentUser, canvas->texture, canvas->width, canvas->height);
    if (sample.enabled) {
        const quint64 qtWaitUs = sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0;
        self->m_metrics->finishPresent(result, sample, producerUs, qtWaitUs);
    }
    return result;
}

bool PreviewGpuCanvasHost::drawTextPath(void *handle, const float *path, int count, int fill,
                                        const megler_gfx_paint *paint, void *shader)
{
    Canvas *canvas = findCanvas(handle);
    if (!canvas)
        return false;
    if (paint && paint->filter_mode >= 0) {
        if (drawFilteredPath(canvas, pathFrom(path, count, fill), paint, shader))
            return true;
        if (readback(canvas, PreviewGpuReadbackReason::Filter)) {
            canvas->cpuMirrorCurrent = false;
            m_cpu->draw_path(m_cpu->user, canvas->cpuCanvas, path, count, fill, paint, cpuShader(shader));
            upload(canvas);
        }
        return true;
    }
    paintPath(canvas, pathFrom(path, count, fill), paint, shader);
    return true;
}

bool PreviewGpuCanvasHost::drawTextFallbackPath(void *handle, const float *path, int count, int fill,
                                                const megler_gfx_paint *paint, void *shader)
{
    Canvas *canvas = findCanvas(handle);
    if (canvas)
        return drawTextPath(handle, path, count, fill, paint, shader);
    if (m_cpu->draw_path)
        m_cpu->draw_path(m_cpu->user, handle, path, count, fill, paint, cpuShader(shader));
    return true;
}

bool PreviewGpuCanvasHost::drawTextBitmap(void *canvas, const megler_gfx_image *image, const float *src,
                                          const float *dst, const megler_gfx_paint *paint)
{
    drawBitmap(this, canvas, image, src, dst, paint);
    return true;
}

bool previewGpuCanvasDrawPath(void *user, void *canvas, const float *path, int count, int fill,
                              const megler_gfx_paint *paint, void *shader)
{
    return user && static_cast<PreviewGpuCanvasHost *>(user)->drawTextPath(canvas, path, count, fill, paint, shader);
}

bool previewGpuCanvasDrawFallbackPath(void *user, void *canvas, const float *path, int count, int fill,
                                      const megler_gfx_paint *paint, void *shader)
{
    return user && static_cast<PreviewGpuCanvasHost *>(user)->drawTextFallbackPath(canvas, path, count, fill, paint, shader);
}

bool previewGpuCanvasDrawBitmap(void *user, void *canvas, const megler_gfx_image *image, const float *src,
                                const float *dst, const megler_gfx_paint *paint)
{
    return user && static_cast<PreviewGpuCanvasHost *>(user)->drawTextBitmap(canvas, image, src, dst, paint);
}
