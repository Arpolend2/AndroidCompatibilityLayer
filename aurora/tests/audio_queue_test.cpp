#include "../audio_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main()
{
    using namespace std::chrono;
    AudioQueue queue(4000);
    queue.setAvailable(true);
    const char first[] = {1, 2, 3, 4};
    CHECK(queue.write(first, sizeof(first)));
    char out[8];
    std::memset(out, 9, sizeof(out));
    CHECK(queue.read(out, sizeof(out)) == sizeof(first));
    CHECK(std::memcmp(out, first, sizeof(first)) == 0);
    CHECK(out[4] == 0 && out[7] == 0);

    std::vector<char> full(2400, 42);
    CHECK(queue.write(full.data(), full.size()));
    std::atomic<bool> done(false);
    const char next[] = {5, 6, 7, 8};
    std::thread writer([&] { queue.write(next, sizeof(next)); done.store(true); });
    std::this_thread::sleep_for(milliseconds(25));
    const bool blocked = !done.load();
    char consumed[4];
    queue.read(consumed, sizeof(consumed));
    writer.join();
    CHECK(blocked && done.load());
    CHECK(std::memcmp(consumed, full.data(), sizeof(consumed)) == 0);
    std::vector<char> rest(2400);
    CHECK(queue.read(rest.data(), rest.size()) == rest.size());
    CHECK(std::memcmp(rest.data() + 2396, next, sizeof(next)) == 0);

    CHECK(queue.write(full.data(), full.size()));
    const auto start = steady_clock::now();
    CHECK(!queue.write(next, sizeof(next)));
    CHECK(steady_clock::now() - start >= milliseconds(200));
    CHECK(queue.read(out, sizeof(out)) == 0);
    CHECK(out[0] == 0 && out[7] == 0);

    std::vector<char> tenMs(160, 1);
    const auto silentStart = steady_clock::now();
    for (int i = 0; i < 5; ++i) CHECK(queue.write(tenMs.data(), tenMs.size()));
    CHECK(steady_clock::now() - silentStart >= milliseconds(40));

    queue.setAvailable(true);
    CHECK(queue.write(first, sizeof(first)));
    queue.setAvailable(false);
    CHECK(queue.read(out, sizeof(out)) == 0);

    queue.setAvailable(true);
    CHECK(queue.write(full.data(), full.size()));
    done.store(false);
    std::thread closingWriter([&] { queue.write(tenMs.data(), tenMs.size()); done.store(true); });
    std::this_thread::sleep_for(milliseconds(25));
    const bool waitingAtClose = !done.load();
    queue.close();
    closingWriter.join();
    CHECK(waitingAtClose && done.load());
    queue.setAvailable(true);
    CHECK(queue.read(out, sizeof(out)) == 0);
    const auto closedStart = steady_clock::now();
    for (int i = 0; i < 5; ++i) CHECK(queue.write(tenMs.data(), tenMs.size()));
    CHECK(steady_clock::now() - closedStart >= milliseconds(40));
    CHECK(queue.read(out, sizeof(out)) == 0);
    std::puts("audio queue tests passed");
    return 0;
}
