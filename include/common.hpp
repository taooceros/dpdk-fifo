#pragma once

#include <cstdio>
#include <cstdlib>

inline void panic(const char *msg) {
  fprintf(stderr, "PANIC: %s\n", msg);
  exit(1);
}