#ifndef MEGLER_AURORA_ORIENTATION_BRIDGE_H
#define MEGLER_AURORA_ORIENTATION_BRIDGE_H

#include <QObject>
#include <QMutex>
#include <QTimer>

#include "../core/include/megler.h"

class OrientationBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int policy READ policy NOTIFY policyChanged)
    Q_PROPERTY(int requested READ requested NOTIFY requestChanged)
    Q_PROPERTY(bool requestSupported READ requestSupported NOTIFY requestChanged)

public:
    enum Direction { Unknown = 0, Portrait = 1, Landscape = 2,
                     PortraitInverted = 3, LandscapeInverted = 4 };
    Q_ENUM(Direction)
    enum Policy { PlatformDefault = 0, FixedPortrait, FixedLandscape,
                  FixedPortraitInverted, FixedLandscapeInverted };
    Q_ENUM(Policy)

    struct Surface { int width = 0; int height = 0; int density = 0; int rotation = -1; };

    // The callback user remains alive for the process. Use only from the GUI thread.
    static OrientationBridge *instance();
    megler_display_host host();
    void start();
    void stop();

    int policy() const { return m_policy; }
    int requested() const { return m_requested; }
    bool requestSupported() const { return m_supported; }
    Surface surface() const { return m_surface; }
    bool hasSurface() const { return !m_timer.isActive() && m_surface.rotation >= 0; }

    // Directions describe the actual Page.orientation and screen nativeOrientation,
    // not the sensor or the Android request. Unknown invalidates the snapshot.
    Q_INVOKABLE void setOrientations(int natural, int actual);
    // Supply viewport dimensions in Qt logical units, device pixel ratio and the
    // actual Android-compatible density for this output, not physical monitor DPI.
    Q_INVOKABLE void setViewport(qreal width, qreal height, qreal pixelRatio, int density);

signals:
    void policyChanged();
    void requestChanged();
    void surfacePending();
    // Emitted after every successful settle, including a return to the same size.
    void surfaceChanged(int width, int height, int density, int rotation);
    void surfaceUnavailable();

private slots:
    void deliver(qulonglong generation, int requested);
    void settle();

private:
    explicit OrientationBridge();
    static void MEGLER_CB orientationThunk(void *user, int requested);
    void scheduleSurface();
    static int rotationFor(int natural, int actual);

    QMutex m_mutex;
    qulonglong m_generation = 0;
    bool m_active = false;
    QTimer m_timer;
    int m_policy = PlatformDefault;
    int m_requested = MEGLER_ORIENTATION_UNSPECIFIED;
    bool m_supported = true;
    int m_natural = Unknown;
    int m_actual = Unknown;
    qreal m_width = 0;
    qreal m_height = 0;
    qreal m_ratio = 0;
    int m_density = 0;
    Surface m_surface;
};

#endif
