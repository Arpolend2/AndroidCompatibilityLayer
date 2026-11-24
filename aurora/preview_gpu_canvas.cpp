#include "preview_gpu_canvas.h"
#include "egl_frame_transport.h"
#include "preview_gpu_metrics.h"
#include "preview_gpu_pixels.h"
#include "store_worker_teardown.h"

#include "../core/src/drivers/mg_java_gfx.h"
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
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>
#include <QRegion>
#include <QThread>
#include <QTransform>
#include <QVector>
#include <EGL/egl.h>

#ifdef ACL_HAVE_WAYLAND_EGL
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#endif

#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
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

#ifdef ACL_HAVE_WAYLAND_EGL
struct InvisibleEglWindow {
    EGLDisplay display;
    EGLSurface surface;
    wl_surface *waylandSurface;
    wl_egl_window *window;
};
#endif

std::atomic<quint64> nextContextId{1};
constexpr quint64 multipleContexts = ~quint64(0);

typedef void *GpuSync;
typedef GpuSync (*GpuFenceSyncFn)(GLenum, GLbitfield);
typedef GLenum (*GpuClientWaitSyncFn)(GpuSync, GLbitfield, quint64);
typedef void (*GpuDeleteSyncFn)(GpuSync);

constexpr GLenum gpuSyncGpuCommandsComplete = 0x9117;
constexpr GLenum gpuAlreadySignaled = 0x911a;
constexpr GLenum gpuConditionSatisfied = 0x911c;
constexpr quint64 gpuTimeoutIgnored = ~quint64(0);

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
    EGLContext eglContext = EGL_NO_CONTEXT;
    quint64 contextId = 0;
    QHash<quint64, ThreadFramebuffer> framebuffers;
    GLuint filterProgram = 0;
    GLint filterTintLocation = -1;
    GLint filterOpacityLocation = -1;
    GLint filterInputOpacityLocation = -1;
    GLint filterModeLocation = -1;
    GLint filterTextureLocation = -1;
    GLint filterTexScaleLocation = -1;
    GLint filterTexOffsetLocation = -1;
    GpuFenceSyncFn fenceSync = nullptr;
    GpuClientWaitSyncFn clientWaitSync = nullptr;
    GpuDeleteSyncFn deleteSync = nullptr;
    // The guest thread can outlive the host on an unexpected exit.
    std::shared_ptr<PreviewGpuOffscreenContexts> offscreenContexts;
    long tid = 0;
    bool wasBound = false;

    void clear()
    {
        const bool current = context && QOpenGLContext::currentContext() == context &&
                             eglGetCurrentContext() == eglContext;
        QOpenGLFunctions *gl = current
            ? context->functions() : nullptr;
        if (offscreenContexts) {
            QMutexLocker lock(&offscreenContexts->mutex);
            offscreenContexts->owners[tid].state = "releasing";
        }
        for (auto it = framebuffers.begin(); it != framebuffers.end(); ++it) {
            if (gl) {
                if (it->id) gl->glDeleteFramebuffers(1, &it->id);
                if (it->filterId) gl->glDeleteFramebuffers(1, &it->filterId);
                if (it->stencilBuffer) gl->glDeleteRenderbuffers(1, &it->stencilBuffer);
                if (it->filterStencilBuffer) gl->glDeleteRenderbuffers(1, &it->filterStencilBuffer);
                if (it->filterTexture) gl->glDeleteTextures(1, &it->filterTexture);
            }
            delete it->device;
            delete it->filterDevice;
        }
        framebuffers.clear();
        if (gl && filterProgram)
            gl->glDeleteProgram(filterProgram);
        filterProgram = 0;
        filterTintLocation = filterOpacityLocation = filterInputOpacityLocation = filterModeLocation = filterTextureLocation = -1;
        filterTexScaleLocation = filterTexOffsetLocation = -1;
        fenceSync = nullptr;
        clientWaitSync = nullptr;
        deleteSync = nullptr;
        if (current)
            context->doneCurrent();
        delete context;
        context = nullptr;
        eglContext = EGL_NO_CONTEXT;
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

class ScopedOffscreenCurrent {
public:
    explicit ScopedOffscreenCurrent(PreviewGpuCanvasHost *)
    {
        if (depth++ == 0) {
            previousDisplay = eglGetCurrentDisplay();
            previousDraw = eglGetCurrentSurface(EGL_DRAW);
            previousRead = eglGetCurrentSurface(EGL_READ);
            previousContext = eglGetCurrentContext();
            restorePrevious = previousContext != EGL_NO_CONTEXT &&
                (!threadContext.context || previousContext != threadContext.eglContext);
        }
    }

    ~ScopedOffscreenCurrent()
    {
        if (--depth != 0)
            return;
        if (threadContext.owner) {
            if (!mg_java_gfx_retains_thread_context()) {
                threadContext.owner->releaseThreadContext();
            } else {
                if (threadContext.context &&
                    QOpenGLContext::currentContext() == threadContext.context &&
                    eglGetCurrentContext() == threadContext.eglContext)
                    threadContext.context->doneCurrent();
                if (threadContext.offscreenContexts) {
                    QMutexLocker lock(&threadContext.offscreenContexts->mutex);
                    threadContext.offscreenContexts->owners[threadContext.tid].state = "unbound";
                }
                threadContext.wasBound = false;
            }
        }
        if (restorePrevious &&
            !eglMakeCurrent(previousDisplay, previousDraw, previousRead, previousContext))
            qWarning("GPU Canvas could not restore the guest EGL context after a Canvas callback (EGL error 0x%x)",
                     static_cast<unsigned int>(eglGetError()));
        previousDisplay = EGL_NO_DISPLAY;
        previousDraw = EGL_NO_SURFACE;
        previousRead = EGL_NO_SURFACE;
        previousContext = EGL_NO_CONTEXT;
        restorePrevious = false;
    }

private:
    static thread_local unsigned depth;
    static thread_local EGLDisplay previousDisplay;
    static thread_local EGLSurface previousDraw;
    static thread_local EGLSurface previousRead;
    static thread_local EGLContext previousContext;
    static thread_local bool restorePrevious;
};

thread_local unsigned ScopedOffscreenCurrent::depth = 0;
thread_local EGLDisplay ScopedOffscreenCurrent::previousDisplay = EGL_NO_DISPLAY;
thread_local EGLSurface ScopedOffscreenCurrent::previousDraw = EGL_NO_SURFACE;
thread_local EGLSurface ScopedOffscreenCurrent::previousRead = EGL_NO_SURFACE;
thread_local EGLContext ScopedOffscreenCurrent::previousContext = EGL_NO_CONTEXT;
thread_local bool ScopedOffscreenCurrent::restorePrevious = false;

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
        "uniform float u_inputOpacity;\n"
        "uniform int u_filterMode;\n"
        "uniform vec2 u_texScale;\n"
        "uniform vec2 u_texOffset;\n"
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
        "void main() { vec4 source = texture2D(u_mask, v_texCoord * u_texScale + u_texOffset) * u_inputOpacity; "
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
    const GLint inputOpacity = gl->glGetUniformLocation(program, "u_inputOpacity");
    const GLint mode = gl->glGetUniformLocation(program, "u_filterMode");
    const GLint texture = gl->glGetUniformLocation(program, "u_mask");
    const GLint texScale = gl->glGetUniformLocation(program, "u_texScale");
    const GLint texOffset = gl->glGetUniformLocation(program, "u_texOffset");
    if (tint < 0 || opacity < 0 || inputOpacity < 0 || mode < 0 || texture < 0 ||
        texScale < 0 || texOffset < 0) {
        gl->glDeleteProgram(program);
        qWarning("GPU Canvas color-filter shader uniforms are unavailable");
        return false;
    }
    threadContext.filterProgram = program;
    threadContext.filterTintLocation = tint;
    threadContext.filterOpacityLocation = opacity;
    threadContext.filterInputOpacityLocation = inputOpacity;
    threadContext.filterModeLocation = mode;
    threadContext.filterTextureLocation = texture;
    threadContext.filterTexScaleLocation = texScale;
    threadContext.filterTexOffsetLocation = texOffset;
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

bool isRectangularPath(const QPainterPath &path)
{
    if (path.elementCount() != 5 || !path.elementAt(0).isMoveTo())
        return false;
    const QRectF bounds = path.boundingRect();
    if (bounds.isEmpty())
        return false;
    for (int i = 0; i < 4; ++i) {
        const QPainterPath::Element a = path.elementAt(i);
        const QPainterPath::Element b = path.elementAt(i + 1);
        if (!b.isLineTo() ||
            (a.x != bounds.left() && a.x != bounds.right()) ||
            (a.y != bounds.top() && a.y != bounds.bottom()) ||
            ((a.x == b.x) == (a.y == b.y)))
            return false;
    }
    return path.elementAt(4).x == path.elementAt(0).x &&
           path.elementAt(4).y == path.elementAt(0).y;
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

bool gpuBlendFactors(int blend, GpuBlendFactors *factors, bool rectangularCopy = false)
{
    if (!factors)
        return false;
    factors->sourceRgb = GL_ONE;
    factors->destinationRgb = GL_ONE_MINUS_SRC_ALPHA;
    factors->sourceAlpha = GL_ONE;
    factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
    switch (blend) {
    case MEGLER_GFX_BLEND_CLEAR:
        if (!rectangularCopy) return false;
        factors->sourceRgb = factors->sourceAlpha = GL_ZERO;
        factors->destinationRgb = factors->destinationAlpha = GL_ZERO;
        return true;
    case MEGLER_GFX_BLEND_SRC:
        if (!rectangularCopy) return false;
        factors->destinationRgb = factors->destinationAlpha = GL_ZERO;
        return true;
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
    case MEGLER_GFX_BLEND_SRC_IN:
        if (!rectangularCopy) return false;
        factors->sourceRgb = factors->sourceAlpha = GL_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ZERO;
        return true;
    case MEGLER_GFX_BLEND_DST_IN:
        if (!rectangularCopy) return false;
        factors->sourceRgb = factors->sourceAlpha = GL_ZERO;
        factors->destinationRgb = factors->destinationAlpha = GL_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_SRC_OUT:
        if (!rectangularCopy) return false;
        factors->sourceRgb = factors->sourceAlpha = GL_ONE_MINUS_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ZERO;
        return true;
    case MEGLER_GFX_BLEND_SRC_ATOP:
        factors->sourceRgb = factors->sourceAlpha = GL_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_DST_ATOP:
        if (!rectangularCopy) return false;
        factors->sourceRgb = factors->sourceAlpha = GL_ONE_MINUS_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_XOR:
        factors->sourceRgb = factors->sourceAlpha = GL_ONE_MINUS_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE_MINUS_SRC_ALPHA;
        return true;
    case MEGLER_GFX_BLEND_PLUS:
        factors->sourceRgb = factors->sourceAlpha = GL_ONE;
        factors->destinationRgb = factors->destinationAlpha = GL_ONE;
        return true;
    case MEGLER_GFX_BLEND_MODULATE:
        if (!rectangularCopy) return false;
        factors->sourceRgb = GL_DST_COLOR;
        factors->sourceAlpha = GL_DST_ALPHA;
        factors->destinationRgb = factors->destinationAlpha = GL_ZERO;
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
        QRegion copyClip;
        bool copyClipValid = true;
        int copyClipCuts = 0;
        bool hasClip = false;
        qreal opacity = 1.0;
    };

    quint64 id = 0;
    GLuint texture = 0;
    Bitmap *bitmap = nullptr;
    void *pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    void *cpuCanvas = nullptr;
    bool gpuBacked = true;
    std::atomic<quint64> contextId{0};
    QVector<State> states;

    void noteContext()
    {
        quint64 previous = 0;
        if (!contextId.compare_exchange_strong(previous, threadContext.contextId) &&
            previous != threadContext.contextId && previous != multipleContexts)
            contextId.store(multipleContexts);
    }
};

struct PreviewGpuCanvasHost::Bitmap {
    struct ScopedLocks {
        std::unique_lock<QMutex> first;
        std::unique_lock<QMutex> second;
        ScopedLocks(Bitmap *a, Bitmap *b);
    };

    explicit Bitmap(bool kept) : mutex(QMutex::Recursive), persistent(kept) {}
    QMutex mutex;
    quint32 id = 0;
    quint64 framebufferId = 0;
    GLuint texture = 0;
    int canvasRefs = 0;
    void *pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool persistent = false;
    bool cpuMirrorCurrent = true;
    bool gpuCurrent = true;
    GpuSync gpuFence = nullptr;
    quint64 gpuWriterContextId = 0;
};

PreviewGpuCanvasHost::Bitmap::ScopedLocks::ScopedLocks(Bitmap *a, Bitmap *b)
{
    if (a == b) {
        if (a)
            first = std::unique_lock<QMutex>(a->mutex);
        return;
    }
    Bitmap *low = !a ? b : !b ? a : std::less<Bitmap *>()(a, b) ? a : b;
    Bitmap *high = low == a ? b : a;
    if (low)
        first = std::unique_lock<QMutex>(low->mutex);
    if (high)
        second = std::unique_lock<QMutex>(high->mutex);
}

struct PreviewGpuCanvasHost::Shader {
    QMutex mutex;
    QBrush brush;
    void *cpuShader = nullptr;
};

struct PreviewGpuCanvasHost::EglTexture {
    AuroraEglFrame frame = {};
    ~EglTexture() { aurora_egl_frame_release(&frame); }
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
    if (!enableNativeEglTextures())
        qWarning("WebView GPU: native EGL texture capture unavailable; software fallback");
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

bool PreviewGpuCanvasHost::prepareRemotePresentationWithCpuFallback(
    QOpenGLContext *shareContext, Present presentCallback, Release releaseCallback,
    PresentPixels fallback, void *presentUser)
{
    if (!fallback || !prepareRemotePresentation(shareContext, presentCallback, releaseCallback, presentUser))
        return false;
    if (m_presentPixels && m_presentPixels != fallback) return false;
    m_presentPixels = fallback;
    return true;
}

bool PreviewGpuCanvasHost::enableNativeEglTextures()
{
    if (m_nativeEglTextures)
        return true;
    QOpenGLContext *previous = QOpenGLContext::currentContext();
    QSurface *previousSurface = previous ? previous->surface() : nullptr;
    if (!m_surface || !m_shareContext)
        return false;
    // The share context belongs to the Qt render thread; probe with a same-group context owned by this thread.
    QOpenGLContext probe;
    probe.setFormat(m_shareContext->format());
    probe.setShareContext(m_shareContext.data());
    if (!probe.create() || probe.shareGroup() != m_shareContext->shareGroup() || !probe.makeCurrent(m_surface))
        return false;
    const EGLDisplay display = eglGetCurrentDisplay();
#ifdef ACL_HAVE_WAYLAND_EGL
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        if (native)
            m_waylandCompositor = native->nativeResourceForIntegration(QByteArrayLiteral("compositor"));
    }
#endif
    m_nativeEglDisplay = display;
    const QByteArray extensions = display == EGL_NO_DISPLAY ? QByteArray() :
        QByteArray(eglQueryString(display, EGL_EXTENSIONS));
    const auto has = [&extensions](const char *name) {
        return (QByteArray(" ") + extensions + " ").contains(QByteArray(" ") + name + " ");
    };
    bool ready = has("EGL_KHR_image_base") && has("EGL_KHR_gl_texture_2D_image") &&
        has("EGL_KHR_fence_sync") && has("EGL_KHR_wait_sync") &&
        probe.hasExtension(QByteArrayLiteral("GL_OES_EGL_image"));
    if (ready) {
        EGLint configAttrs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
        };
        const EGLint surfaceAttrs[] = { EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE };
        EGLConfig config = nullptr;
        EGLint count = 0;
        ready = eglChooseConfig(display, configAttrs, &config, 1, &count) && count == 1;
        bool hostSurface = false;
        if (!ready && m_waylandCompositor) {
            configAttrs[1] = EGL_WINDOW_BIT;
            count = 0;
            ready = eglChooseConfig(display, configAttrs, &config, 1, &count) && count == 1;
            hostSurface = ready;
        }
        if (ready) {
            void *token = nullptr;
            EGLSurface surface = hostSurface
                ? static_cast<EGLSurface>(eglSurfaceNew(this, display, config, 8, 8, &token))
                : eglCreatePbufferSurface(display, config, surfaceAttrs);
            const EGLint contextAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            EGLContext producer = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
            const EGLContext original = eglGetCurrentContext();
            const EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
            const EGLSurface read = eglGetCurrentSurface(EGL_READ);
            AuroraEglFrame frame = {};
            char failure[256] = {};
            ready = surface != EGL_NO_SURFACE && producer != EGL_NO_CONTEXT &&
                    eglMakeCurrent(display, surface, surface, producer);
            if (ready)
                ready = aurora_egl_frame_capture(display, surface, 8, 8, 0,
                                                  &frame, failure, sizeof(failure)) == AURORA_EGL_FRAME_READY;
            const bool restored = eglMakeCurrent(display, draw, read, original);
            ready = ready && restored;
            if (ready) {
                const GLuint imported = aurora_egl_frame_import(&frame);
                ready = imported != 0;
                if (imported) probe.functions()->glDeleteTextures(1, &imported);
            }
            aurora_egl_frame_release(&frame);
            if (token) eglSurfaceFree(this, token);
            else if (surface != EGL_NO_SURFACE && !eglDestroySurface(display, surface)) ready = false;
            if (producer != EGL_NO_CONTEXT && !eglDestroyContext(display, producer)) ready = false;
            if (!ready && failure[0]) qWarning("GPU Canvas EGL probe: %s", failure);
        }
        if (!ready)
            qWarning("GPU Canvas EGL display cannot capture and import a WebView surface");
    }
    probe.doneCurrent();
    if (previous && previousSurface && !previous->makeCurrent(previousSurface)) {
        qWarning("GPU Canvas could not restore the previous context after the EGL capability probe");
        m_nativeEglDisplay = nullptr;
        m_nativeEglTextures = false;
        return false;
    }
    m_nativeEglTextures = ready;
    m_nativeEglDisplay = ready ? display : nullptr;
    return ready;
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
    host->canvas_new_bitmap = canvasNewBitmap;
    host->canvas_free = canvasFree;
    host->canvas_discard = canvasDiscard;
    host->flush = flush;
    host->touched = touched;
    host->bitmap_readback = bitmapReadback;
    host->bitmap_changed = bitmapChanged;
    host->bitmap_gone = bitmapGone;
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
    host->draw_canvas = drawCanvas;
    host->shader_linear = shaderLinear;
    host->shader_radial = shaderRadial;
    host->shader_sweep = shaderSweep;
    host->shader_bitmap = shaderBitmap;
    host->shader_matrix = shaderMatrix;
    host->shader_free = shaderFree;
    host->present = present;
    if (m_offscreenContexts) host->release_thread = releaseThread;
    if (m_nativeEglTextures) {
        host->texture_egl_frame = textureEglFrame;
        host->draw_texture = drawTexture;
        if (m_waylandCompositor) {
            host->egl_surface_configs = eglSurfaceConfigs;
            host->egl_surface_new = eglSurfaceNew;
            host->egl_surface_free = eglSurfaceFree;
        }
    }
}

int MEGLER_CB PreviewGpuCanvasHost::eglSurfaceConfigs(void *user, void *display, int version,
                                                       void **configs, int capacity)
{
    auto *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self || !self->m_waylandCompositor || display != self->m_nativeEglDisplay ||
        !configs || capacity <= 0 || (version != 2 && version != 3))
        return 0;
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, version == 3 ? 0x40 : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLint count = 0;
    return eglChooseConfig(static_cast<EGLDisplay>(display), attrs, configs, capacity, &count)
        ? qMin(count, capacity) : 0;
}

