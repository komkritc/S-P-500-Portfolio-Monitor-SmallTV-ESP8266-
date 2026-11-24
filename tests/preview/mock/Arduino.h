// Host stand-in for <Arduino.h>: only what the display code and the font renderer touch.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#define PROGMEM
#define PSTR(x) x
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define memcpy_P memcpy
using std::min;
using std::max;
