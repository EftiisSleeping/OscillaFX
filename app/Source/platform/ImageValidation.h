#pragma once

#include <juce_core/juce_core.h>

// Decides from the file's bytes (never the extension) whether a file is a picture the gallery can
// show. Thread-safe. macOS sniffs with ImageIO (jpg png webp heic gif tiff); Windows decodes with
// JUCE (jpg png gif only).
namespace ImageValidation
{
struct Result
{
    bool isImage = false;
    juce::String extension;  // canonical, without dot: jpg png webp heic gif tiff
    juce::String error;      // user-readable reason when not an image
};

Result inspect (const juce::File&);
} // namespace ImageValidation