void *MEGLER_CB PreviewGpuCanvasHost::eglSurfaceNew(void *user, void *display, void *config,
                                                    int width, int height, void **token)
{
    if (token) *token = nullptr;
#ifdef ACL_HAVE_WAYLAND_EGL
    auto *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self || !token || !self->m_waylandCompositor || display != self->m_nativeEglDisplay ||
        !config || width <= 0 || height <= 0)
        return nullptr;
    auto *native = new InvisibleEglWindow{static_cast<EGLDisplay>(display), EGL_NO_SURFACE, nullptr, nullptr};
    native->waylandSurface = wl_compositor_create_surface(static_cast<wl_compositor *>(self->m_waylandCompositor));
    if (native->waylandSurface)
        native->window = wl_egl_window_create(native->waylandSurface, width, height);
    if (native->window)
        native->surface = eglCreateWindowSurface(native->display, static_cast<EGLConfig>(config),
                                                 reinterpret_cast<EGLNativeWindowType>(native->window), nullptr);
    if (native->surface == EGL_NO_SURFACE) {
        if (native->window) wl_egl_window_destroy(native->window);
        if (native->waylandSurface) wl_surface_destroy(native->waylandSurface);
        delete native;
        return nullptr;
    }
    *token = native;
    return native->surface;
#else
    Q_UNUSED(user) Q_UNUSED(display) Q_UNUSED(config) Q_UNUSED(width) Q_UNUSED(height)
    return nullptr;
