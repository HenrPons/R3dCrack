#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

struct alignas(8) SharedRunState {
    volatile LONG percent;
    volatile LONG stop;
    volatile LONG state; // 0 initializing, 1 running, 2 paused, 3 finished, 4 error, 5 found
    LONG ownerPid;
    alignas(8) volatile LONG64 keysPerSecond;
    alignas(8) volatile LONG64 total;
};

class RunControl {
    HANDLE _mapping = NULL, _owner = NULL;
public:
    SharedRunState *data = nullptr;
    ~RunControl() { close(); }
    void close() {
        if(data) UnmapViewOfFile(data);
        if(_mapping) CloseHandle(_mapping);
        if(_owner) CloseHandle(_owner);
        data = nullptr; _mapping = _owner = NULL;
    }
    bool create(const std::string &name, unsigned percent) {
        close();
        _mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedRunState), name.c_str());
        if(!_mapping) return false;
        if(GetLastError() == ERROR_ALREADY_EXISTS) { close(); return false; }
        data = (SharedRunState*)MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRunState));
        if(!data) { close(); return false; }
        ZeroMemory(data, sizeof(*data)); data->percent = percent; data->ownerPid = GetCurrentProcessId();
        return true;
    }
    bool open(const std::string &name) {
        _mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if(!_mapping) return false;
        data = (SharedRunState*)MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRunState));
        if(!data) return false;
        _owner = OpenProcess(SYNCHRONIZE, FALSE, data->ownerPid);
        return _owner != NULL;
    }
    unsigned percent(unsigned fallback) const {
        if(!data) return fallback;
        LONG p = InterlockedCompareExchange(&data->percent, 0, 0);
        return p < 0 ? 0 : p > 100 ? 100 : (unsigned)p;
    }
    bool stopping() const {
        return data && (InterlockedCompareExchange(&data->stop, 0, 0) ||
            (_owner && WaitForSingleObject(_owner, 0) == WAIT_OBJECT_0));
    }
};

// Separate lock file survives atomic checkpoint replacement while keeping
// concurrent workers from loading and overwriting the same progress.
class CheckpointLock {
    HANDLE file = INVALID_HANDLE_VALUE;
public:
    ~CheckpointLock() { if(file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    bool acquire(const std::string &checkpoint) {
        file = CreateFileA((checkpoint + ".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        return file != INVALID_HANDLE_VALUE;
    }
};
#endif
