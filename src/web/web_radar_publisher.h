#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EntityManager;
class Process;

class WebRadarPublisher {
public:
    WebRadarPublisher() = default;
    ~WebRadarPublisher();

    void update(const Process& proc, const EntityManager& em);

private:
    std::string ensureSessionId();
    bool postSnapshot(const std::string& payloadJson);
    void ensureWorkerStarted();
    void stopWorker();
    void enqueueSnapshot(std::string payloadJson);
    void workerLoop();

    std::uint64_t m_lastPublishMs = 0;
    std::string m_sessionId;

    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::vector<std::thread> m_workers;
    std::deque<std::string> m_payloadQueue;
    bool m_workersStarted = false;
    bool m_stopWorker = false;
};
