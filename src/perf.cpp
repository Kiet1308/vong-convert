#include "perf.h"

#include <psapi.h>

#include <cstdio>

#include "gfx.h"

namespace perf {

double NowMs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart * 1000.0 / freq.QuadPart;
}

double Stats::Avg() const {
  if (v.empty()) return 0;
  double s = 0;
  for (double x : v) s += x;
  return s / v.size();
}

double Stats::Pct(double q) const {
  if (v.empty()) return 0;
  std::vector<double> s = v;
  std::sort(s.begin(), s.end());
  return s[std::min(s.size() - 1, (size_t)(q * (s.size() - 1) + .5))];
}

std::wstring Stats::Line(const wchar_t* name) const {
  wchar_t buf[200];
  swprintf(buf, 200, L"%ls avg %.2f / p95 %.2f / max %.2f ms (%zu)", name, Avg(), Pct(.95), Max(), v.size());
  return buf;
}

void WaitGpu() {
  if (!g_gfx.d3d) return;
  ComPtr<ID3D11DeviceContext> ctx;
  g_gfx.d3d->GetImmediateContext(&ctx);
  D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
  ComPtr<ID3D11Query> q;
  if (FAILED(g_gfx.d3d->CreateQuery(&qd, &q))) return;
  ctx->End(q.Get());
  ctx->Flush();
  BOOL done = FALSE;
  while (ctx->GetData(q.Get(), &done, sizeof(done), 0) == S_FALSE) YieldProcessor();
}

std::wstring Memory() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
  double gpu = 0;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIAdapter3> a3;
  if (g_gfx.dxgi && SUCCEEDED(g_gfx.dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&a3))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
    gpu = (double)(local.CurrentUsage + nonLocal.CurrentUsage);
  }
  wchar_t buf[120];
  swprintf(buf, 120, L"private %.1f MB, GPU %.1f MB", pmc.PrivateUsage / 1048576.0, gpu / 1048576.0);
  return buf;
}

}  // namespace perf
