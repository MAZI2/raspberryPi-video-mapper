#pragma once

// ============================================================
// Common includes + configuration shared across modules
// ============================================================

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <gpiod.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <dirent.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

// ================= CONFIG =================

#define GRID_X 16
#define GRID_Y 9

// MiniMAD board GPIOs (BCM)
#define GPIO_BTN1   17  // cycle corner (EDIT+SELECT), random video (EDIT OFF)
#define GPIO_BTN2   18  // toggle SELECT<->MOVE (EDIT ON)
#define GPIO_BTN3   27  // toggle EDIT mode
#define GPIO_UP     24
#define GPIO_DOWN   22
#define GPIO_LEFT   25
#define GPIO_RIGHT  23

extern volatile int keepRunning;

// Debounce (ms)
#define DEBOUNCE_MS 120

// Crossfade duration (seconds)
#define XFADE_SECONDS 0.60f

// Corner order for homography: BL, BR, TR, TL
typedef enum { C_BL=0, C_BR=1, C_TR=2, C_TL=3 } CornerSq;

// UI cycle order: TL, TR, BL, BR
extern const int UI_TO_SQ_CORNER[4];

const char* corner_name_ui(int uiIdx);
void handle_sigint(int sig);
