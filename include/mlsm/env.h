#pragma once

#include <cstdint>
#include <string>

#include "mlsm/status.h"

namespace mlsm {

class WritableFile;
class SequentialFile;

// Minimal filesystem access layer. Rather than a full LevelDB-style Env object,
// these free functions hand back real Posix-backed implementations of the
// abstract file interfaces defined in earlier commits. Callers own the returned
// object and must delete it.

// Creates (truncating) or opens a file for appending writes.
Status NewWritableFile(const std::string& path, WritableFile** result);

// Opens an existing file for appending, keeping its current contents. Used to
// resume writing to a WAL after recovery. Creates the file if absent.
Status NewAppendableFile(const std::string& path, WritableFile** result);

// Opens a file for sequential reading.
Status NewSequentialFile(const std::string& path, SequentialFile** result);

bool FileExists(const std::string& path);

// Returns the size of an existing file in bytes. Used to seed the WAL Writer's
// block cursor when resuming an existing log.
Status GetFileSize(const std::string& path, uint64_t* size);

// Creates a directory. An already-existing directory is treated as success.
Status MakeDir(const std::string& path);

}  // namespace mlsm
