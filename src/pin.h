#pragma once
#include "common.h"

// "Ghim nổi": small always-on-top cards in the top-right corner holding files the user
// wants at hand. Drag a card out to drop its files into any app; double-click opens them.
namespace pins {
void Pin(const std::vector<std::wstring>& files, POINT from);
bool TickAll(float dt);
void RenderAll();
size_t Count();
void Clear();
}  // namespace pins
