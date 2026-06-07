#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void gd_skip_rules_load(void);
bool gd_skip_exact(const char *exe_lower);
bool gd_skip_heuristic(const char *exe_lower);

#ifdef __cplusplus
}
#endif
