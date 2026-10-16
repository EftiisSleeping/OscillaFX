#include "GalleryStore.h"
#include "AppPaths.h"
#include "platform/ImageValidation.h"
#if ! JUCE_WINDOWS
 #include <climits>
 #include <cstdlib>
 #include <sys/stat.h>
#endif

namespace
{
const char* const IMAGE_PATTERN = "*.png;*.jpg;*.jpeg;*.webp;*.heic;*.gif;*.tif;*.tiff;*.bmp";
const char* const TEMP_PREFIX = ".oscilla-import-";
constexpr int MAX_STEM_LENGTH = 60;
constexpr int MAX_UNIQUE_SUFFIX = 9999;

#if JUCE_WINDOWS
// Symlinks / junctions on the file itself resolved (JUCE uses GetFinalPathNameByHandle); empty when
// the path does not exist.
juce::String realPath (const juce::File& file)
{
    return file.exists() ? file.getLinkedTarget().getFullPathName() : juce::String();
}

bool isRegularFile (const juce::String& path) { return juce::File (path).existsAsFile(); }

juce::int64 fileSize (const juce::String& path)
{
    const juce::File file (path);
    return file.existsAsFile() ? file.getSize() : -1;
}
#else
// Symlinks and ".." resolved; empty when the path does not exist.
juce::String realPath (const juce::File& file)
{
    char buffer[PATH_MAX];
    if (::realpath (file.getFullPathName().toRawUTF8(), buffer) == nullptr)
        return {};
    return juce::String::fromUTF8 (buffer);
}

bool isRegularFile (const juce::String& path)
{
    struct stat info {};
    return ::stat (path.toRawUTF8(), &info) == 0 && S_ISREG (info.st_mode);
}

juce::int64 fileSize (const juce::String& path)
{
    struct stat info {};
    return ::stat (path.toRawUTF8(), &info) == 0 ? (juce::int64) info.st_size : -1;
}
#endif

// Letters, digits, space, dash, underscore; never empty, never a dotfile, never long.
juce::String safeStem (const juce::File& source)
{
    juce::String stem;
    for (auto c : source.getFileNameWithoutExtension())
        if (juce::CharacterFunctions::isLetterOrDigit (c) || c == ' ' || c == '-' || c == '_')
            stem += c;
    stem = stem.trim().substring (0, MAX_STEM_LENGTH).trim();
    return stem.isEmpty() ? juce::String ("image") : stem;
}

juce::File uniqueTarget (const juce::File& dir, const juce::String& stem, const juce::String& extension)
{
    auto candidate = dir.getChildFile (stem + "." + extension);
    for (int n = 2; candidate.existsAsFile() || candidate.isSymbolicLink() || candidate.isDirectory(); ++n)
    {
        if (n > MAX_UNIQUE_SUFFIX)
            return {};
        candidate = dir.getChildFile (stem + "-" + juce::String (n) + "." + extension);
    }
    return candidate;
}

juce::String line (const juce::File& source, const juce::String& why)
{
    return source.getFileName() + ": " + why;
}

void listPictures (const juce::File& dir, juce::Array<juce::File>& into)
{
    if (! dir.isDirectory())
        return;
    for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, IMAGE_PATTERN))
        if (! file.getFileName().startsWith ("."))
            into.add (file);
}
} // namespace

GalleryStore::GalleryStore (juce::File userDirectory, juce::File bundledDirectory)
    : userDir (std::move (userDirectory)), bundledDir (std::move (bundledDirectory)) {}

juce::File GalleryStore::defaultUserDirectory()
{
    return AppPaths::dataDirectory().getChildFile ("Gallery");
}

juce::File GalleryStore::defaultBundledDirectory()
{
    return AppPaths::resourcesDirectory().getChildFile ("Gallery");
}

juce::Array<juce::File> GalleryStore::images() const
{
    juce::Array<juce::File> found;
    const auto overrideDir = juce::SystemStats::getEnvironmentVariable ("OSCILLA_GALLERY_DIR", {});
    if (overrideDir.isNotEmpty() && userDir == defaultUserDirectory())
    {
        listPictures (juce::File (overrideDir), found);
    }
    else
    {
        listPictures (userDir, found);
        listPictures (bundledDir, found);
        if (found.isEmpty() && userDir == defaultUserDirectory())
        {
            // <repo>/app/Source/core/GalleryStore.cpp -> <repo>/assets/gallery
            auto dir = juce::File (__FILE__);
            for (int i = 0; i < 4; ++i)
                dir = dir.getParentDirectory();
            listPictures (dir.getChildFile ("assets/gallery"), found);
        }
    }
    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareNatural (b.getFileName()) < 0;
    });
    return found;
}

int GalleryStore::importImages (const juce::Array<juce::File>& sources, juce::String& error)
{
    error.clear();
    if (sources.isEmpty())
        return 0;
    if (auto created = userDir.createDirectory(); created.failed())
    {
        error = "Cannot create the gallery folder: " + created.getErrorMessage();
        return 0;
    }

    juce::StringArray problems;
    int imported = 0;
    for (const auto& source : sources)
    {
        const auto sourcePath = realPath (source);
        if (sourcePath.isEmpty() || ! isRegularFile (sourcePath))
        {
            problems.add (line (source, "not a readable file."));
            continue;
        }
        const auto size = fileSize (sourcePath);
        if (size <= 0 || size > maxImageBytes)
        {
            problems.add (line (source, size <= 0 ? juce::String ("empty file.") : juce::String ("larger than 50 MB.")));
            continue;
        }

        // Validate the bytes we keep, not the original, which could change between check and copy.
        const auto temp = userDir.getChildFile (TEMP_PREFIX + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        if (! juce::File (sourcePath).copyFileTo (temp))
        {
            temp.deleteFile();
            problems.add (line (source, "could not copy into the gallery folder."));
            continue;
        }
        const auto verdict = temp.getSize() <= maxImageBytes ? ImageValidation::inspect (temp)
                                                              : ImageValidation::Result { false, {}, "larger than 50 MB." };
        auto target = verdict.isImage ? uniqueTarget (userDir, safeStem (source), verdict.extension) : juce::File();
        if (target == juce::File() || ! temp.moveFileTo (target))
        {
            temp.deleteFile();
            problems.add (line (source, verdict.isImage ? juce::String ("could not store the image.") : verdict.error));
            continue;
        }
        ++imported;
    }
    error = problems.joinIntoString ("\n");
    return imported;
}

bool GalleryStore::removeImage (const juce::File& file)
{
    const auto folder = realPath (userDir);
    const auto parent = realPath (file.getParentDirectory());
    if (folder.isEmpty() || parent != folder || file.getFileName().isEmpty() || file.getFileName().startsWith ("."))
        return false;
    // The entry may be a symlink to a picture: deleting removes only the link, never its target.
    // Folders, links to folders and dangling links are not pictures.
    const auto resolved = realPath (file);
    if (resolved.isEmpty() || ! isRegularFile (resolved))
        return false;
    return file.deleteFile();
}
