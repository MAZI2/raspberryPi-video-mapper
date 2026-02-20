// src/common.c
#include "common.h"

volatile int keepRunning = 1;

void handle_sigint(int sig)
{
    (void)sig;
    keepRunning = 0;
}
