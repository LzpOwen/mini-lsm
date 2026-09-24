#include "mlsm/raft/raft.h"

#include <algorithm>

namespace mlsm {
namespace raft {

Raft::Raft(const Config& config) : config_(config) {
  // Index-0 sentinel so a 1-based Raft index i lives at log_[i] and TermAt(0)==0
  // makes prev_log_index==0 pass the consistency check (see raft.h).
  log_.push_back(LogEntry{/*term=*/0, /*index=*/0, /*data=*/""});
}

void Raft::Tick() {
  if (role_ == Role::kLeader) {
    if (++heartbeat_elapsed_ >= config_.heartbeat_timeout) {
      heartbeat_elapsed_ = 0;
      BroadcastAppendEntries();
    }
    return;
  }
  // Followers and candidates start a new election if they go too long without
  // hearing from a leader (or winning).
  if (++election_elapsed_ >= config_.election_timeout) {
    BecomeCandidate();
  }
}

void Raft::Step(const Message& msg) {
  // Term rules (paper 5.1): a larger term always wins and forces us to step
  // down; a smaller term is stale and gets rejected so the sender learns the
  // current term and steps down itself.
  if (msg.term > current_term_) {
    NodeId lead = (msg.type == MessageType::kAppendEntries) ? msg.from : 0;
    BecomeFollower(msg.term, lead);
  } else if (msg.term < current_term_) {
    Message resp;
    resp.from = config_.id;
    resp.to = msg.from;
    resp.term = current_term_;
    if (msg.type == MessageType::kRequestVote) {
      resp.type = MessageType::kRequestVoteResp;
      resp.vote_granted = false;
      Send(resp);
    } else if (msg.type == MessageType::kAppendEntries) {
      resp.type = MessageType::kAppendEntriesResp;
      resp.success = false;
      Send(resp);
    }
    return;
  }

  switch (role_) {
    case Role::kFollower:
      StepFollower(msg);
      break;
    case Role::kCandidate:
      StepCandidate(msg);
      break;
    case Role::kLeader:
      StepLeader(msg);
      break;
  }
}

void Raft::StepFollower(const Message& msg) {
  switch (msg.type) {
    case MessageType::kRequestVote:
      HandleRequestVote(msg);
      break;
    case MessageType::kAppendEntries:
      HandleAppendEntries(msg);
      break;
    default:
      break;  // Responses are not expected by a follower; ignore.
  }
}

void Raft::StepCandidate(const Message& msg) {
  switch (msg.type) {
    case MessageType::kRequestVoteResp:
      if (msg.vote_granted) {
        // Record a unique grant, then check for a majority.
        if (std::find(votes_granted_.begin(), votes_granted_.end(), msg.from) ==
            votes_granted_.end()) {
          votes_granted_.push_back(msg.from);
        }
        if (votes_granted_.size() >= QuorumSize()) {
          BecomeLeader();
        }
      }
      break;
    case MessageType::kAppendEntries:
      // Someone else won this term; step down and accept them as leader, then
      // process the entries as a follower would.
      BecomeFollower(msg.term, msg.from);
      HandleAppendEntries(msg);
      break;
    case MessageType::kRequestVote:
      // We already voted for ourselves this term, so this is rejected inside
      // HandleRequestVote (voted_for_ != msg.from).
      HandleRequestVote(msg);
      break;
    default:
      break;
  }
}

void Raft::StepLeader(const Message& msg) {
  switch (msg.type) {
    case MessageType::kRequestVote:
      // Same-term vote request: we already voted for ourselves, so reject.
      HandleRequestVote(msg);
      break;
    case MessageType::kAppendEntriesResp:
      HandleAppendEntriesResp(msg);
      break;
    default:
      break;
  }
}

void Raft::HandleRequestVote(const Message& msg) {
  // Grant the vote if we haven't voted for anyone else this term AND the
  // candidate's log is at least as up-to-date as ours (paper 5.4.1): compare by
  // last term first, then by length. This stops a node with a stale log from
  // winning and overwriting committed entries.
  const bool can_vote = (voted_for_ == 0 || voted_for_ == msg.from);
  const bool up_to_date =
      (msg.last_log_term > LastTerm()) ||
      (msg.last_log_term == LastTerm() && msg.last_log_index >= LastIndex());

  Message resp;
  resp.type = MessageType::kRequestVoteResp;
  resp.from = config_.id;
  resp.to = msg.from;
  resp.term = current_term_;
  resp.vote_granted = can_vote && up_to_date;
  if (resp.vote_granted) {
    voted_for_ = msg.from;
    hard_state_dirty_ = true;
    election_elapsed_ = 0;  // Granting a vote counts as hearing from a leader.
  }
  Send(resp);
}

void Raft::HandleAppendEntries(const Message& msg) {
  // A valid same-term AppendEntries comes from the current leader: accept it and
  // reset the election timer so we don't start a competing election.
  leader_ = msg.from;
  election_elapsed_ = 0;

  Message resp;
  resp.type = MessageType::kAppendEntriesResp;
  resp.from = config_.id;
  resp.to = msg.from;
  resp.term = current_term_;

  // Log consistency check (paper 5.3): reject unless our log contains an entry
  // at prev_log_index whose term matches. The leader then backs off and retries.
  if (msg.prev_log_index > LastIndex() ||
      TermAt(msg.prev_log_index) != msg.prev_log_term) {
    resp.success = false;
    Send(resp);
    return;
  }

  // Append the entries, overwriting any conflicting suffix. An existing entry
  // that disagrees on term (and everything after it) is truncated before we
  // append the leader's version (paper 5.3). Entries that already match are left
  // alone so we don't rewrite committed history.
  for (const LogEntry& e : msg.entries) {
    if (e.index <= LastIndex()) {
      if (TermAt(e.index) == e.term) continue;  // Already consistent.
      log_.resize(e.index);                      // Drop conflict and its suffix.
      // If the dropped suffix was already persisted, lower the watermark so the
      // caller knows to truncate its durable log too.
      if (stable_index_ >= e.index) stable_index_ = e.index - 1;
    }
    log_.push_back(e);
  }

  // Advance our commit index, but never past what we actually hold. Guard with
  // max so a reordered/stale heartbeat can't move it backwards.
  if (msg.leader_commit > commit_index_) {
    commit_index_ = std::min(msg.leader_commit, LastIndex());
  }

  resp.success = true;
  resp.match_index = msg.prev_log_index + msg.entries.size();
  Send(resp);
}

void Raft::HandleAppendEntriesResp(const Message& msg) {
  if (msg.success) {
    match_index_[msg.from] = msg.match_index;
    next_index_[msg.from] = msg.match_index + 1;
    MaybeCommit();
  } else {
    // Follower's log diverged before next_index_; step back one and retry on the
    // next broadcast. etcd/raft uses a conflict-term hint to jump back in one
    // step; this is the paper's plain decrement-by-one.
    uint64_t& next = next_index_[msg.from];
    if (next > 1) --next;
  }
}

void Raft::BecomeFollower(Term term, NodeId leader) {
  if (term > current_term_) {
    current_term_ = term;
    voted_for_ = 0;  // New term: vote is fresh.
    hard_state_dirty_ = true;
  }
  role_ = Role::kFollower;
  leader_ = leader;
  election_elapsed_ = 0;
}

void Raft::BecomeCandidate() {
  ++current_term_;
  voted_for_ = config_.id;  // Vote for ourselves.
  hard_state_dirty_ = true;
  role_ = Role::kCandidate;
  leader_ = 0;
  election_elapsed_ = 0;
  votes_granted_.assign(1, config_.id);

  // A single-node cluster is its own majority and wins immediately.
  if (votes_granted_.size() >= QuorumSize()) {
    BecomeLeader();
    return;
  }
  for (NodeId peer : config_.peers) {
    if (peer == config_.id) continue;
    Message msg;
    msg.type = MessageType::kRequestVote;
    msg.from = config_.id;
    msg.to = peer;
    msg.term = current_term_;
    msg.last_log_index = LastIndex();  // For the up-to-date check (5.4.1).
    msg.last_log_term = LastTerm();
    Send(msg);
  }
}

void Raft::BecomeLeader() {
  role_ = Role::kLeader;
  leader_ = config_.id;
  heartbeat_elapsed_ = 0;
  // Reset per-peer replication state: optimistically assume followers match our
  // log, and correct downward on the first rejected AppendEntries.
  next_index_.clear();
  match_index_.clear();
  for (NodeId peer : config_.peers) {
    if (peer == config_.id) continue;
    next_index_[peer] = LastIndex() + 1;
    match_index_[peer] = 0;
  }
  // Assert leadership right away with an empty AppendEntries (heartbeat).
  // etcd/raft additionally appends a no-op entry here to commit entries from
  // prior terms sooner (paper Figure 8 safety); omitted for simplicity.
  BroadcastAppendEntries();
}

void Raft::BroadcastAppendEntries() {
  for (NodeId peer : config_.peers) {
    if (peer == config_.id) continue;
    SendAppendEntries(peer);
  }
}

void Raft::SendAppendEntries(NodeId peer) {
  const uint64_t next = next_index_[peer];
  const uint64_t prev = next - 1;

  Message msg;
  msg.type = MessageType::kAppendEntries;
  msg.from = config_.id;
  msg.to = peer;
  msg.term = current_term_;
  msg.prev_log_index = prev;
  msg.prev_log_term = TermAt(prev);
  msg.leader_commit = commit_index_;
  for (uint64_t i = next; i <= LastIndex(); ++i) {
    msg.entries.push_back(log_[i]);  // Empty when the follower is caught up.
  }
  Send(msg);
}

void Raft::MaybeCommit() {
  // Find the highest index replicated on a majority whose term is the current
  // term, and commit up to it. Restricting to current-term entries is the
  // paper's 5.4.2 rule: a leader never commits an earlier term's entry by count
  // alone — it rides in once a current-term entry above it commits.
  for (uint64_t n = LastIndex(); n > commit_index_; --n) {
    if (TermAt(n) != current_term_) continue;
    size_t replicas = 1;  // Count ourselves.
    for (NodeId peer : config_.peers) {
      if (peer == config_.id) continue;
      if (match_index_[peer] >= n) ++replicas;
    }
    if (replicas >= QuorumSize()) {
      commit_index_ = n;
      break;
    }
  }
}

bool Raft::Propose(const std::string& data) {
  if (role_ != Role::kLeader) return false;
  log_.push_back(LogEntry{current_term_, LastIndex() + 1, data});
  // Replication to peers rides on the next broadcast. Try to commit right away:
  // a single-node cluster is its own majority and must advance immediately;
  // with peers MaybeCommit only counts this node, so it can't over-commit
  // before acks arrive (and 5.4.2's current-term rule still holds — the entry
  // was appended at current_term_).
  MaybeCommit();
  return true;
}

std::vector<LogEntry> Raft::TakeCommitted() {
  std::vector<LogEntry> out;
  for (uint64_t i = last_applied_ + 1; i <= commit_index_; ++i) {
    out.push_back(log_[i]);
  }
  last_applied_ = commit_index_;
  return out;
}

void Raft::Restore(Term term, NodeId vote,
                   const std::vector<LogEntry>& entries) {
  current_term_ = term;
  voted_for_ = vote;
  // log_ already holds just the index-0 sentinel from construction; append the
  // recovered entries after it so index i lands at log_[i].
  for (const LogEntry& e : entries) log_.push_back(e);
  stable_index_ = LastIndex();   // Everything recovered is already durable.
  hard_state_dirty_ = false;     // Nothing new to persist yet.
}

uint64_t Raft::LastIndex() const { return log_.size() - 1; }

Term Raft::LastTerm() const { return log_.back().term; }

Term Raft::TermAt(uint64_t index) const {
  if (index > LastIndex()) return 0;
  return log_[index].term;
}

size_t Raft::QuorumSize() const { return config_.peers.size() / 2 + 1; }

void Raft::Send(const Message& msg) { out_msgs_.push_back(msg); }

std::vector<Message> Raft::TakeMessages() {
  std::vector<Message> out;
  out.swap(out_msgs_);
  return out;
}

}  // namespace raft
}  // namespace mlsm
