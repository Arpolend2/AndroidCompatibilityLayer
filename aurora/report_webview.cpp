#include <QCoreApplication>
#include <webenginecontext.h>

void initReportWebView(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    // CEF child processes must enter the engine before any Qt application or worker logic.
    Aurora::WebView::WebEngineContext::InitBrowser(
        argc, argv, {"--default-encoding=UTF-8"});
}
