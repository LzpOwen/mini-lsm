#include "mlsm/raft/replicated_db.h"

#include <utility>

#include "mlsm/coding.h"

namespace mlsm {
namespace raft {

namespace {

// Command value types, byte-compatible with the DB WAL's ValueType (src/db.cc)
// so the on-log encoding reads the same. The Raft core treats the encoded bytes
// as an opaque payload; only this layer interprets them.
enum CommandType : uint8_t {
  kDelete = 0,
  kPut = 1,
};

// [type:1][klen:fixed32][key]([vlen:fixed32][value] only for Put) — the same
// layout as db.cc's EncodeRecord, reimplemented here so this layer does not
// reach into the storage engine's internals.
std::string EncodeCommand(CommandType type, const Slice& key,
                          const Slice& value) {
  std::string out;
  out.push_back(static_cast<char>(type));
  char buf[4];
  EncodeFixed32(buf, static_cast<uint32_t>(key.size()));
  out.append(buf, 4);
  out.append(key.data(), key.size());
  if (type == kPut) {
    EncodeFixed32(buf, static_cast<uint32_t>(value.size()));
    out.append(buf, 4);
    out.append(value.data(), value.size());
  }
  return out;
}

bool DecodeCommand(const std::string& record, CommandType* type,
                   std::string* key, std::string* value) {
  const char* p = record.data();
  size_t left = record.size();
  if (left < 1 + 4) return false;
  *type = static_cast<CommandType>(static_cast<uint8_t>(p[0]));
  p += 1;
  left -= 1;

  uint32_t klen = DecodeFixed32(p);
  p += 4;
  left -= 4;
  if (left < klen) return false;
  key->assign(p, klen);
  p += klen;
  left -= klen;

  if (*type == kPut) {
    if (left < 4) return false;
    uint32_t vlen = DecodeFixed32(p);
    p += 4;
    left -= 4;
    if (left < vlen) return false;
    value->assign(p, vlen);
  } else {
    value->clear();
  }
  return true;
}

}  // namespace

Status ReplicatedDB::Open(const std::string& dir, NodeId id,
                          std::vector<NodeId> peers, ReplicatedDB** out) {
  RaftStorage* storage = nullptr;
  Status s = RaftStorage::Open(dir + "/raft.log", &storage);
  if (!s.ok()) return s;

  HardState hs;
  std::vector<LogEntry> entries;
  s = storage->Recover(&hs, &entries);
  if (!s.ok()) {
    delete storage;
    return s;
  }

  const bool single_node = (peers.size() == 1);

  Config cfg;
  cfg.id = id;
  cfg.peers = std::move(peers);
  cfg.election_timeout = 1;  // A ready-to-drive timeout; symmetry-breaking in
                             // multi-node tests comes from the test Cluster.
  Raft raft(cfg);
  raft.Restore(hs.term, hs.vote, entries);

  ReplicatedDB* db = new ReplicatedDB(std::move(raft), storage);

  // Rebuild the state machine from the recovered log. On a single node an entry
  // is committed the moment it is proposed (self-majority), so every durable
  // entry is safe to apply. (Multi-node recovery would need a persisted commit
  // index or a fresh election to re-establish the commit point; out of scope.)
  for (const LogEntry& e : entries) {
    CommandType type;
    std::string key, value;
    if (DecodeCommand(e.data, &type, &key, &value)) {
      if (type == kPut) {
        db->table_[key] = value;
      } else {
        db->table_.erase(key);
      }
    }
  }
  db->applied_index_ = entries.empty() ? 0 : entries.back().index;

  // A single-node cluster elects itself so it is immediately writable. Ticking
  // past election_timeout makes it a candidate and, being its own majority, a
  // leader in one step.
  if (single_node) {
    while (db->raft_.role() != Role::kLeader) db->raft_.Tick();
    s = db->Sync();  // Persist the term/vote bump from winning.
    if (!s.ok()) {
      delete db;
      return s;
    }
  }

  *out = db;
  return Status::OK();
}

ReplicatedDB::~ReplicatedDB() { delete storage_; }

Status ReplicatedDB::Put(const Slice& key, const Slice& value) {
  if (!is_leader()) return Status::InvalidArgument("not leader");
  raft_.Propose(EncodeCommand(kPut, key, value));
  return Sync();
}

Status ReplicatedDB::Delete(const Slice& key) {
  if (!is_leader()) return Status::InvalidArgument("not leader");
  raft_.Propose(EncodeCommand(kDelete, key, Slice()));
  return Sync();
}

Status ReplicatedDB::Get(const Slice& key, std::string* value) {
  auto it = table_.find(key.ToString());
  if (it == table_.end()) return Status::NotFound(key.ToString());
  value->assign(it->second);
  return Status::OK();
}

Status ReplicatedDB::Sync() {
  Status s = storage_->Save(raft_);  // Durable before visible.
  if (!s.ok()) return s;
  ApplyCommitted();
  return Status::OK();
}

void ReplicatedDB::ApplyCommitted() {
  for (uint64_t i = applied_index_ + 1; i <= raft_.commit_index(); ++i) {
    CommandType type;
    std::string key, value;
    if (DecodeCommand(raft_.entry_at(i).data, &type, &key, &value)) {
      if (type == kPut) {
        table_[key] = value;
      } else {
        table_.erase(key);
      }
    }
  }
  applied_index_ = raft_.commit_index();
}

}  // namespace raft
}  // namespace mlsm
