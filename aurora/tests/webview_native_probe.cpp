#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QSharedPointer>
#include <QTimer>

#include <cstdio>

#include <preferencemanager.h>
#include <webenginecontext.h>
#include <webviewprivate.h>
#include <extensions/irendering.h>

namespace AW = Aurora::WebView;

class ShutdownObserver : public QObject
{
public:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == AW::WebEngineContext::ReadyToClose::readyToCloseEventType) {
            QCoreApplication::exit(result);
            return true;
        }
        return false;
    }

    int result = 1;
};

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    const bool frameProbe = app.arguments().contains(QStringLiteral("--frame"));
    app.setApplicationName(QStringLiteral("webview-native-probe"));
    app.setOrganizationName(QStringLiteral("androidcompatibilitylayer-test"));
    AW::WebEngineContext::InitBrowser(argc, argv);

    ShutdownObserver observer;
    app.installEventFilter(&observer);
    AW::WebViewPrivate *view = nullptr;
    bool stopping = false;
    auto finish = [&](bool passed, const char *message) {
        if (stopping) return;
        stopping = true;
        std::fprintf(passed ? stdout : stderr, "%s\n", message);
        observer.result = passed ? 0 : 1;
        QTimer::singleShot(0, &app, [&]() {
            delete view;
            view = nullptr;
            AW::WebEngineContext::Shutdown();
            QTimer::singleShot(10000, &app, [&]() {
                std::fprintf(stderr, "WebView shutdown timed out\n");
                QCoreApplication::exit(1);
            });
        });
    };

    auto preferences = AW::PreferenceManager::GetInstance();
    bool started = false;
    bool loaded = false;
    bool frameReceived = false;
    bool frameAccelerated = false;
    auto verifyFrame = [&]() {
        if (!frameProbe || !loaded || stopping || !view || frameReceived) return;
        AW::IRendering *rendering = view->renderingExtension();
        if (!rendering) {
            finish(false, "WebView rendering extension unavailable");
            return;
        }
        frameAccelerated = rendering->isAccelerated();
        const auto image = rendering->viewTextureSW();
        if (!image || image->isNull()) return;
        const QImage snapshot = image->copy();
        if (snapshot.width() != 120 || snapshot.height() != 80) return;
        const QColor center = snapshot.pixelColor(snapshot.width() / 2, snapshot.height() / 2);
        if (center.red() < 220 || center.green() > 35 || center.blue() > 35) return;
        frameReceived = true;
        finish(true, "Offline HTML and WebView software frame verified");
    };
    auto start = [&]() {
        if (started || stopping) return;
        started = true;
        preferences->setJavascriptEnabled(false);
        if (preferences->javascriptEnabled()) {
            finish(false, "Could not disable JavaScript");
            return;
        }
        view = new AW::WebViewPrivate(&app);
        if (frameProbe) {
            AW::IRendering *rendering = view->renderingExtension();
            if (!rendering) {
                finish(false, "WebView rendering extension unavailable");
                return;
            }
            QObject::connect(rendering, &AW::IRendering::viewTextureReady, &app, verifyFrame);
            view->setDPR(1.0);
            view->updateSize(QRectF(0, 0, 120, 80));
            view->setRenderRect(QRect(0, 0, 120, 80));
            view->startRender();
        }
        QObject::connect(view, &AW::WebView::loadError, &app, [&](const QString &, int code) {
            std::fprintf(stderr, "WebView load failed: %d\n", code);
            finish(false, "Offline HTML probe failed");
        });
        QObject::connect(view, &AW::WebView::loadFinished, &app, [&](int) {
            QTimer::singleShot(1000, &app, [&]() {
                if (!view || view->title() != QStringLiteral("webview-probe-ok")) {
                    finish(false, "Unexpected page title or JavaScript executed");
                    return;
                }
                if (frameProbe) {
                    loaded = true;
                    verifyFrame();
                    return;
                }
                finish(true, "Offline HTML loaded with JavaScript disabled");
            });
        });
        const QString content = frameProbe
            ? QStringLiteral("<style>html,body{margin:0;width:100%;height:100%;background:#ff0000}</style>")
            : QStringLiteral("<p>WebView probe</p>");
        view->loadHtml(QStringLiteral(
            "<!doctype html><meta http-equiv=\"Content-Security-Policy\" "
            "content=\"default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'\">"
            "<title>webview-probe-ok</title>") + content +
            QStringLiteral("<script>document.title='javascript-executed'</script>"));
    };
    if (preferences->isReady()) QTimer::singleShot(0, &app, start);
    else QObject::connect(preferences.data(), &AW::PreferenceManager::ready, &app, start);
    QTimer::singleShot(30000, &app, [&]() {
        finish(false, frameProbe && loaded
            ? (frameAccelerated ? "No verified software frame; accelerated rendering is active"
                                : "No verified software frame before timeout")
            : "WebView probe timed out");
    });
    return app.exec();
}
