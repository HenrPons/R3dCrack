#pragma once
#include "KeySearchDevice.h"
#include "AddressUtil.h"
#include "SearchArithmetic.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

// Independent CPU backend. Persistent threads own disjoint contiguous batches.
class CpuKeySearchDevice : public KeySearchDevice {
    static const unsigned BATCH = 32;
    unsigned _threads;
    unsigned _lastPoints = 0;
    int _compression = 0;
    secp256k1::uint256 _next, _stride, _end = secp256k1::N - 1;
    std::set<KeySearchTarget> _targets;
    std::vector<KeySearchResult> _results;
    std::vector<std::thread> _workers;
    std::mutex _mutex;
    std::condition_variable _work, _done;
    unsigned _epoch = 0, _remaining = 0;
    bool _quit = false;
    std::exception_ptr _error;
    secp256k1::ecpoint _increment;

    void worker(unsigned id) {
#ifdef _WIN32
        // Explicitly spread workers across processor groups, including multi-socket
        // systems and Windows versions whose default affinity is one group.
        unsigned logical = id % GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        for(WORD group = 0; group < GetActiveProcessorGroupCount(); ++group) {
            DWORD count = GetActiveProcessorCount(group);
            if(logical < count) {
                GROUP_AFFINITY affinity = {};
                affinity.Group = group;
                affinity.Mask = count >= sizeof(KAFFINITY)*8 ? ~(KAFFINITY)0 : (((KAFFINITY)1 << count) - 1);
                if(!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
                    std::lock_guard<std::mutex> guard(_mutex);
                    _error = std::make_exception_ptr(std::runtime_error("CPU group affinity failed"));
                }
                break;
            }
            logical -= count;
        }
#endif
        unsigned seen = 0;
        for(;;) {
            std::unique_lock<std::mutex> lock(_mutex);
            _work.wait(lock, [&] { return _quit || seen != _epoch; });
            if(_quit) return;
            seen = _epoch;
            auto key = _next + _stride * (id * BATCH);
            lock.unlock();
            std::vector<KeySearchResult> found;
            try {
                if(key.cmp(_end) <= 0) {
                    auto point = secp256k1::multiplyPoint(key, secp256k1::G());
                    for(unsigned i = 0; i < BATCH && key.cmp(_end) <= 0; ++i) {
                        for(int mode = 0; mode < 2; ++mode) {
                            if(_compression != PointCompressionType::BOTH && mode != _compression) continue;
                            unsigned int digest[5];
                            if(mode == PointCompressionType::COMPRESSED) Hash::hashPublicKeyCompressed(point, digest);
                            else Hash::hashPublicKey(point, digest);
                            if(_targets.count(KeySearchTarget(digest))) {
                                KeySearchResult r;
                                r.privateKey = key; r.publicKey = point; r.compressed = mode == 0;
                                memcpy(r.hash, digest, sizeof(digest));
                                found.push_back(r);
                            }
                        }
                        key = key + _stride;
                        if(i + 1 < BATCH && key.cmp(_end) <= 0) point = secp256k1::addPoints(point, _increment);
                    }
                }
            } catch(...) {
                std::lock_guard<std::mutex> guard(_mutex);
                _error = std::current_exception();
            }
            lock.lock();
            _results.insert(_results.end(), found.begin(), found.end());
            if(--_remaining == 0) _done.notify_one();
        }
    }
public:
    explicit CpuKeySearchDevice(unsigned threads) : _threads(threads ? threads : 1) {}
    ~CpuKeySearchDevice() override {
        { std::lock_guard<std::mutex> lock(_mutex); _quit = true; }
        _work.notify_all();
        for(auto &t : _workers) t.join();
    }
    void init(const secp256k1::uint256 &start, int compression, const secp256k1::uint256 &stride) override {
        _next = start; _stride = stride; _compression = compression;
        _increment = secp256k1::multiplyPoint(stride, secp256k1::G());
        try { for(unsigned i = 0; i < _threads; ++i) _workers.emplace_back(&CpuKeySearchDevice::worker, this, i); }
        catch(...) { throw KeySearchException("Unable to create CPU threads"); }
    }
    void setEndKey(const secp256k1::uint256 &end) override { _end = end; }
    void doStep() override {
        std::unique_lock<std::mutex> lock(_mutex);
        _lastPoints = SearchArithmetic::validPoints(_next, _end, _stride, _threads * BATCH);
        _remaining = _threads; ++_epoch; _work.notify_all();
        _done.wait(lock, [&] { return _remaining == 0; });
        if(_error) throw KeySearchException("CPU search failed");
        _next = _next + _stride * (_threads * BATCH);
    }
    void setTargets(const std::set<KeySearchTarget> &targets) override { _targets = targets; }
    size_t getResults(std::vector<KeySearchResult> &results) override {
        results.swap(_results); return results.size();
    }
    uint64_t keysPerStep() override { return _epoch ? _lastPoints : (uint64_t)_threads * BATCH; }
    std::string getDeviceName() override { return "CPU (" + std::to_string(_threads) + " threads)"; }
    void getMemoryInfo(uint64_t &free, uint64_t &total) override { free = total = 0; }
    secp256k1::uint256 getNextKey() override { return _next; }
};