#endif
}

void MEGLER_CB PreviewGpuCanvasHost::eglSurfaceFree(void *user, void *token)
{
    Q_UNUSED(user)
#ifdef ACL_HAVE_WAYLAND_EGL
    auto *native = static_cast<InvisibleEglWindow *>(token);
    if (!native) return;
    if (!eglDestroySurface(native->display, native->surface))
        qWarning("GPU Canvas could not destroy its invisible EGL window surface");
    wl_egl_window_destroy(native->window);
    wl_surface_destroy(native->waylandSurface);
    delete native;
#else
    Q_UNUSED(token)
#endif
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
        PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
        self->releaseThreadContext();
        std::snprintf(stage, sizeof(stage), "GPU Canvas release thread complete tid=%ld", tid);
        traceStoreWorkerTeardown(stage);
    }
}

void PreviewGpuCanvasHost::releaseThreadContext()
{
    if (threadContext.owner != this)
        return;
    if (threadContext.context &&
        (QOpenGLContext::currentContext() != threadContext.context ||
         eglGetCurrentContext() != threadContext.eglContext) &&
        !ensureCurrent("release-thread"))
        qWarning("GPU Canvas could not make its context current during thread cleanup");
    threadContext.clear();
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
        threadContext.contextId = nextContextId.fetch_add(1, std::memory_order_relaxed);
        if (m_offscreenContexts) {
            threadContext.offscreenContexts = m_offscreenContexts;
            threadContext.tid = ::syscall(SYS_gettid);
            char name[16] = {};
            pthread_getname_np(pthread_self(), name, sizeof(name));
            QMutexLocker lock(&m_offscreenContexts->mutex);
            m_offscreenContexts->owners.insert(threadContext.tid, {QByteArray(name), origin, "unbound"});
        }
    }
    if (QOpenGLContext::currentContext() != threadContext.context ||
        eglGetCurrentContext() != threadContext.eglContext) {
        if (QOpenGLContext::currentContext() == threadContext.context)
            threadContext.context->doneCurrent();
        if (!threadContext.context->makeCurrent(m_surface))
            return false;
        threadContext.eglContext = eglGetCurrentContext();
        if (threadContext.eglContext == EGL_NO_CONTEXT)
            return false;
    }
    if (threadContext.offscreenContexts && !threadContext.wasBound) {
        QMutexLocker lock(&threadContext.offscreenContexts->mutex);
        threadContext.offscreenContexts->owners[threadContext.tid].state = "was-bound";
        threadContext.wasBound = true;
    }
    if (threadContext.context->isOpenGLES() && threadContext.context->format().majorVersion() >= 3 &&
        !threadContext.fenceSync) {
        threadContext.fenceSync = reinterpret_cast<GpuFenceSyncFn>(
            threadContext.context->getProcAddress("glFenceSync"));
        threadContext.clientWaitSync = reinterpret_cast<GpuClientWaitSyncFn>(
            threadContext.context->getProcAddress("glClientWaitSync"));
        threadContext.deleteSync = reinterpret_cast<GpuDeleteSyncFn>(
            threadContext.context->getProcAddress("glDeleteSync"));
        if (!threadContext.fenceSync || !threadContext.clientWaitSync || !threadContext.deleteSync) {
            threadContext.fenceSync = nullptr;
            threadContext.clientWaitSync = nullptr;
            threadContext.deleteSync = nullptr;
        }
    }
    threadContext.context->functions()->initializeOpenGLFunctions();
    return true;
}

bool PreviewGpuCanvasHost::waitForBitmap(Bitmap *bitmap, bool forceWait)
{
    if (!bitmap || !bitmap->gpuFence ||
        (!forceWait && bitmap->gpuWriterContextId == threadContext.contextId))
        return true;
    if (!threadContext.clientWaitSync) {
        qWarning("GPU Canvas cannot synchronize a bitmap written by another context");
        return false;
    }
    const GLenum result = threadContext.clientWaitSync(bitmap->gpuFence, 0, gpuTimeoutIgnored);
    if (result != gpuAlreadySignaled && result != gpuConditionSatisfied) {
        qWarning("GPU Canvas waiting for another context's bitmap write failed (0x%x)",
                 static_cast<unsigned int>(result));
        return false;
    }
    return true;
}

bool PreviewGpuCanvasHost::ensureFramebuffer(Canvas *canvas, const char *origin)
{
    if (!canvas || !canvas->gpuBacked || !ensureCurrent(origin))
        return false;
    canvas->noteContext();
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
    if (!canvas || !canvas->bitmap)
        return false;
    QMutexLocker bitmapLock(&canvas->bitmap->mutex);
    if (canvas->bitmap->cpuMirrorCurrent) {
        m_metrics->recordReadbackCacheHit(reason);
        return true;
    }
    if (!canvas->gpuBacked)
        return false;
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
    if (!waitForBitmap(canvas->bitmap)) {
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
    canvas->bitmap->cpuMirrorCurrent = true;
    return true;
}

bool PreviewGpuCanvasHost::upload(Canvas *canvas)
{
    if (!canvas || !canvas->bitmap || !canvas->gpuBacked || !ensureCurrent("upload"))
        return false;
    QMutexLocker bitmapLock(&canvas->bitmap->mutex);
    if (!waitForBitmap(canvas->bitmap))
        return false;
    canvas->noteContext();
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
    GLint unpackAlignment = 4;
    GLint unpackBuffer = 0;
    GLint unpackRowLength = 0;
    GLint unpackSkipPixels = 0;
    GLint unpackSkipRows = 0;
    const bool gles3 = threadContext.context->isOpenGLES() &&
                       threadContext.context->format().majorVersion() >= 3;
    clearGlErrors(gl);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    if (gles3) {
        gl->glGetIntegerv(0x88EF, &unpackBuffer); // GL_PIXEL_UNPACK_BUFFER_BINDING
        gl->glGetIntegerv(0x0CF2, &unpackRowLength); // GL_UNPACK_ROW_LENGTH
        gl->glGetIntegerv(0x0CF3, &unpackSkipRows); // GL_UNPACK_SKIP_ROWS
        gl->glGetIntegerv(0x0CF4, &unpackSkipPixels); // GL_UNPACK_SKIP_PIXELS
        gl->glBindBuffer(0x88EC, 0); // GL_PIXEL_UNPACK_BUFFER
        gl->glPixelStorei(0x0CF2, 0);
        gl->glPixelStorei(0x0CF3, 0);
        gl->glPixelStorei(0x0CF4, 0);
    }
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glBindTexture(GL_TEXTURE_2D, canvas->texture);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvas->width, canvas->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constData());
    const GLenum error = gl->glGetError();
    gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    if (gles3) {
        gl->glPixelStorei(0x0CF2, unpackRowLength);
        gl->glPixelStorei(0x0CF3, unpackSkipRows);
        gl->glPixelStorei(0x0CF4, unpackSkipPixels);
        gl->glBindBuffer(0x88EC, static_cast<GLuint>(unpackBuffer));
    }
    if (error != GL_NO_ERROR)
        qWarning("GPU Canvas upload failed (OpenGL error 0x%x)", static_cast<unsigned int>(error));
    else {
        gl->glFinish();
        canvas->bitmap->cpuMirrorCurrent = true;
        canvas->bitmap->gpuCurrent = true;
    }
    return error == GL_NO_ERROR;
}

bool PreviewGpuCanvasHost::ensureBitmapCurrent(Canvas *canvas)
{
    if (!canvas || !canvas->bitmap || !canvas->gpuBacked)
        return false;
    QMutexLocker bitmapLock(&canvas->bitmap->mutex);
    if (!ensureCurrent("bitmap-current"))
        return false;
    if (canvas->bitmap->gpuCurrent)
        return waitForBitmap(canvas->bitmap);
    return canvas->bitmap->cpuMirrorCurrent && upload(canvas);
}

bool PreviewGpuCanvasHost::ensureCpuMirrorCurrent(Bitmap *bitmap)
{
    if (!bitmap)
        return false;
    QMutexLocker bitmapLock(&bitmap->mutex);
    if (bitmap->cpuMirrorCurrent)
        return true;
    if (!bitmap->gpuCurrent || !bitmap->texture)
        return false;
    const bool hadContext = threadContext.owner == this && threadContext.context;
    Canvas canvas;
    canvas.bitmap = bitmap;
    canvas.texture = bitmap->texture;
    canvas.id = bitmap->framebufferId;
    canvas.pixels = bitmap->pixels;
    canvas.width = bitmap->width;
    canvas.height = bitmap->height;
    canvas.stride = bitmap->stride;
    const bool result = readback(&canvas, PreviewGpuReadbackReason::Self);
    removeFramebuffer(&canvas);
    if (!hadContext && threadContext.owner == this && threadContext.framebuffers.isEmpty())
        threadContext.clear();
    return result;
}

void PreviewGpuCanvasHost::finishGpuWrite(Canvas *canvas)
{
    if (!canvas || !canvas->bitmap || !canvas->gpuBacked || !threadContext.context)
        return;
    QMutexLocker bitmapLock(&canvas->bitmap->mutex);
    QOpenGLFunctions *gl = threadContext.context->functions();
    canvas->bitmap->cpuMirrorCurrent = false;
    canvas->bitmap->gpuCurrent = false;
    GLenum error = gl->glGetError();
    if (error != GL_NO_ERROR) {
        qWarning("GPU Canvas drawing completion failed (OpenGL error 0x%x)", static_cast<unsigned int>(error));
        gl->glFinish();
        error = gl->glGetError();
        if (error != GL_NO_ERROR)
            qWarning("GPU Canvas drawing completion failed (OpenGL error 0x%x)", static_cast<unsigned int>(error));
        return;
    }
    if (threadContext.fenceSync && threadContext.deleteSync) {
        GpuSync fence = threadContext.fenceSync(gpuSyncGpuCommandsComplete, 0);
        if (fence && gl->glGetError() == GL_NO_ERROR) {
            gl->glFlush();
            error = gl->glGetError();
            if (error == GL_NO_ERROR) {
                if (canvas->bitmap->gpuFence)
                    threadContext.deleteSync(canvas->bitmap->gpuFence);
                canvas->bitmap->gpuFence = fence;
                canvas->bitmap->gpuWriterContextId = threadContext.contextId;
                canvas->bitmap->gpuCurrent = true;
                return;
            }
            threadContext.deleteSync(fence);
        }
        clearGlErrors(gl);
    }

    gl->glFinish();
    error = gl->glGetError();
    if (error != GL_NO_ERROR)
        qWarning("GPU Canvas drawing completion failed (OpenGL error 0x%x)", static_cast<unsigned int>(error));
    else {
        if (canvas->bitmap->gpuFence && threadContext.deleteSync)
            threadContext.deleteSync(canvas->bitmap->gpuFence);
        canvas->bitmap->gpuFence = nullptr;
        canvas->bitmap->gpuWriterContextId = threadContext.contextId;
        canvas->bitmap->gpuCurrent = true;
    }
}

