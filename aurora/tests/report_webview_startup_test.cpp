#include <QCoreApplication>
#include <webenginecontext.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>

#define main applicationEntry
#include "../application_entry.cpp"
#undef main

namespace {
int engineCalls, applicationCalls, workerCalls;
int graphicsSentinel;
int expectedArgc;
char **expectedArgv;
bool expectedAvailable;
struct CefSubprocessExit {};
}

namespace Aurora { namespace WebView { namespace WebEngineContext {
void InitBrowser(int argc, char **argv, const std::vector<std::string> &args,
                 QMap<QString, QString> params, IEglContextDelegatePtr)
{
    assert(!QCoreApplication::instance());
    assert(QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts));
    assert(argc == expectedArgc && argv == expectedArgv);
    assert(args == std::vector<std::string>{"--default-encoding=UTF-8"});
    assert(params.isEmpty());
    ++engineCalls;
    if (argc > 1 && std::strcmp(argv[1], "--type=renderer") == 0)
        throw CefSubprocessExit();
}
}}}

megler_gfx_host previewGraphics()
{
    megler_gfx_host host{};
    host.user = &graphicsSentinel;
    return host;
}
int shared_microg_worker_main(int, char **, const megler_gfx_host *graphics)
{
    assert(qobject_cast<QGuiApplication *>(QCoreApplication::instance()));
    assert(QGuiApplication::platformName() == QLatin1String("minimal"));
    assert(QGuiApplication::allWindows().isEmpty());
    assert(graphics && graphics->user == &graphicsSentinel);
    ++workerCalls;
    return 0;
}
int runMicrogWatchdog(int, char **) { ++workerCalls; return 0; }
int runApplication(int argc, char **argv, bool available)
{
    assert(argc == expectedArgc && argv == expectedArgv);
    assert(available == expectedAvailable);
    assert(engineCalls == (available ? 1 : 0));
    ++applicationCalls;
    return 0;
}

int main()
{
    for (const char *mode : {"", "--microg-service", "--type=renderer", "--preview",
                            "--standalone", "--build-standalone", "--unpack-bundle",
                            "--unpack-store-bundle", "--unpack-store-xapk",
                            "--shared-microg-worker", "--microg-watchdog"}) {
        char program[] = "manager";
        char argument[64];
        std::strcpy(argument, mode);
        char *argv[] = {program, argument, nullptr};
        expectedArgc = *mode ? 2 : 1;
        expectedArgv = argv;
        expectedAvailable = !*mode || std::strcmp(mode, "--microg-service") == 0;
        engineCalls = applicationCalls = workerCalls = 0;
        try {
            assert(applicationEntry(expectedArgc, argv) == 0);
            if (std::strcmp(mode, "--shared-microg-worker") == 0 ||
                std::strcmp(mode, "--microg-watchdog") == 0)
                assert(workerCalls == 1 && engineCalls == 0 && applicationCalls == 0);
            else
                assert(applicationCalls == 1);
        } catch (const CefSubprocessExit &) {
            assert(std::strcmp(mode, "--type=renderer") == 0);
            assert(engineCalls == 1 && applicationCalls == 0 && workerCalls == 0);
        }
    }
    std::puts("Report WebView startup ordering and worker isolation OK");
}
