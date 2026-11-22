#include "shared_microg_worker.h"
#include "microg_watchdog.h"

#include <cstring>

int runApplication(int argc, char **argv, void (*initReportWebView)());

#ifdef ACL_WITH_REPORT_WEBVIEW
void initReportWebView();
#endif

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--shared-microg-worker") == 0)
        return shared_microg_worker_main(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--microg-watchdog") == 0)
        return runMicrogWatchdog(argc, argv);
#ifdef ACL_WITH_REPORT_WEBVIEW
    return runApplication(argc, argv, initReportWebView);
#else
    return runApplication(argc, argv, nullptr);
#endif
}