void PreviewGpuCanvasHost::markCpuWrite(Bitmap *bitmap)
{
    if (!bitmap)
        return;
    QMutexLocker bitmapLock(&bitmap->mutex);
    bitmap->cpuMirrorCurrent = true;
    bitmap->gpuCurrent = false;
}

bool PreviewGpuCanvasHost::drawFiltered(Canvas *canvas, const megler_gfx_paint *paint,
                                        const QRectF &deviceBounds,
                                        const std::function<void(QPainter &)> &drawSource,
                                        Canvas *copyCanvas, const QRect &copySource)
{
    if (!canvas || !paint)
        return false;
    GpuBlendFactors blendFactors;
    if (!gpuBlendFactors(paint->blend, &blendFactors, copyCanvas != nullptr)) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedBlend);
        return false;
    }
    if (!copyCanvas && paint->blend != MEGLER_GFX_BLEND_SRC_OVER && !filterScalesWithSource(paint->filter_mode)) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedBlend);
        return false;
    }
    const bool scaledCopy = copyCanvas && QSizeF(copySource.size()) != deviceBounds.size();
    const bool shaderScaledCopy = scaledCopy && threadContext.context->isOpenGLES() &&
                                  threadContext.context->format().majorVersion() < 3;
    const Canvas::State &state = canvas->states.last();
    if (copyCanvas && !state.copyClipValid)
        return false;
    if (scaledCopy && !shaderScaledCopy) {
        GLint readFramebuffer = 0, drawFramebuffer = 0;
        auto *gl = threadContext.context->functions();
        gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        // The scratch FBO allocator and paint path assume a single saved binding.
        if (readFramebuffer != drawFramebuffer)
            return false;
    }
    if (!ensureFilterFramebuffer(canvas) || !ensureFilterProgram()) {
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }

    QRectF clippedBounds = deviceBounds.intersected(QRectF(0, 0, canvas->width, canvas->height));
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
    const QRegion copyClip = copyCanvas
        ? state.copyClip.intersected(QRect(scissorLeft, scissorTop,
                                           scissorRight - scissorLeft, scissorBottom - scissorTop))
        : QRegion();
    if (copyCanvas && copyClip.isEmpty())
        return true;

    ThreadFramebuffer framebuffer = threadContext.framebuffers.value(canvas->id);
    QOpenGLFunctions *gl = threadContext.context->functions();
    GLint previousFramebuffer = 0;
    GLuint scaledSnapshot = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    if (shaderScaledCopy) {
        GLint previousTexture = 0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, threadContext.framebuffers.value(copyCanvas->id).id);
        gl->glGenTextures(1, &scaledSnapshot);
        gl->glBindTexture(GL_TEXTURE_2D, scaledSnapshot);
        const GLint filtering = (paint->flags & MEGLER_GFX_FILTER_BITMAP) ? GL_LINEAR : GL_NEAREST;
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, copySource.width(), copySource.height(), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, copySource.x(),
                                copyCanvas->height - copySource.y() - copySource.height(),
                                copySource.width(), copySource.height());
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    } else if (scaledCopy) {
        auto *extra = threadContext.context->extraFunctions();
        const auto target = deviceBounds.toRect();
        const bool smooth = (paint->flags & MEGLER_GFX_FILTER_BITMAP) != 0;
        GLuint snapshotTexture = 0, snapshotFramebuffer = 0;
        GLint previousTexture = 0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, threadContext.framebuffers.value(copyCanvas->id).id);
        if (smooth) {
            gl->glGenTextures(1, &snapshotTexture);
            gl->glBindTexture(GL_TEXTURE_2D, snapshotTexture);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, copySource.width(), copySource.height(), 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, copySource.x(),
                                    copyCanvas->height - copySource.y() - copySource.height(),
                                    copySource.width(), copySource.height());
            gl->glGenFramebuffers(1, &snapshotFramebuffer);
            gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, snapshotFramebuffer);
            gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, snapshotTexture, 0);
            const bool ready = snapshotTexture && snapshotFramebuffer &&
                gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                gl->glGetError() == GL_NO_ERROR;
            if (!ready) {
                gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
                gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
                if (snapshotFramebuffer) gl->glDeleteFramebuffers(1, &snapshotFramebuffer);
                if (snapshotTexture) gl->glDeleteTextures(1, &snapshotTexture);
                clearGlErrors(gl);
                m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
                return false;
            }
        }
        const bool scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
        if (scissor)
            gl->glDisable(GL_SCISSOR_TEST);
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer.filterId);
        const int sourceLeft = smooth ? 0 : copySource.x();
        const int sourceBottom = smooth ? 0 : copyCanvas->height - copySource.y() - copySource.height();
        extra->glBlitFramebuffer(sourceLeft, sourceBottom,
                                 sourceLeft + copySource.width(), sourceBottom + copySource.height(),
                                 target.x(), canvas->height - target.y() - target.height(),
                                 target.x() + target.width(), canvas->height - target.y(),
                                 GL_COLOR_BUFFER_BIT, smooth ? GL_LINEAR : GL_NEAREST);
        if (scissor)
            gl->glEnable(GL_SCISSOR_TEST);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        if (snapshotFramebuffer) gl->glDeleteFramebuffers(1, &snapshotFramebuffer);
        if (snapshotTexture) gl->glDeleteTextures(1, &snapshotTexture);
    } else if (copyCanvas) {
        const int width = scissorRight - scissorLeft, height = scissorBottom - scissorTop;
        const QPoint origin = copySource.topLeft() + QPoint(scissorLeft, scissorTop) - deviceBounds.topLeft().toPoint();
        GLint previousTexture = 0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, threadContext.framebuffers.value(copyCanvas->id).id);
        gl->glBindTexture(GL_TEXTURE_2D, framebuffer.filterTexture);
        // Snapshot into a different texture before blending, including for a self-copy.
        gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, scissorLeft, canvas->height - scissorBottom,
                               origin.x(), copyCanvas->height - origin.y() - height, width, height);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    } else {
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
    }
    const GLenum maskError = gl->glGetError();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    if (maskError != GL_NO_ERROR) {
        qWarning("GPU Canvas color-filter mask failed (OpenGL error 0x%x)", static_cast<unsigned int>(maskError));
        if (scaledSnapshot)
            gl->glDeleteTextures(1, &scaledSnapshot);
        clearGlErrors(gl);
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }

    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.id);
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
    const GLboolean sampleCoverageEnabled = gl->glIsEnabled(GL_SAMPLE_COVERAGE);
    const GLboolean alphaToCoverageEnabled = gl->glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
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
    gl->glDisable(GL_SAMPLE_COVERAGE);
    gl->glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl->glBlendFuncSeparate(blendFactors.sourceRgb, blendFactors.destinationRgb,
                            blendFactors.sourceAlpha, blendFactors.destinationAlpha);
    gl->glUseProgram(threadContext.filterProgram);
    gl->glBindTexture(GL_TEXTURE_2D, shaderScaledCopy ? scaledSnapshot : framebuffer.filterTexture);
    const QColor tint = QColor::fromRgba(paint->filter_color);
    const GLfloat tintAlpha = static_cast<GLfloat>(tint.alphaF());
    gl->glUniform4f(threadContext.filterTintLocation,
                    static_cast<GLfloat>(tint.redF()) * tintAlpha,
                    static_cast<GLfloat>(tint.greenF()) * tintAlpha,
                    static_cast<GLfloat>(tint.blueF()) * tintAlpha,
                    tintAlpha);
    gl->glUniform1f(threadContext.filterOpacityLocation, static_cast<GLfloat>(canvas->states.last().opacity));
    gl->glUniform1f(threadContext.filterInputOpacityLocation,
                    copyCanvas ? static_cast<GLfloat>(qAlpha(paint->color) / 255.0) : 1.0f);
    gl->glUniform1i(threadContext.filterModeLocation, paint->filter_mode);
    gl->glUniform1i(threadContext.filterTextureLocation, 0);
    gl->glUniform2f(threadContext.filterTexScaleLocation,
                    shaderScaledCopy ? canvas->width / deviceBounds.width() : 1.0,
                    shaderScaledCopy ? canvas->height / deviceBounds.height() : 1.0);
    gl->glUniform2f(threadContext.filterTexOffsetLocation,
                    shaderScaledCopy ? -deviceBounds.left() / deviceBounds.width() : 0.0,
                    shaderScaledCopy ? -(canvas->height - deviceBounds.bottom()) / deviceBounds.height() : 0.0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);

    GLenum error = gl->glGetError();
    if (error == GL_NO_ERROR) {
        if (copyCanvas) {
            for (const QRect &rect : copyClip.rects()) {
                gl->glScissor(rect.x(), canvas->height - rect.y() - rect.height(),
                              rect.width(), rect.height());
                gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        } else {
            gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
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
    restoreEnabled(GL_SAMPLE_COVERAGE, sampleCoverageEnabled);
    restoreEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE, alphaToCoverageEnabled);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    if (scaledSnapshot)
        gl->glDeleteTextures(1, &scaledSnapshot);

    if (error != GL_NO_ERROR) {
        qWarning("GPU Canvas color-filter setup failed before drawing (OpenGL error 0x%x)",
                 static_cast<unsigned int>(error));
        clearGlErrors(gl);
        m_metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
        return false;
    }
    finishGpuWrite(canvas);
    if (drawError != GL_NO_ERROR) {
        qWarning("GPU Canvas color-filter draw reported OpenGL error 0x%x", static_cast<unsigned int>(drawError));
        m_metrics->recordFilterDrawError();
    } else if (copyCanvas) {
        m_metrics->recordCanvasCopy();
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
        if (image->width == canvas->width && image->height == canvas->height &&
            image->stride == canvas->stride && copyCanvasPixels(canvas, canvas, src, dst, paint))
            return true;
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

bool PreviewGpuCanvasHost::copyCanvasPixels(Canvas *canvas, Canvas *source, const float *src, const float *dst,
                                            const megler_gfx_paint *paint)
{
    Bitmap::ScopedLocks bitmapLocks(canvas ? canvas->bitmap : nullptr,
                                    source ? source->bitmap : nullptr);
    if (!canvas || !source || !canvas->gpuBacked || !source->gpuBacked ||
        !ensureBitmapCurrent(canvas) || !ensureBitmapCurrent(source) || !src || !dst || !paint ||
        paint->filter_mode > MEGLER_GFX_BLEND_LIGHTEN || source->states.size() != 1)
        return false;
    const Canvas::State &state = canvas->states.last();
    const auto integral = [](qreal n) {
        return n >= -16777216 && n <= 16777216 && n == std::floor(n);
    };
    if (state.matrix.type() > QTransform::TxTranslate ||
        !integral(state.matrix.dx()) || !integral(state.matrix.dy()))
        return false;
    for (int i = 0; i < 4; ++i) {
        if (!integral(src[i]) || !integral(dst[i]))
            return false;
    }
    if (src[0] < 0 || src[1] < 0 || src[2] > source->width || src[3] > source->height ||
        src[2] <= src[0] || src[3] <= src[1])
        return false;

    const QRectF sourceRect(src[0], src[1], qreal(src[2]) - src[0], qreal(src[3]) - src[1]);
    const QRectF bounds(dst[0] + state.matrix.dx(), dst[1] + state.matrix.dy(),
                        qreal(dst[2]) - dst[0], qreal(dst[3]) - dst[1]);
    if (bounds.width() <= 0 || bounds.height() <= 0)
        return false;
    const bool scaled = sourceRect.size() != bounds.size();
    const auto exactNearestScale = [](qreal from, qreal to) {
        int numerator = static_cast<int>(from);
        int denominator = static_cast<int>(to);
        while (denominator) {
            const int remainder = numerator % denominator;
            numerator = denominator;
            denominator = remainder;
        }
        return (static_cast<int>(from) / numerator) % 2 == 1;
    };
    if (scaled && (m_offscreenContexts ||
                   (!(paint->flags & MEGLER_GFX_FILTER_BITMAP) &&
                    (!exactNearestScale(sourceRect.width(), bounds.width()) ||
                     !exactNearestScale(sourceRect.height(), bounds.height())))))
        return false;
    if (!state.copyClipValid)
        return false;

    if (paint->blend == MEGLER_GFX_BLEND_DST)
        return true;
    GpuBlendFactors factors;
    if (!gpuBlendFactors(paint->blend, &factors, true))
        return false;
    megler_gfx_paint copyPaint = *paint;
    if (copyPaint.filter_mode < 0)
        copyPaint.filter_mode = MEGLER_GFX_BLEND_DST; // Identity filter.
    if (!ensureFramebuffer(source, "copy-canvas"))
        return false;
    if (scaled && !threadContext.context->isOpenGLES())
        return false;
    return drawFiltered(canvas, &copyPaint, bounds, {}, source, sourceRect.toRect());
}

bool MEGLER_CB PreviewGpuCanvasHost::drawCanvas(void *user, void *handle, void *sourceHandle, const float *src,
                                                const float *dst, const megler_gfx_paint *paint)
{
    auto *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    Canvas *source = self ? self->findCanvas(sourceHandle) : nullptr;
    if (!canvas || !source || canvas == source || canvas->pixels == source->pixels)
        return false;
    Bitmap::ScopedLocks bitmapLocks(canvas->bitmap, source->bitmap);
    if (!canvas->gpuBacked || !source->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(source->bitmap))
            return false;
        if (canvas->gpuBacked && (!self->ensureBitmapCurrent(canvas) ||
                                  !self->readback(canvas, PreviewGpuReadbackReason::Self)))
            return false;
        if (!canvas->gpuBacked && !self->ensureCpuMirrorCurrent(canvas->bitmap))
            return false;
        if (!self->m_cpu->draw_canvas ||
            !self->m_cpu->draw_canvas(self->m_cpu->user, canvas->cpuCanvas, source->cpuCanvas,
                                      src, dst, paint))
            return false;
        self->markCpuWrite(canvas->bitmap);
        return !canvas->gpuBacked || self->upload(canvas);
    }
    return self->copyCanvasPixels(canvas, source, src, dst, paint);
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

void PreviewGpuCanvasHost::clip(Canvas *canvas, const QPainterPath &path, int op, bool rectangular)
{
    if (!canvas || canvas->states.isEmpty())
        return;
    Canvas::State &state = canvas->states.last();
    const QPainterPath devicePath = state.matrix.map(path);
    if (state.copyClipValid) {
        const QRectF rect = devicePath.boundingRect();
        const auto integral = [](qreal value) {
            return value >= -16777216 && value <= 16777216 && value == std::floor(value);
        };
        if (!rectangular || state.matrix.type() > QTransform::TxTranslate ||
            !integral(rect.left()) || !integral(rect.top()) ||
            !integral(rect.right()) || !integral(rect.bottom()) ||
            (op != MEGLER_GFX_CLIP_INTERSECT && op != MEGLER_GFX_CLIP_DIFFERENCE) ||
            (op == MEGLER_GFX_CLIP_DIFFERENCE && ++state.copyClipCuts > 8)) {
            state.copyClipValid = false;
        } else {
            const QRect deviceRect = rect.toRect();
            if (op == MEGLER_GFX_CLIP_DIFFERENCE)
                state.copyClip -= QRegion(deviceRect);
            else
                state.copyClip &= QRegion(deviceRect);
        }
    }
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
    finishGpuWrite(canvas);
}

void *MEGLER_CB PreviewGpuCanvasHost::canvasNew(void *user, void *pixels, int width, int height, int stride)
{
    return canvasNewBitmap(user, pixels, width, height, stride, 0);
}

void *MEGLER_CB PreviewGpuCanvasHost::canvasNewBitmap(void *user, void *pixels, int width, int height, int stride,
                                                      uint32_t bitmapId)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    if (!self)
        return nullptr;
    const auto cpuCanvasNew = [self, pixels, width, height, stride]() {
        return self->m_cpu->canvas_new
            ? self->m_cpu->canvas_new(self->m_cpu->user, pixels, width, height, stride) : nullptr;
    };
    if (!pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 ||
        height > INT_MAX / (width * 4) || stride < width * 4)
        return cpuCanvasNew();
    const bool hadContext = threadContext.owner == self && threadContext.context;
    const auto cleanupTemporaryContext = [self, hadContext]() {
        if (hadContext || threadContext.owner != self || !threadContext.framebuffers.isEmpty())
            return;
        threadContext.clear();
    };
    const auto cpuCanvasFallback = [self, pixels, width, height, stride, bitmapId, cpuCanvasNew,
                                    cleanupTemporaryContext]() -> void * {
        bool safe = true;
        if (bitmapId) {
            Bitmap *bitmap = nullptr;
            {
                QMutexLocker lock(&self->m_mutex);
                bitmap = self->m_bitmaps.value(bitmapId, nullptr);
            }
            if (bitmap) {
                QMutexLocker bitmapLock(&bitmap->mutex);
                if (!bitmap->cpuMirrorCurrent) {
                    if (!bitmap->gpuCurrent) {
                        safe = false;
                    } else {
                        Canvas canvas;
                        canvas.bitmap = bitmap;
                        canvas.texture = bitmap->texture;
                        canvas.id = bitmap->framebufferId;
                        canvas.pixels = pixels;
                        canvas.width = width;
                        canvas.height = height;
                        canvas.stride = stride;
                        safe = self->readback(&canvas, PreviewGpuReadbackReason::Flush);
                        self->removeFramebuffer(&canvas);
                    }
                }
            }
        }
        if (!safe) {
            qWarning("GPU Canvas cannot fall back without materializing current bitmap pixels");
            cleanupTemporaryContext();
            return static_cast<void *>(nullptr);
        }
        void *cpuCanvas = cpuCanvasNew();
        if (!cpuCanvas) {
            cleanupTemporaryContext();
            return static_cast<void *>(nullptr);
        }
        Bitmap *bitmap = nullptr;
        bool incompatible = false;
        {
            QMutexLocker lock(&self->m_mutex);
            bitmap = bitmapId ? self->m_bitmaps.value(bitmapId, nullptr) : nullptr;
            if (!bitmap) {
                bitmap = new Bitmap(bitmapId != 0);
                bitmap->id = bitmapId;
                bitmap->framebufferId = self->m_nextCanvasId++;
                bitmap->pixels = pixels;
                bitmap->width = width;
                bitmap->height = height;
                bitmap->stride = stride;
                bitmap->gpuCurrent = false;
                if (bitmapId)
                    self->m_bitmaps.insert(bitmapId, bitmap);
            } else if (bitmap->pixels != pixels || bitmap->width != width ||
                       bitmap->height != height || bitmap->stride != stride) {
                incompatible = true;
            }
            if (!incompatible)
                ++bitmap->canvasRefs;
        }
        if (incompatible) {
            self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
            qWarning("GPU Canvas bitmap ID was reused for different pixel storage");
            cleanupTemporaryContext();
            return static_cast<void *>(nullptr);
        }
        Canvas *canvas = new Canvas;
        canvas->bitmap = bitmap;
        canvas->texture = bitmap->texture;
        canvas->id = bitmap->framebufferId;
        canvas->pixels = pixels;
        canvas->width = width;
        canvas->height = height;
        canvas->stride = stride;
        canvas->cpuCanvas = cpuCanvas;
        canvas->gpuBacked = false;
        canvas->states.append(Canvas::State());
        canvas->states.last().copyClip = QRegion(0, 0, width, height);
        {
            QMutexLocker lock(&self->m_mutex);
            self->m_canvases.insert(canvas, canvas);
        }
        cleanupTemporaryContext();
        return canvas;
    };

    void *cpuCanvas = cpuCanvasNew();
    const bool ready = self->ensureCurrent("canvas-new");
    if (!cpuCanvas || !ready) {
        if (!ready)
            warnCpuFallback("shared OpenGL context is unavailable");
        if (cpuCanvas && self->m_cpu->canvas_free)
            self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        return cpuCanvasFallback();
    }

    GLint maxTextureSize = 0;
    threadContext.context->functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (width > maxTextureSize || height > maxTextureSize) {
        warnCpuFallback("bitmap exceeds the GPU's maximum texture size");
        self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        return cpuCanvasFallback();
    }

    Bitmap *bitmap = nullptr;
    bool created = false;
    bool incompatible = false;
    {
        QMutexLocker lock(&self->m_mutex);
        bitmap = bitmapId ? self->m_bitmaps.value(bitmapId, nullptr) : nullptr;
        if (!bitmap) {
            bitmap = new Bitmap(bitmapId != 0);
            bitmap->id = bitmapId;
            bitmap->framebufferId = self->m_nextCanvasId++;
            bitmap->pixels = pixels;
            bitmap->width = width;
            bitmap->height = height;
            bitmap->stride = stride;
            if (bitmapId)
                self->m_bitmaps.insert(bitmapId, bitmap);
            created = true;
        } else if (bitmap->pixels != pixels || bitmap->width != width ||
                   bitmap->height != height || bitmap->stride != stride) {
            incompatible = true;
        }
        if (!incompatible)
            ++bitmap->canvasRefs;
    }
    if (incompatible) {
        self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        qWarning("GPU Canvas bitmap ID was reused for different pixel storage");
        return nullptr;
    }

    Canvas *canvas = new Canvas;
    canvas->bitmap = bitmap;
    canvas->texture = bitmap->texture;
    canvas->id = bitmap->framebufferId;
    canvas->pixels = pixels;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = stride;
    canvas->cpuCanvas = cpuCanvas;
    canvas->states.append(Canvas::State());
    canvas->states.last().copyClip = QRegion(0, 0, width, height);

    bool allocated = true;
    bool createdTexture = false;
    {
        QMutexLocker bitmapLock(&bitmap->mutex);
        QOpenGLFunctions *gl = threadContext.context->functions();
        clearGlErrors(gl);
        if (!bitmap->texture) {
            GLint previous = 0;
            gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
            gl->glGenTextures(1, &bitmap->texture);
            createdTexture = bitmap->texture != 0;
            gl->glBindTexture(GL_TEXTURE_2D, bitmap->texture);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
            bitmap->gpuCurrent = false;
        }
        canvas->texture = bitmap->texture;
        allocated = bitmap->texture && gl->glGetError() == GL_NO_ERROR;
        if (allocated && !bitmap->gpuCurrent)
            allocated = bitmap->cpuMirrorCurrent && self->upload(canvas);
        if (allocated)
            allocated = self->ensureFramebuffer(canvas, "canvas-new");
        if (!allocated && createdTexture) {
            gl->glDeleteTextures(1, &bitmap->texture);
            bitmap->texture = 0;
            bitmap->gpuCurrent = false;
            canvas->texture = 0;
        }
    }
    if (!allocated) {
        {
            QMutexLocker lock(&self->m_mutex);
            --bitmap->canvasRefs;
            if (created && bitmapId)
                self->m_bitmaps.remove(bitmapId);
        }
        if (created && bitmap->texture) {
            QOpenGLFunctions *gl = threadContext.context->functions();
            gl->glDeleteTextures(1, &bitmap->texture);
        }
        warnCpuFallback("GPU bitmap allocation or upload failed");
        self->m_cpu->canvas_free(self->m_cpu->user, cpuCanvas);
        delete canvas;
        if (created)
            delete bitmap;
        return cpuCanvasFallback();
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
    ScopedOffscreenCurrent scope(self);
    if (!self || !handle)
        return;
    Canvas *canvas = self->findCanvas(handle);
    if (!canvas) {
        if (self->m_cpu->canvas_free)
            self->m_cpu->canvas_free(self->m_cpu->user, handle);
        return;
    }
    bool idle = false;
    Bitmap *bitmap = canvas->bitmap;
    bool releaseBitmap = false;
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_canvases.remove(handle);
        idle = self->m_canvases.isEmpty();
        if (bitmap) {
            QMutexLocker bitmapLock(&bitmap->mutex);
            releaseBitmap = --bitmap->canvasRefs == 0 && !bitmap->persistent;
        }
    }
    if (canvas->gpuBacked)
        self->removeFramebuffer(canvas);
    if (releaseBitmap) {
        const bool synchronized = !bitmap->gpuFence ||
            (self->ensureCurrent("canvas-free") && self->waitForBitmap(bitmap, true));
        if (synchronized && bitmap->gpuFence && threadContext.deleteSync) {
            threadContext.deleteSync(bitmap->gpuFence);
            bitmap->gpuFence = nullptr;
        }
        const bool released = synchronized && (!bitmap->texture || !self->m_release ||
            self->m_release(self->m_presentUser, bitmap->texture));
        if (!released)
            qWarning("GPU Canvas texture could not be safely released; retaining it to avoid use-after-free");
        else if (bitmap->texture) {
            if (self->ensureCurrent("canvas-free"))
                threadContext.context->functions()->glDeleteTextures(1, &bitmap->texture);
            else
                qWarning("GPU Canvas could not make its context current to delete a bitmap texture");
        }
        delete bitmap;
    }
    if (self->m_cpu->canvas_free)
        self->m_cpu->canvas_free(self->m_cpu->user, canvas->cpuCanvas);
    delete canvas;
    if (idle && threadContext.owner == self)
        self->releaseThreadContext();
}

void MEGLER_CB PreviewGpuCanvasHost::canvasDiscard(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
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
    Bitmap *bitmap = canvas->bitmap;
    bool releaseBitmap = false;
    {
        QMutexLocker lock(&self->m_mutex);
        self->m_canvases.remove(handle);
        idle = self->m_canvases.isEmpty();
        if (bitmap) {
            QMutexLocker bitmapLock(&bitmap->mutex);
            releaseBitmap = --bitmap->canvasRefs == 0 && !bitmap->persistent;
        }
    }
    if (canvas->gpuBacked)
        self->removeFramebuffer(canvas);
    if (releaseBitmap) {
        const bool synchronized = !bitmap->gpuFence ||
            (self->ensureCurrent("canvas-discard") && self->waitForBitmap(bitmap, true));
        if (synchronized && bitmap->gpuFence && threadContext.deleteSync) {
            threadContext.deleteSync(bitmap->gpuFence);
            bitmap->gpuFence = nullptr;
        }
        const bool released = synchronized && (!bitmap->texture || !self->m_release ||
            self->m_release(self->m_presentUser, bitmap->texture));
        if (!released)
            qWarning("GPU Canvas texture could not be safely released; retaining it to avoid use-after-free");
        else if (bitmap->texture) {
            if (self->ensureCurrent("canvas-discard"))
                threadContext.context->functions()->glDeleteTextures(1, &bitmap->texture);
            else
                qWarning("GPU Canvas could not make its context current to delete a bitmap texture");
        }
        delete bitmap;
    }
    if (self->m_cpu->canvas_discard)
        self->m_cpu->canvas_discard(self->m_cpu->user, canvas->cpuCanvas);
    else if (self->m_cpu->canvas_free)
        self->m_cpu->canvas_free(self->m_cpu->user, canvas->cpuCanvas);
    delete canvas;
    if (idle && threadContext.owner == self)
        self->releaseThreadContext();
}

void MEGLER_CB PreviewGpuCanvasHost::flush(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->flush)
            self->m_cpu->flush(self->m_cpu->user, handle);
        return;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap)) {
            qWarning("GPU Canvas CPU fallback cannot draw with a stale bitmap mirror");
            return;
        }
        if (self->m_cpu->flush)
            self->m_cpu->flush(self->m_cpu->user, canvas->cpuCanvas);
        return;
    }
    if (!self->ensureBitmapCurrent(canvas))
        qWarning("GPU Canvas could not upload CPU changes before flushing the bitmap");
    if (self->ensureCurrent("flush")) {
        canvas->noteContext();
        threadContext.context->functions()->glFinish();
    } else {
        qWarning("GPU Canvas flush could not make its context current");
    }
    if (!canvas->bitmap->persistent && !self->readback(canvas, PreviewGpuReadbackReason::Flush))
        return;
    if (self->m_cpu->flush)
        self->m_cpu->flush(self->m_cpu->user, canvas->cpuCanvas);
}

