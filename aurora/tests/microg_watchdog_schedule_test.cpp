#include "../microg_watchdog_schedule.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <vector>

namespace {
struct Call {
    bool enabled;
    MicrogWatchdogSchedule::Completion complete;
};

bool check(bool value, const char *message)
{
    if (!value) std::fprintf(stderr, "%s\n", message);
    return value;
}

void wait(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::vector<Call> calls;
    {
        MicrogWatchdogSchedule schedule(
            [&calls](bool enabled, const MicrogWatchdogSchedule::Completion &complete) {
                calls.push_back(Call{enabled, complete});
            }, 10);
        schedule.setEnabled(true);
        if (!check(calls.size() == 1 && calls[0].enabled, "initial enable")) return 1;
        schedule.setEnabled(false);
        if (!check(calls.size() == 1, "disable waits for enable")) return 1;
        auto complete = calls[0].complete;
        complete(true);
        if (!check(calls.size() == 2 && !calls[1].enabled, "disable follows enable")) return 1;
        complete = calls[1].complete;
        complete(false);
        if (!check(calls.size() == 2, "failed disable waits before retry")) return 1;
        wait(30);
        if (!check(calls.size() == 3 && !calls[2].enabled, "failed disable retried")) return 1;
        schedule.setEnabled(true);
        schedule.setEnabled(false);
        schedule.setEnabled(true);
        if (!check(calls.size() == 3, "rapid toggles wait for pending operation")) return 1;
        complete = calls[2].complete;
        complete(true);
        if (!check(calls.size() == 4 && calls[3].enabled, "latest setting applied")) return 1;
        complete = calls[3].complete;
        complete(false);
        wait(30);
        if (!check(calls.size() == 5 && calls[4].enabled, "failed enable retried")) return 1;
        complete = calls[4].complete;
        complete(true);
        schedule.setEnabled(true);
        if (!check(calls.size() == 5, "confirmed setting not repeated")) return 1;
        schedule.setEnabled(false);
        if (!check(calls.size() == 6 && !calls[5].enabled, "new disable issued")) return 1;
        schedule.setEnabled(true);
        complete = calls[5].complete;
        complete(false);
        if (!check(calls.size() == 7 && calls[6].enabled, "stale failure applies latest setting")) return 1;
    }
    calls.back().complete(true);
    calls.clear();
    {
        MicrogWatchdogSchedule schedule(
            [&calls](bool enabled, const MicrogWatchdogSchedule::Completion &complete) {
                calls.push_back(Call{enabled, complete});
            }, 10);
        schedule.setEnabled(true);
        auto complete = calls[0].complete;
        complete(false);
        schedule.setEnabled(true);
        if (!check(calls.size() == 1, "same setting does not bypass retry delay")) return 1;
        schedule.setEnabled(false);
        if (!check(calls.size() == 2 && !calls[1].enabled, "new setting cancels retry delay")) return 1;
        complete = calls[1].complete;
        complete(true);
        wait(30);
        if (!check(calls.size() == 2, "obsolete retry cancelled")) return 1;
    }
    int synchronousCalls = 0;
    MicrogWatchdogSchedule synchronous(
        [&synchronousCalls](bool, const MicrogWatchdogSchedule::Completion &complete) {
            ++synchronousCalls;
            complete(true);
        }, 10);
    synchronous.setEnabled(true);
    synchronous.setEnabled(true);
    synchronous.setEnabled(false);
    if (!check(synchronousCalls == 2, "synchronous completion")) return 1;
    return 0;
}
