#pragma once

#include <cstdio>
#include <cstdlib>

template <typename... Args>
inline void panic(const char *msg, Args... args) {
  fprintf(stderr, "PANIC: ");
  fprintf(stderr, msg, args...);
  fprintf(stderr, "\n");
  exit(1);
}