void MEGLER_CB PreviewGpuCanvasHost::touched(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->touched)
            self->m_cpu->touched(self->m_cpu->user, handle);
        return;
    }
    self->markCpuWrite(canvas->bitmap);
    if (self->m_cpu->touched)
        self->m_cpu->touched(self->m_cpu->user, canvas->cpuCanvas);
    if (canvas->gpuBacked && !self->upload(canvas))
        qWarning("GPU Canvas could not upload externally changed bitmap pixels");
}

bool MEGLER_CB PreviewGpuCanvasHost::bitmapReadback(void *user, uint32_t id, void *pixels,
                                                   int width, int height, int stride)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    if (!self || !id || !pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 ||
        height > INT_MAX / (width * 4) || stride < width * 4)
        return false;
    Bitmap *bitmap = nullptr;
    {
        QMutexLocker lock(&self->m_mutex);
        bitmap = self->m_bitmaps.value(id, nullptr);
    }
    if (!bitmap)
        return true;
    if (bitmap->pixels != pixels || bitmap->width != width || bitmap->height != height ||
        bitmap->stride != stride)
        return false;
    QMutexLocker bitmapLock(&bitmap->mutex);
    if (bitmap->cpuMirrorCurrent)
        return true;
    if (!bitmap->gpuCurrent || !bitmap->texture)
        return false;
    return self->ensureCpuMirrorCurrent(bitmap);
}

