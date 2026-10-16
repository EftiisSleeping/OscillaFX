#include "ui/DockReopen.h"

// No Dock on Windows: relaunching the exe reaches anotherInstanceStarted(), which shows the window.
namespace DockReopen
{
void setHandler (std::function<void()>) {}
}
