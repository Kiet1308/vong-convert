#pragma once
#include <algorithm>
#include <cmath>

#include "common.h"

// Damped spring used for every animation; dt is in seconds.
struct Spring {
  float v = 0, t = 0, vel = 0, k = 420, c = 32, eps = 0.001f;

  Spring() = default;
  Spring(float v0, float stiffness, float damping, float epsilon = 0.001f)
      : v(v0), t(v0), k(stiffness), c(damping), eps(epsilon) {
    if (ReducedMotion()) { k = 900; c = 60; }
  }

  void To(float x) { t = x; }
  void Snap(float x) { v = t = x; vel = 0; }
  bool Moving() const { return v != t || vel != 0; }

  void Step(float dt) {
    if (!Moving()) return;
    int n = std::max(1, (int)std::ceil(dt * 240));
    float h = dt / n;
    for (int i = 0; i < n; ++i) {
      float a = -k * (v - t) - c * vel;
      vel += a * h;
      v += vel * h;
    }
    if (std::fabs(v - t) < eps && std::fabs(vel) < eps) { v = t; vel = 0; }
  }
};
