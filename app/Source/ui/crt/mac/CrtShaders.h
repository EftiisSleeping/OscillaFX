#pragma once

#include <string>

// Metal Shading Language source for the CRT, compiled at runtime (newLibraryWithSource) because
// the build machine has no offline `metal` compiler. The uniform structs declared at the top of the
// source are mirrored byte-for-byte in MetalCrtView.mm.
namespace Crt
{
    std::string metalShaderSource();
}