void MEGLER_CB PreviewGpuCanvasHost::bitmapChanged(void *user, uint32_t id)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    if (!self || !id)
        return;
    Bitmap *bitmap = nullptr;
    {
        QMutexLocker lock(&self->m_mutex);
        bitmap = self->m_bitmaps.value(id, nullptr);
    }
    if (bitmap)
        self->markCpuWrite(bitmap);
}

void MEGLER_CB PreviewGpuCanvasHost::bitmapGone(void *user, uint32_t id)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    if (!self || !id)
        return;
    Bitmap *bitmap = nullptr;
    {
        QMutexLocker lock(&self->m_mutex);
        bitmap = self->m_bitmaps.value(id, nullptr);
        if (!bitmap)
            return;
        QMutexLocker bitmapLock(&bitmap->mutex);
        if (bitmap->canvasRefs != 0) {
            qWarning("GPU Canvas bitmap %u was released while %d canvases still referenced it",
                     id, bitmap->canvasRefs);
            return;
        }
        self->m_bitmaps.remove(id);
    }
    const bool hadContext = threadContext.owner == self && threadContext.context;
    const bool synchronized = !bitmap->gpuFence ||
        (self->ensureCurrent("bitmap-gone") && self->waitForBitmap(bitmap, true));
    if (synchronized && bitmap->gpuFence && threadContext.deleteSync) {
        threadContext.deleteSync(bitmap->gpuFence);
        bitmap->gpuFence = nullptr;
    }
    const bool released = synchronized && (!bitmap->texture || !self->m_release ||
        self->m_release(self->m_presentUser, bitmap->texture));
    if (!released) {
        qWarning("GPU Canvas bitmap texture could not be safely released; retaining it to avoid use-after-free");
    } else if (bitmap->texture) {
        if (self->ensureCurrent("bitmap-gone")) {
            Canvas framebufferCanvas;
            framebufferCanvas.bitmap = bitmap;
            framebufferCanvas.texture = bitmap->texture;
            framebufferCanvas.id = bitmap->framebufferId;
            self->removeFramebuffer(&framebufferCanvas);
            threadContext.context->functions()->glDeleteTextures(1, &bitmap->texture);
        } else {
            qWarning("GPU Canvas could not make its context current to delete a bitmap texture");
        }
    }
    if (!hadContext && threadContext.owner == self && threadContext.framebuffers.isEmpty())
        threadContext.clear();
    delete bitmap;
}

