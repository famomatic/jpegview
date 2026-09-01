#include "StdAfx.h"
#include "WorkThread.h"

#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

CAppModule _Module;

class TestRequest : public CRequestBase {
public:
    explicit TestRequest(int value, HANDLE finished = NULL) : CRequestBase(finished), value(value) {}
    int value;
};

class TestThread final : public CWorkThread {
public:
    TestThread() : CWorkThread(false), gate(NULL) {}
    bool Run(int value) { return ProcessAndWait(new TestRequest(value)); }
    bool Queue(TestRequest* request) { return ProcessAsync(request); }
    HANDLE gate;
    std::vector<int> processed;

protected:
    void ProcessRequest(CRequestBase& base) override {
        TestRequest& request = static_cast<TestRequest&>(base);
        if (request.value == -1) throw std::runtime_error("intentional");
        if (request.value == 100 && gate != NULL) ::WaitForSingleObject(gate, INFINITE);
        processed.push_back(request.value);
    }
};

int main() {
    TestThread worker;
    for (int i = 0; i < 200; ++i) if (!worker.Run(i)) return 1;
    if (worker.Run(-1)) return 2; // exceptions must signal the waiter and report failure

    HANDLE gate = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    HANDLE firstDone = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    HANDLE queuedDone = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    if (gate == NULL || firstDone == NULL || queuedDone == NULL) return 3;
    worker.gate = gate;
    TestRequest* first = new TestRequest(100, firstDone);
    TestRequest* queued = new TestRequest(101, queuedDone);
    if (!worker.Queue(first) || !worker.Queue(queued)) return 4;
    ::Sleep(20);
    std::thread terminator([&worker]() { worker.Terminate(); });
    ::Sleep(20);
    ::SetEvent(gate);
    terminator.join();
    if (::WaitForSingleObject(firstDone, 1000) != WAIT_OBJECT_0 ||
        ::WaitForSingleObject(queuedDone, 1000) != WAIT_OBJECT_0 || !queued->Processed.load()) return 5;
    first->Deleted.store(true);
    queued->Deleted.store(true);
    TestRequest* rejected = new TestRequest(102);
    if (worker.Queue(rejected)) return 6;
    delete rejected;
    ::CloseHandle(gate);
    ::CloseHandle(firstDone);
    ::CloseHandle(queuedDone);
    std::puts("WorkThreadTest passed");
    return 0;
}
