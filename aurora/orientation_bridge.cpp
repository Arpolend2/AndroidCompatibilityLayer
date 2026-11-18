#include "orientation_bridge.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QtMath>

#include <climits>
#include <cmath>

OrientationBridge *OrientationBridge::instance()
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    static OrientationBridge *bridge = new OrientationBridge;
    return bridge;
}

OrientationBridge::OrientationBridge()
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(150);
    connect(&m_timer, &QTimer::timeout, this, &OrientationBridge::settle);
}

megler_display_host OrientationBridge::host()
{
    megler_display_host result = {};
    result.user = this;
    result.orientation = &orientationThunk;
    return result;
}

void OrientationBridge::start()
{
    Q_ASSERT(QThread::currentThread() == thread());
    {
        QMutexLocker lock(&m_mutex);
        ++m_generation;
        m_active = true;
    }
    m_timer.stop();
    m_surface = Surface();
    m_requested = MEGLER_ORIENTATION_UNSPECIFIED;
    m_supported = true;
    if (m_policy != PlatformDefault) {
        m_policy = PlatformDefault;
        emit policyChanged();
    }
    emit requestChanged();
    scheduleSurface();
}

void OrientationBridge::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    {
        QMutexLocker lock(&m_mutex);
        m_active = false;
        ++m_generation;
    }
    m_timer.stop();
    m_surface = Surface();
}

void MEGLER_CB OrientationBridge::orientationThunk(void *user, int requested)
{
    OrientationBridge *self = static_cast<OrientationBridge *>(user);
    QMutexLocker lock(&self->m_mutex);
    if (!self->m_active) return;
    if (!QMetaObject::invokeMethod(self, "deliver", Qt::QueuedConnection,
                                   Q_ARG(qulonglong, self->m_generation), Q_ARG(int, requested)))
        qWarning() << "Unable to queue Android orientation request" << requested;
}

void OrientationBridge::deliver(qulonglong generation, int requested)
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || generation != m_generation) return;
    }
    int nextPolicy = m_policy;
    bool supported = true;
    switch (requested) {
    case MEGLER_ORIENTATION_UNSPECIFIED: nextPolicy = PlatformDefault; break;
    case MEGLER_ORIENTATION_PORTRAIT: nextPolicy = FixedPortrait; break;
    case MEGLER_ORIENTATION_LANDSCAPE: nextPolicy = FixedLandscape; break;
    case MEGLER_ORIENTATION_REVERSE_PORTRAIT: nextPolicy = FixedPortraitInverted; break;
    case MEGLER_ORIENTATION_REVERSE_LANDSCAPE: nextPolicy = FixedLandscapeInverted; break;
    case MEGLER_ORIENTATION_NOSENSOR:
    case MEGLER_ORIENTATION_LOCKED: {
        const int direction = requested == MEGLER_ORIENTATION_NOSENSOR ? m_natural : m_actual;
        switch (direction) {
        case Portrait: nextPolicy = FixedPortrait; break;
        case Landscape: nextPolicy = FixedLandscape; break;
        case PortraitInverted: nextPolicy = FixedPortraitInverted; break;
        case LandscapeInverted: nextPolicy = FixedLandscapeInverted; break;
        default: supported = false; break;
        }
        break;
    }
    default: supported = false; break;
    }
    m_requested = requested;
    m_supported = supported;
    if (!supported)
        qWarning() << "Unsupported Android orientation request:" << requested;
    if (supported && nextPolicy != m_policy) {
        m_policy = nextPolicy;
        emit policyChanged();
    }
    emit requestChanged();
}

int OrientationBridge::rotationFor(int natural, int actual)
{
    // The sequence is Android's anticlockwise rotation for a portrait-native phone.
    const int sequence[] = { Portrait, Landscape, PortraitInverted, LandscapeInverted };
    int n = -1, a = -1;
    for (int i = 0; i != 4; ++i) {
        if (sequence[i] == natural) n = i;
        if (sequence[i] == actual) a = i;
    }
    return n < 0 || a < 0 ? -1 : (a - n + 4) % 4;
}

void OrientationBridge::setOrientations(int natural, int actual)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_natural == natural && m_actual == actual) return;
    m_natural = natural;
    m_actual = actual;
    scheduleSurface();
}

void OrientationBridge::setViewport(qreal width, qreal height, qreal pixelRatio, int density)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_width == width && m_height == height && m_ratio == pixelRatio && m_density == density) return;
    m_width = width;
    m_height = height;
    m_ratio = pixelRatio;
    m_density = density;
    scheduleSurface();
}

void OrientationBridge::scheduleSurface()
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active) return;
    }
    m_timer.start();
    emit surfacePending();
}

void OrientationBridge::settle()
{
    const int rotation = rotationFor(m_natural, m_actual);
    const double width = double(m_width) * double(m_ratio);
    const double height = double(m_height) * double(m_ratio);
    if (rotation < 0 || !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(double(m_ratio)) || m_width <= 0 || m_height <= 0 || m_ratio <= 0 ||
        width < 0.5 || height < 0.5 || width > INT_MAX - 0.5 || height > INT_MAX - 0.5 || m_density <= 0) {
        m_surface = Surface();
        emit surfaceUnavailable();
        return;
    }
    const int w = qRound(width), h = qRound(height);
    m_surface.width = w;
    m_surface.height = h;
    m_surface.density = m_density;
    m_surface.rotation = rotation;
    emit surfaceChanged(w, h, m_density, rotation);
}