void MEGLER_CB PreviewGpuCanvasHost::save(void *user, void *handle)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
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
        self->clip(canvas, boundsPath, MEGLER_GFX_CLIP_INTERSECT, true);
    }
    if (self->m_cpu->save_layer)
        self->m_cpu->save_layer(self->m_cpu->user, canvas->cpuCanvas, bounds, alpha, blend);
}

void MEGLER_CB PreviewGpuCanvasHost::saveLayerBlur(void *user, void *handle, const float *bounds, int alpha,
                                                   float radiusX, float radiusY, bool clamp)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
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
        self->clip(canvas, boundsPath, MEGLER_GFX_CLIP_INTERSECT, true);
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
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->clip_rect)
            self->m_cpu->clip_rect(self->m_cpu->user, handle, left, top, right, bottom, op);
        return;
    }
    QPainterPath path;
    path.addRect(QRectF(left, top, right - left, bottom - top));
    self->clip(canvas, path, op, true);
    if (self->m_cpu->clip_rect)
        self->m_cpu->clip_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, op);
}

void MEGLER_CB PreviewGpuCanvasHost::clipPath(void *user, void *handle, const float *path, int count, int fill, int op)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->clip_path)
            self->m_cpu->clip_path(self->m_cpu->user, handle, path, count, fill, op);
        return;
    }
    const QPainterPath shape = pathFrom(path, count, fill);
    self->clip(canvas, shape, op, isRectangularPath(shape));
    if (self->m_cpu->clip_path)
        self->m_cpu->clip_path(self->m_cpu->user, canvas->cpuCanvas, path, count, fill, op);
}

void MEGLER_CB PreviewGpuCanvasHost::drawColor(void *user, void *handle, uint32_t color, int blend)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_color)
            self->m_cpu->draw_color(self->m_cpu->user, handle, color, blend);
        return;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return;
        if (self->m_cpu->draw_color)
            self->m_cpu->draw_color(self->m_cpu->user, canvas->cpuCanvas, color, blend);
        self->markCpuWrite(canvas->bitmap);
        return;
    }
    if (!self->ensureBitmapCurrent(canvas))
        return;
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
    self->finishGpuWrite(canvas);
}

void MEGLER_CB PreviewGpuCanvasHost::drawRect(void *user, void *handle, float left, float top, float right, float bottom,
                                             const megler_gfx_paint *paint, void *shader)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_rect)
            self->m_cpu->draw_rect(self->m_cpu->user, handle, left, top, right, bottom, paint, self->cpuShader(shader));
        return;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return;
        if (self->m_cpu->draw_rect)
            self->m_cpu->draw_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom,
                                   paint, self->cpuShader(shader));
        self->markCpuWrite(canvas->bitmap);
        return;
    }
    if (!self->ensureBitmapCurrent(canvas))
        return;
    if (paint && paint->filter_mode >= 0) {
        QPainterPath path;
        path.addRect(QRectF(left, top, right - left, bottom - top));
        if (self->drawFilteredPath(canvas, path, paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            self->m_cpu->draw_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, paint, self->cpuShader(shader));
            self->markCpuWrite(canvas->bitmap);
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
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_round_rect)
            self->m_cpu->draw_round_rect(self->m_cpu->user, handle, left, top, right, bottom, rx, ry, paint, self->cpuShader(shader));
        return;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return;
        if (self->m_cpu->draw_round_rect)
            self->m_cpu->draw_round_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom,
                                         rx, ry, paint, self->cpuShader(shader));
        self->markCpuWrite(canvas->bitmap);
        return;
    }
    if (!self->ensureBitmapCurrent(canvas))
        return;
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
            self->m_cpu->draw_round_rect(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, rx, ry, paint, self->cpuShader(shader));
            self->markCpuWrite(canvas->bitmap);
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
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_oval)
            self->m_cpu->draw_oval(self->m_cpu->user, handle, left, top, right, bottom, paint, self->cpuShader(shader));
        return;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return;
        if (self->m_cpu->draw_oval)
            self->m_cpu->draw_oval(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom,
                                   paint, self->cpuShader(shader));
        self->markCpuWrite(canvas->bitmap);
        return;
    }
    if (!self->ensureBitmapCurrent(canvas))
        return;
    if (paint && paint->filter_mode >= 0) {
        QPainterPath path;
        path.addEllipse(QRectF(left, top, right - left, bottom - top));
        if (self->drawFilteredPath(canvas, path, paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            self->m_cpu->draw_oval(self->m_cpu->user, canvas->cpuCanvas, left, top, right, bottom, paint, self->cpuShader(shader));
            self->markCpuWrite(canvas->bitmap);
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
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_path)
            self->m_cpu->draw_path(self->m_cpu->user, handle, path, count, fill, paint, self->cpuShader(shader));
        return;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return;
        if (self->m_cpu->draw_path)
            self->m_cpu->draw_path(self->m_cpu->user, canvas->cpuCanvas, path, count, fill,
                                   paint, self->cpuShader(shader));
        self->markCpuWrite(canvas->bitmap);
        return;
    }
    if (!self->ensureBitmapCurrent(canvas))
        return;
    if (paint && paint->filter_mode >= 0) {
        if (self->drawFilteredPath(canvas, pathFrom(path, count, fill), paint, shader))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            self->m_cpu->draw_path(self->m_cpu->user, canvas->cpuCanvas, path, count, fill, paint, self->cpuShader(shader));
            self->markCpuWrite(canvas->bitmap);
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
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas) {
        if (self && self->m_cpu->draw_bitmap)
            self->m_cpu->draw_bitmap(self->m_cpu->user, handle, image, src, dst, paint);
        return;
    }
    if (!image || !image->pixels || !src || !dst || !paint)
        return;
    if (!canvas->gpuBacked) {
        Bitmap *sourceBitmap = nullptr;
        if (image->id) {
            QMutexLocker lock(&self->m_mutex);
            sourceBitmap = self->m_bitmaps.value(image->id, nullptr);
        }
        Bitmap::ScopedLocks bitmapLocks(canvas->bitmap, sourceBitmap);
        if (sourceBitmap &&
            (sourceBitmap->pixels != image->pixels || sourceBitmap->width != image->width ||
             sourceBitmap->height != image->height || sourceBitmap->stride != image->stride)) {
            qWarning("GPU Canvas bitmap ID was reused for different source pixels");
            return;
        }
        if (sourceBitmap && !self->ensureCpuMirrorCurrent(sourceBitmap)) {
            qWarning("GPU Canvas could not materialize bitmap pixels for the CPU draw fallback");
            return;
        }
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return;
        if (self->m_cpu->draw_bitmap)
            self->m_cpu->draw_bitmap(self->m_cpu->user, canvas->cpuCanvas, image, src, dst, paint);
        self->markCpuWrite(canvas->bitmap);
        return;
    }
    Bitmap *sourceBitmap = nullptr;
    if (image->id && image->id != canvas->bitmap->id) {
        if (paint->filter_mode < 0) {
            void *sourceHandle = canvasNewBitmap(self, const_cast<void *>(image->pixels), image->width,
                                                 image->height, image->stride, image->id);
            Canvas *sourceCanvas = self->findCanvas(sourceHandle);
            if (sourceCanvas && self->copyCanvasPixels(canvas, sourceCanvas, src, dst, paint)) {
                canvasFree(self, sourceHandle);
                return;
            }
            if (sourceCanvas)
                canvasFree(self, sourceHandle);
        }
        {
            QMutexLocker lock(&self->m_mutex);
            sourceBitmap = self->m_bitmaps.value(image->id, nullptr);
        }
        if (sourceBitmap) {
            QMutexLocker bitmapLock(&sourceBitmap->mutex);
            if (sourceBitmap->pixels != image->pixels || sourceBitmap->width != image->width ||
                sourceBitmap->height != image->height || sourceBitmap->stride != image->stride) {
                qWarning("GPU Canvas bitmap ID was reused for different source pixels");
                return;
            }
            if (!self->ensureCpuMirrorCurrent(sourceBitmap)) {
                qWarning("GPU Canvas could not materialize bitmap pixels for the CPU draw fallback");
                return;
            }
        }
    }
    Bitmap::ScopedLocks bitmapLocks(canvas->bitmap, sourceBitmap);
    if (!self->ensureBitmapCurrent(canvas))
        return;
    if (paint->filter_mode >= 0) {
        if (self->drawFilteredBitmap(canvas, image, src, dst, paint))
            return;
        if (self->readback(canvas, PreviewGpuReadbackReason::Filter)) {
            self->m_cpu->draw_bitmap(self->m_cpu->user, canvas->cpuCanvas, image, src, dst, paint);
            self->markCpuWrite(canvas->bitmap);
            self->upload(canvas);
        }
        return;
    }
    if (image->pixels == canvas->pixels) {
        if (image->width == canvas->width && image->height == canvas->height &&
            image->stride == canvas->stride && self->copyCanvasPixels(canvas, canvas, src, dst, paint))
            return;
        if (!self->readback(canvas, PreviewGpuReadbackReason::Self))
            return;
    }
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
    self->finishGpuWrite(canvas);
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
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
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
    ScopedOffscreenCurrent scope(self);
    if (self && self->m_cpu->shader_matrix && shader->cpuShader)
        self->m_cpu->shader_matrix(self->m_cpu->user, shader->cpuShader, matrix);
}

