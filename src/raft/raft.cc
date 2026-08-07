#include "mlsm/raft/raft.h"

#include <algorithm>

namespace mlsm {
namespace raft {

Raft::Raft(const Config& config) : config_(config) {}

void Raft::Tick() {
  if (role_ == Role::kLeader) {
    if (++heartbeat_elapsed_ >= config_.heartbeat_timeout) {
      heartbeat_elapsed_ = 0;
      BroadcastHeartbeat();
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
    NodeId lead = (msg.type == MessageType::kHeartbeat) ? msg.from : 0;
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
    } else if (msg.type == MessageType::kHeartbeat) {
      resp.type = MessageType::kHeartbeatResp;
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
    case MessageType::kHeartbeat: {
      // A heartbeat at our term comes from the current leader; accept it and
      // reset the election timer so we don't start a competing election.
      leader_ = msg.from;
      election_elapsed_ = 0;
      Message resp;
      resp.type = MessageType::kHeartbeatResp;
      resp.from = config_.id;
      resp.to = msg.from;
      resp.term = current_term_;
      resp.success = true;
      Send(resp);
      break;
    }
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
    case MessageType::kHeartbeat:
      // Someone else won this term; step down and accept them as leader.
      BecomeFollower(msg.term, msg.from);
      StepFollower(msg);  // Reply to the heartbeat as a follower would.
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
    default:
      break;  // Heartbeat responses are not tracked in this commit.
  }
}

void Raft::HandleRequestVote(const Message& msg) {
  // Grant the vote if we haven't voted for anyone else this term. The log
  // up-to-date check (paper 5.4.1) is added with the log-replication commit;
  // for now there is no log to compare.
  const bool can_vote = (voted_for_ == 0 || voted_for_ == msg.from);

  Message resp;
  resp.type = MessageType::kRequestVoteResp;
  resp.from = config_.id;
  resp.to = msg.from;
  resp.term = current_term_;
  resp.vote_granted = can_vote;
  if (can_vote) {
    voted_for_ = msg.from;
    hard_state_dirty_ = true;
    election_elapsed_ = 0;  // Granting a vote counts as hearing from a leader.
  }
  Send(resp);
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
    Send(msg);
  }
}

void Raft::BecomeLeader() {
  role_ = Role::kLeader;
  leader_ = config_.id;
  heartbeat_elapsed_ = 0;
  BroadcastHeartbeat();  // Assert leadership right away.
}

void Raft::BroadcastHeartbeat() {
  for (NodeId peer : config_.peers) {
    if (peer == config_.id) continue;
    Message msg;
    msg.type = MessageType::kHeartbeat;
    msg.from = config_.id;
    msg.to = peer;
    msg.term = current_term_;
    Send(msg);
  }
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
