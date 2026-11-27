// No-op default: keeps the host buildable without any platform libraries.
#include "window.h"

namespace weblox {

bool Window::init() { return false; }
void Window::pump() {}
void Window::quit() {}

}  // namespace weblox
