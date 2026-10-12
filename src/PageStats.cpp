#include "PageStats.h"
// 最后 include：Serial 重定向进日志环形缓冲（/debug 网页调试台，DESIGN §11）
#include "DbgLog.h"

void PageStats::S::add(uint32_t dt, bool ok) {
  n++;
  if (!ok) { timeouts++; return; }
  sum += dt;
  if (tmin == 0 || dt < tmin) tmin = dt;
  if (dt > tmax) tmax = dt;
}

void PageStats::add(bool full, uint32_t dt, bool ok) {
  (full ? m_full : m_fast).add(dt, ok);
}

void PageStats::printSummary(uint32_t pageCount, uint32_t fastTargetMs) const {
  Serial.printf("== stats after %lu pages ==\n", (unsigned long)pageCount);
  if (m_fast.n) {
    Serial.printf("fast: n=%lu avg=%lums min=%lu max=%lu (target<=%lums: %s)\n",
                  (unsigned long)m_fast.n, (unsigned long)m_fast.avg(),
                  (unsigned long)m_fast.tmin, (unsigned long)m_fast.tmax,
                  (unsigned long)fastTargetMs,
                  m_fast.tmax && m_fast.tmax <= fastTargetMs ? "MEET" : "OVER");
  }
  if (m_full.n) {
    Serial.printf("full: n=%lu avg=%lums min=%lu max=%lu\n",
                  (unsigned long)m_full.n, (unsigned long)m_full.avg(),
                  (unsigned long)m_full.tmin, (unsigned long)m_full.tmax);
  }
  Serial.printf("timeouts: fast=%lu full=%lu\n",
                (unsigned long)m_fast.timeouts, (unsigned long)m_full.timeouts);
}

void PageStats::get(bool full, uint32_t& n, uint32_t& avg, uint32_t& min,
                    uint32_t& max, uint32_t& timeouts) const {
  const S& s = full ? m_full : m_fast;
  n = s.count();
  avg = s.avg();
  min = s.tmin;
  max = s.tmax;
  timeouts = s.timeouts;
}
