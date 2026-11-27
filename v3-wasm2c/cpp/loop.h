// Event-loop inversion.  The engine calls schedule_main_loop instead of
// blocking, so the host owns the pump.  No guest indices: every entry point
// comes from the generated WC.h by name.
#pragma once

#include <chrono>

struct w2c_WebClient;

namespace weblox::loop {

// True while the engine has asked to be pumped again.
bool scheduled();

// Bounded run: enter the module's main entry once, then pump until the deadline.
// Returns the number of loop iterations performed.
int run(w2c_WebClient* inst, std::chrono::milliseconds budget);

}  // namespace weblox::loop
