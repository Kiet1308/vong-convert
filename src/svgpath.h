#pragma once
#include <d2d1_1.h>

#include "common.h"

// Builds a D2D path from SVG path data (M L H V C A Z, absolute and relative).
// Used for the 24x24 line icons; figures stay open unless the data closes them.
ComPtr<ID2D1PathGeometry> BuildSvgPath(ID2D1Factory* factory, const char* data);
