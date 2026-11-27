#pragma once
// ImGui config for t8_uitest only (TEST-5): every IM_ASSERT stays on, also in a Release build, and a failed one stops the test with
// the expression, the file and what the test was doing. The program itself keeps ImGui's default (asserts off in Release).
void T8UiAssertFailed(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : T8UiAssertFailed(#_EXPR, __FILE__, __LINE__))
