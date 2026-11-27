// Emscripten EM_ASM dispatch for the clean C host.
//
// The engine calls env.emscripten_asm_const_* with a numeric page-side closure
// key (an ASM_CONSTS index from WebClient.js), a guest signature string and a
// packed guest argument buffer.  This header exposes the clock anchor used by
// the performance.now key; weblox_asm_const_init() is idempotent and is called
// automatically before the first dispatch.
#ifndef WEBLOX_ASM_CONST_H
#define WEBLOX_ASM_CONST_H

#include "host.h"

// Anchor the monotonic clock to process start so performance.now() returns
// small elapsed values instead of a time-since-boot timestamp.
void weblox_asm_const_init(void);

#endif  // WEBLOX_ASM_CONST_H
