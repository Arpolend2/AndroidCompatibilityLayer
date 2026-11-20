int runApplication(int argc, char **argv, void (*initReportWebView)());

#ifdef ACL_WITH_REPORT_WEBVIEW
void initReportWebView();
#endif

int main(int argc, char **argv)
{
#ifdef ACL_WITH_REPORT_WEBVIEW
    return runApplication(argc, argv, initReportWebView);
#else
    return runApplication(argc, argv, nullptr);
#endif
}
