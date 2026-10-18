#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "common.h"

// Small helpers for the --bench command and per-session timing in the log.
namespace perf {

double NowMs();

struct Stats {
  std::vector<double> v;
  void Add(double x) { v.push_back(x); }
  double Avg() const;
  double Pct(double q) const;  // q in [0, 1]
  double Max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
  std::wstring Line(const wchar_t* name) const;  // "name avg 1.23 / p95 2.34 / max 3.45 ms (n)"
};

// Flushes Direct3D and waits until the GPU has finished everything submitted so far.
void WaitGpu();
// Process memory: private bytes and GPU memory this process holds.
std::wstring Memory();

}  // namespace perf
