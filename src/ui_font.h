#pragma once

#include <stdint.h>

void uiDrawCenter(const char* text, int cx, int cy, uint16_t color, bool large);
int uiTextWidth(const char* text, bool large);
