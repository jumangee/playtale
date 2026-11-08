#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Returns true if the directory name is a cache leaf this firmware creates under
// /.crosspoint and should therefore be reclaimed by Settings -> Clear cache.
bool isBookCacheDirectoryName(const char* name);

// Delete a local book and its local clipping state after the file is removed.
bool removeBookFile(const std::string& path);
