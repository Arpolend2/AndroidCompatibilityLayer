#include <QByteArray>
#include <QCoreApplication>
#include <QStringList>
#include <webenginecontext.h>

#include <vector>

void initReportWebView()
{
    static const bool initialized = [] {
        std::vector<QByteArray> arguments;
        std::vector<char *> argv;
        for (const QString &argument : QCoreApplication::arguments())
            arguments.push_back(argument.toLocal8Bit());
        for (QByteArray &argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        Aurora::WebView::WebEngineContext::InitBrowser(
            static_cast<int>(arguments.size()), argv.data(), {"--default-encoding=UTF-8"});
        return true;
    }();
    Q_UNUSED(initialized);
}
