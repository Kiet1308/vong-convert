#pragma once
#include "catalog.h"
#include "common.h"

struct JobRequest {
  Mode mode = Mode::Convert;
  std::wstring itemId, subId;
  std::vector<std::wstring> files;
};

struct JobResult {
  bool ok = false;
  std::vector<std::wstring> outputs;
  std::wstring title, detail;  // notification text
};

// Runs the request on a worker thread. Progress and completion are posted to
// `notify` as WM_APP_JOB (wParam = returned id). Returns 0 if nothing can run.
int StartJob(HWND notify, const JobRequest& request);
// Fetches and forgets the result once WM_APP_JOB reported kJobDone or kJobFailed.
JobResult TakeJobResult(int id);
// Kills running FFmpeg/7-Zip processes; called on exit.
void ShutdownJobs();

// Decodes the picture (or a video's first frame) exactly as FFmpeg will when cropping,
// rotation included, scaled down to fit boxW x boxH, into a PNG. fullW/fullH receive the real size.
bool MakePreview(const std::wstring& in, const std::wstring& outPng, int boxW, int boxH, int* fullW, int* fullH);
