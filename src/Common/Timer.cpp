#include "Timer.h"

#include <windows.h>

Timer::Timer() : sec_per_cnt(0), delta_time(-1), base_time(0), paused_time(0),
        stop_time(0), prev_time(0), curr_time(0), stopped(false)  {
    long long cnt_per_sec;
    QueryPerformanceFrequency((LARGE_INTEGER *) &cnt_per_sec);
    sec_per_cnt = 1.0 / cnt_per_sec;
}

double Timer::DeltaTime() const {
    return delta_time;
}

double Timer::TotalTime() const {
    if (stopped) {
        return (stop_time - base_time - paused_time) * sec_per_cnt;
    } else {
        return (curr_time - base_time - paused_time) * sec_per_cnt;
    }
}

void Timer::Reset() {
    QueryPerformanceCounter((LARGE_INTEGER *) &base_time);
    prev_time = base_time;
    stop_time = 0;
    stopped = false;
}

void Timer::Start() {
    if (stopped) {
        long long start_time;
        QueryPerformanceCounter((LARGE_INTEGER *) &start_time);
        paused_time += start_time - stop_time;
        prev_time = start_time;
        stop_time = 0;
        stopped = false;
    }
}

void Timer::Stop() {
    if (!stopped) {
        QueryPerformanceCounter((LARGE_INTEGER *) &stop_time);
        stopped = true;
    }
}

void Timer::Tick() {
    if (stopped) {
        delta_time = 0;
        return;
    }

    QueryPerformanceCounter((LARGE_INTEGER *) &curr_time);
    delta_time += (curr_time - prev_time) * sec_per_cnt;
    prev_time = curr_time;
    if (delta_time < 0) {
        delta_time = 0;
    }
}