void MEGLER_CB PreviewGpuCanvasHost::shaderFree(void *user, void *handle)
{
    Shader *shader = static_cast<Shader *>(handle);
    if (!shader)
        return;
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    if (self && self->m_cpu->shader_free && shader->cpuShader)
        self->m_cpu->shader_free(self->m_cpu->user, shader->cpuShader);
    delete shader;
}

bool MEGLER_CB PreviewGpuCanvasHost::present(void *user, void *handle, int, int, int, int)
{
    PreviewGpuCanvasHost *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    if (!self)
        return false;
    const PreviewGpuMetrics::Sample sample = self->m_metrics->beginPresent();
    QElapsedTimer timer;
    if (sample.sampled)
        timer.start();
    Canvas *canvas = self->findCanvas(handle);
    if (!canvas) {
        if (sample.enabled)
            self->m_metrics->finishPresent(false, sample, sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0, 0);
        return false;
    }
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!self->ensureCpuMirrorCurrent(canvas->bitmap))
            return false;
        const quint64 producerUs = sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0;
        const bool result = self->m_presentPixels &&
            self->m_presentPixels(self->m_presentUser, canvas->pixels, canvas->width,
                                  canvas->height, canvas->stride);
        if (sample.enabled)
            self->m_metrics->finishPresent(result, sample, producerUs, 0, true,
                sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0);
        return result;
    }
    if ((!self->m_present && !self->m_presentPixels) ||
        !self->ensureCurrent("present") || !self->ensureBitmapCurrent(canvas)) {
        if (sample.enabled)
            self->m_metrics->finishPresent(false, sample, sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0, 0);
        return false;
    }
    if (self->m_present && self->m_presentPixels) {
        QOpenGLFunctions *gl = threadContext.context->functions();
        clearGlErrors(gl);
        gl->glFinish();
        if (gl->glGetError() == GL_NO_ERROR &&
            self->m_present(self->m_presentUser, canvas->texture, canvas->width, canvas->height)) {
            if (sample.enabled)
                self->m_metrics->finishPresent(true, sample,
                    sample.sampled ? static_cast<quint64>(timer.nsecsElapsed() / 1000) : 0, 0);
            return true;
        }
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

int MEGLER_CB PreviewGpuCanvasHost::textureEglFrame(void *user, int id, void *display,
                                                    void *surface, int width, int height)
{
    auto *self = static_cast<PreviewGpuCanvasHost *>(user);
    if (!self || id <= 0 || !self->m_nativeEglTextures)
        return 0;
    if (!display && !surface) {
        QMutexLocker lock(&self->m_mutex);
        self->m_eglTextures.remove(id);
        return 1;
    }
    if (!display || !surface || width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return 0;
    auto texture = std::make_shared<EglTexture>();
    char failure[256] = {};
    const AuroraEglFrameResult result = aurora_egl_frame_capture(
        static_cast<EGLDisplay>(display), static_cast<EGLSurface>(surface),
        width, height, 0, &texture->frame, failure, sizeof(failure));
    if (result != AURORA_EGL_FRAME_READY) {
        qWarning("GPU Canvas could not capture a native EGL texture: %s", failure);
        return result == AURORA_EGL_FRAME_CONTEXT_LOST ? -1 : 0;
    }
    QMutexLocker lock(&self->m_mutex);
    self->m_eglTextures.insert(id, std::move(texture));
    return 1;
}

void MEGLER_CB PreviewGpuCanvasHost::drawTexture(void *user, void *handle, int id,
                                                  float left, float top, float right, float bottom, int alpha)
{
    auto *self = static_cast<PreviewGpuCanvasHost *>(user);
    ScopedOffscreenCurrent scope(self);
    Canvas *canvas = self ? self->findCanvas(handle) : nullptr;
    if (!canvas || right <= left || bottom <= top || alpha <= 0)
        return;
    std::shared_ptr<EglTexture> image;
    {
        QMutexLocker lock(&self->m_mutex);
        image = self->m_eglTextures.value(id);
    }
    if (!image)
        return;
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!self->ensureCurrent("draw-egl-texture") ||
        (canvas->gpuBacked && !self->ensureBitmapCurrent(canvas))) {
        qWarning("GPU Canvas could not make the EGL texture consumer current");
        return;
    }
    QOpenGLFunctions *gl = threadContext.context->functions();
    const GLuint texture = aurora_egl_frame_import(&image->frame);
    if (!texture) {
        qWarning("GPU Canvas could not import a native EGL texture");
        return;
    }
    Canvas source;
    {
        QMutexLocker lock(&self->m_mutex);
        source.id = self->m_nextCanvasId++;
    }
    source.texture = texture;
    source.width = image->frame.width;
    source.height = image->frame.height;
    source.states.append(Canvas::State());
    const bool framebufferReady = self->ensureFramebuffer(&source, "egl-texture-source");
    const Canvas::State &state = canvas->states.last();
    const auto integral = [](qreal n) {
        return n >= -16777216 && n <= 16777216 && n == std::floor(n);
    };
    const QRectF bounds(left + state.matrix.dx(), top + state.matrix.dy(), right - left, bottom - top);
    megler_gfx_paint paint = {};
    paint.color = qRgba(255, 255, 255, qBound(0, alpha, 255));
    paint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    paint.filter_mode = MEGLER_GFX_BLEND_DST;
    paint.flags = MEGLER_GFX_FILTER_BITMAP;
    bool drawn = false;
    if (framebufferReady && canvas->gpuBacked && state.copyClipValid &&
        state.matrix.type() <= QTransform::TxTranslate &&
        integral(bounds.left()) && integral(bounds.top()) &&
        integral(bounds.right()) && integral(bounds.bottom())) {
        drawn = self->drawFiltered(canvas, &paint, bounds, {}, &source,
                                   QRect(0, 0, source.width, source.height));
    }
    if (!drawn && framebufferReady) {
        const qint64 bytes = qint64(source.width) * source.height * 4;
        if (bytes <= INT_MAX) {
            QByteArray rgba(int(bytes), '\0');
            QByteArray bgra(int(bytes), '\0');
            GLint previous = 0, pack = 4;
            GLint packBuffer = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
            const bool gles3 = threadContext.context->isOpenGLES() &&
                               threadContext.context->format().majorVersion() >= 3;
            gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
            gl->glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
            if (gles3) {
                gl->glGetIntegerv(0x88ED, &packBuffer); // GL_PIXEL_PACK_BUFFER_BINDING
                gl->glGetIntegerv(0x0D02, &rowLength); // GL_PACK_ROW_LENGTH
                gl->glGetIntegerv(0x0D04, &skipPixels); // GL_PACK_SKIP_PIXELS
                gl->glGetIntegerv(0x0D03, &skipRows); // GL_PACK_SKIP_ROWS
                gl->glBindBuffer(0x88EB, 0); // GL_PIXEL_PACK_BUFFER
                gl->glPixelStorei(0x0D02, 0);
                gl->glPixelStorei(0x0D04, 0);
                gl->glPixelStorei(0x0D03, 0);
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, threadContext.framebuffers.value(source.id).id);
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            clearGlErrors(gl);
            gl->glReadPixels(0, 0, source.width, source.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            const GLenum error = gl->glGetError();
            gl->glPixelStorei(GL_PACK_ALIGNMENT, pack);
            if (gles3) {
                gl->glPixelStorei(0x0D02, rowLength);
                gl->glPixelStorei(0x0D04, skipPixels);
                gl->glPixelStorei(0x0D03, skipRows);
                gl->glBindBuffer(0x88EB, GLuint(packBuffer));
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous));
            if (error == GL_NO_ERROR &&
                (canvas->gpuBacked ? self->readback(canvas, PreviewGpuReadbackReason::Self)
                                   : self->ensureCpuMirrorCurrent(canvas->bitmap))) {
                copyReadbackRgbaToBgra(reinterpret_cast<const uint8_t *>(rgba.constData()),
                                       reinterpret_cast<uint8_t *>(bgra.data()),
                                       source.width, source.height, source.width * 4);
                const megler_gfx_image pixels = { bgra.constData(), source.width, source.height,
                                                  source.width * 4, 0, 0 };
                const float src[] = { 0, 0, float(source.width), float(source.height) };
                const float dst[] = { left, top, right, bottom };
                paint.filter_mode = -1;
                self->m_cpu->draw_bitmap(self->m_cpu->user, canvas->cpuCanvas, &pixels, src, dst, &paint);
                self->markCpuWrite(canvas->bitmap);
                drawn = !canvas->gpuBacked || self->upload(canvas);
            }
        }
    }
    self->removeFramebuffer(&source);
    gl->glDeleteTextures(1, &texture);
    if (!drawn)
        qWarning("GPU Canvas could not draw a native EGL texture");
}

bool PreviewGpuCanvasHost::drawTextPath(void *handle, const float *path, int count, int fill,
                                        const megler_gfx_paint *paint, void *shader)
{
    ScopedOffscreenCurrent scope(this);
    Canvas *canvas = findCanvas(handle);
    if (!canvas)
        return false;
    Bitmap::ScopedLocks bitmapLock(canvas->bitmap, nullptr);
    if (!canvas->gpuBacked) {
        if (!ensureCpuMirrorCurrent(canvas->bitmap))
            return true;
        if (m_cpu->draw_path)
            m_cpu->draw_path(m_cpu->user, canvas->cpuCanvas, path, count, fill, paint, cpuShader(shader));
        markCpuWrite(canvas->bitmap);
        return true;
    }
    if (!ensureBitmapCurrent(canvas))
        return true;
    if (paint && paint->filter_mode >= 0) {
        if (drawFilteredPath(canvas, pathFrom(path, count, fill), paint, shader))
            return true;
        if (readback(canvas, PreviewGpuReadbackReason::Filter)) {
            m_cpu->draw_path(m_cpu->user, canvas->cpuCanvas, path, count, fill, paint, cpuShader(shader));
            markCpuWrite(canvas->bitmap);
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
    ScopedOffscreenCurrent scope(this);
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
