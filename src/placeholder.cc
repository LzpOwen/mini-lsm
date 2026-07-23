// Placeholder translation unit so the mlsm library has something to compile
// before real sources (crc32c.cc, wal_writer.cc, ...) land in W1.
// Delete this once the first real source file is added.

namespace mlsm {
namespace {
// Keeps the object file non-empty on all toolchains.
[[maybe_unused]] int mlsm_placeholder = 0;
}  // namespace
}  // namespace mlsm