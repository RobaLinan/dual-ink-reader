#ifndef PAGE_STATS_H
#define PAGE_STATS_H

#include <Arduino.h>

// 刷新耗时统计（P2/P3 验收数据）：快刷/全刷分开记 min/avg/max + 超时数，
// 每 N 页调一次 printSummary() 打汇总
class PageStats {
public:
  // dt = 本次刷新耗时(ms)，ok = false 表示 BUSY 超时（不计入耗时）
  void add(bool full, uint32_t dt, bool ok);
  void printSummary(uint32_t pageCount, uint32_t fastTargetMs) const;

private:
  struct S {
    uint32_t n = 0, sum = 0, tmin = 0, tmax = 0, timeouts = 0;
    void add(uint32_t dt, bool ok);
    uint32_t count() const { return n - timeouts; }
    uint32_t avg() const { return count() ? sum / count() : 0; }
  };
  S m_fast, m_full;
};

#endif  // PAGE_STATS_H
