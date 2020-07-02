#pragma once

class Timer {
  public:
    Timer();

    double DeltaTime() const;
    double TotalTime() const;

    void Reset();
    void Start();
    void Stop();
    void Tick();

  private:
    double sec_per_cnt;
    double delta_time;

    long long base_time;
    long long paused_time;
    long long stop_time;
    long long prev_time;
    long long curr_time;

    bool stopped;
};