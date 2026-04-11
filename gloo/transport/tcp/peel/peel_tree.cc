// gloo/transport/tcp/peel/peel_tree.cc

#include "peel_tree.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

PeelTree::PeelTree(const PeelTreeConfig& config)
    : config_(config) {}

PeelTree::~PeelTree() = default;

// static
bool PeelTree::isSwitch(const std::string& id) {
    return id.size() > 7 && id.compare(0, 7, "switch_") == 0;
}

bool PeelTree::loadTopology(const std::string& path) {
    if (path.empty()) return true;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "peel_tree[" << config_.rank
                  << "]: cannot open topology file: " << path << "\n";
        return false;
    }

    // Helper: return existing node or create a new one.
    // unique_ptr ownership stays in nodes_; the raw Node* is stable on the heap.
    auto getOrCreate = [&](const std::string& id) -> Node* {
        auto it = nodes_.find(id);
        if (it != nodes_.end()) return it->second.get();
        auto node      = std::make_unique<Node>();
        node->node_id  = id;
        Node* ptr      = node.get();
        nodes_[id]     = std::move(node);
        return ptr;
    };

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string curr_id;
        if (!(ss >> curr_id)) continue;

        Node* curr = getOrCreate(curr_id);

        // Each subsequent token is a neighbour; its position is the port index
        // on curr_id that the connection arrives on.
        std::string neighbour_id;
        while (ss >> neighbour_id) {
            Node* neighbour = getOrCreate(neighbour_id);
            curr->connections.push_back(neighbour);
        }

        if (isSwitch(curr_id)) {
            // We now know how many ports this switch uses; update the running max.
            int port_count = static_cast<int>(curr->connections.size());
            if (port_count > max_switch_ports_)
                max_switch_ports_ = port_count;
        }
    }

    std::cout << "peel_tree[" << config_.rank << "]: loaded " << nodes_.size()
              << " node(s), max switch ports=" << max_switch_ports_ << "\n";
    return true;
}

bool PeelTree::build() {
    // Root of the collective tree is always rank 0.
    auto src_it = config_.peer_ips.find(0);
    if (src_it == config_.peer_ips.end()) {
        std::cerr << "peel_tree[" << config_.rank << "]: rank 0 IP not in peer_ips\n";
        return false;
    }
    const std::string& source_ip = src_it->second;

    auto node_it = nodes_.find(source_ip);
    if (node_it == nodes_.end()) {
        std::cerr << "peel_tree[" << config_.rank
                  << "]: source IP " << source_ip << " not found in topology\n";
        return false;
    }
    Node* source = node_it->second.get();

    // TODO: if build() ever needs to be called more than once on the same
    // PeelTree instance (e.g. topology change / failover), reset output state
    // and per-node MST fields here before proceeding:
    //   subtrees_.clear();  my_subtree_id_ = -1;
    //   for (auto& [id, node] : nodes_) { node->mst_children.clear(); node->connected = false; }

    // Build IP→rank map for all participating ranks.
    // Used for O(1) membership checks and O(1) IP→rank lookup during subtree
    // partitioning (avoids a reverse scan of config_.peer_ips per node).
    std::unordered_map<std::string, int> participating_ips;
    for (const auto& [rank, ip] : config_.peer_ips)
        participating_ips[ip] = rank;

    if (!buildHopLayers(source, participating_ips))
        return false;

    if (!pruneToMST())
        return false;

    if (!partitionSubtrees(participating_ips))
        return false;

    return true;
}

bool PeelTree::buildHopLayers(Node* source,
                               const std::unordered_map<std::string, int>& participating_ips) {
    hop_layers_.clear();

    const int total_gpus = static_cast<int>(participating_ips.size());
    int covered_gpus     = 0;

    std::unordered_set<Node*> visited;

    // --- Layer 0: source ---------------------------------------------------
    source->layer_number = 0;
    hop_layers_.push_back({source});
    visited.insert(source);

    // Source is always rank 0, which is always a participating GPU.
    ++covered_gpus;

    // --- BFS: one iteration = one hop layer --------------------------------
    // current_layer holds the nodes whose neighbours we expand next.
    // Switches are always expanded; non-participating GPUs are leaf nodes
    // and are never added to the frontier.
    std::vector<Node*> current_layer = {source};

    while (covered_gpus < total_gpus && !current_layer.empty()) {
        const int next_idx = static_cast<int>(hop_layers_.size());
        std::vector<Node*> next_layer;   // nodes that will expand in the next hop
        std::vector<Node*> next_nodes;   // all nodes added to hop_layers_[next_idx]

        for (Node* curr : current_layer) {
            for (Node* nb : curr->connections) {
                if (visited.count(nb)) continue;
                visited.insert(nb);
                nb->layer_number = next_idx;

                if (isSwitch(nb->node_id)) {
                    // Switches: always include in the layer and always expand.
                    next_nodes.push_back(nb);
                    next_layer.push_back(nb);
                } else {
                    // GPU/server: include only if it participates in the collective.
                    if (participating_ips.count(nb->node_id)) {
                        next_nodes.push_back(nb);
                        ++covered_gpus;
                    }
                    // Non-participating GPUs are leaf nodes — nothing reachable
                    // through them, so they are silently skipped.
                }
            }
        }

        if (!next_nodes.empty())
            hop_layers_.push_back(std::move(next_nodes));

        current_layer = std::move(next_layer);
    }

    if (covered_gpus < total_gpus) {
        std::cerr << "peel_tree[" << config_.rank << "]: only reached "
                  << covered_gpus << "/" << total_gpus
                  << " participating GPU(s) — topology may be incomplete\n";
        return false;
    }

    std::cout << "peel_tree[" << config_.rank << "]: " << hop_layers_.size()
              << " hop layer(s), " << covered_gpus << " GPU(s) covered\n";
    return true;
}

bool PeelTree::pruneToMST() {
    if (hop_layers_.empty()) return false;

    Node* source = hop_layers_[0][0];

    // The last hop layer may contain switches that were discovered in the same
    // BFS round as the furthest GPU but have no participating descendants.
    // Remove them now — no switch in the last layer can be part of the MST.
    {
        auto& last = hop_layers_.back();
        last.erase(
            std::remove_if(last.begin(), last.end(),
                           [](Node* n) { return isSwitch(n->node_id); }),
            last.end());
    }

    // Process layers from second-to-last down to layer 1.
    // At each layer L we pick switches to cover nodes in layer L+1.
    // hop_layers_[L+1] already contains exactly the right set:
    //   - switches: only those kept by the previous iteration's pruning
    //   - GPUs: only participating ones (non-participating excluded in BFS)
    // No separate tracking set needed.
    for (int L = static_cast<int>(hop_layers_.size()) - 2; L >= 1; --L) {

        // Every node in the outer layer needs a parent from this layer.
        std::unordered_set<Node*> to_cover(
            hop_layers_[L + 1].begin(), hop_layers_[L + 1].end());
        if (to_cover.empty()) continue;

        // Candidate switches in layer L (GPUs in this layer are destinations,
        // not forwarders, so they are never candidates).
        std::vector<Node*> candidates;
        for (Node* n : hop_layers_[L]) {
            if (isSwitch(n->node_id))
                candidates.push_back(n);
        }

        // Greedy set-cover: repeatedly pick the switch with the most connections
        // to still-uncovered nodes, re-evaluating after every pick.
        while (!to_cover.empty()) {
            Node* best       = nullptr;
            int   best_count = 0;

            for (Node* sw : candidates) {
                int count = 0;
                for (Node* nb : sw->connections)
                    if (to_cover.count(nb)) ++count;
                if (count > best_count) {
                    best_count = count;
                    best       = sw;
                }
            }

            if (!best || best_count == 0) {
                std::cerr << "peel_tree[" << config_.rank
                          << "]: cannot cover all active nodes at layer "
                          << (L + 1) << " — topology gap\n";
                return false;
            }

            // Claim every uncovered node this switch connects to exclusively.
            // Iterate by port index so we store the port number in mst_children
            // rather than the raw pointer — callers can recover the child node
            // via connections[port] while also knowing which port to use.
            // to_cover.erase() returns 1 if uncovered, 0 if already claimed.
            for (int port = 0; port < static_cast<int>(best->connections.size()); ++port) {
                Node* nb = best->connections[port];
                if (to_cover.erase(nb)) {
                    nb->connected = true;
                    best->mst_children.push_back(port);
                }
            }

            // Remove from candidates so it is not evaluated again.
            candidates.erase(
                std::remove(candidates.begin(), candidates.end(), best),
                candidates.end());
        }

        // Prune layer L: discard switches that were never selected.
        // A switch was selected iff the greedy loop populated its mst_children.
        // GPUs in this layer are always kept (they are destinations).
        std::vector<Node*> pruned;
        for (Node* n : hop_layers_[L]) {
            if (!isSwitch(n->node_id) || !n->mst_children.empty())
                pruned.push_back(n);
        }
        hop_layers_[L] = std::move(pruned);
    }

    // Source is at layer 0 and is not a switch, so it falls outside the loop.
    // hop_layers_[1] has already been pruned — every node remaining is an MST
    // member. Walk source's connections by port and claim those that are in
    // layer 1, storing the port number in mst_children.
    if (hop_layers_.size() > 1) {
        const std::unordered_set<Node*> layer1(
            hop_layers_[1].begin(), hop_layers_[1].end());
        for (int port = 0; port < static_cast<int>(source->connections.size()); ++port) {
            Node* nb = source->connections[port];
            if (layer1.count(nb)) {
                nb->connected = true;
                source->mst_children.push_back(port);
            }
        }
    }

    std::cout << "peel_tree[" << config_.rank << "]: MST built across "
              << hop_layers_.size() << " layer(s), source has "
              << source->mst_children.size() << " direct child(ren)\n";
    return true;
}

// static
std::vector<PeelTree::PrefixGroup> PeelTree::decompose(
    int lo, int hi, int depth, int bits_for_value,
    const std::vector<int>& ports) {
    if (ports.empty()) return {};
    if (static_cast<int>(ports.size()) == hi - lo) {
        // Every port in this aligned block is present — one prefix covers all.
        // prefix_len = bits_for_value - depth  (how many bits are fixed)
        // prefix_val = lo >> depth             (the fixed bits, dropping variable tail)
        return {{ bits_for_value - depth, lo >> depth, ports }};
    }
    const int mid = (lo + hi) / 2;
    std::vector<int> left, right;
    for (int p : ports) (p < mid ? left : right).push_back(p);
    auto result = decompose(lo, mid, depth - 1, bits_for_value, left);
    auto rhs    = decompose(mid, hi, depth - 1, bits_for_value, right);
    result.insert(result.end(), rhs.begin(), rhs.end());
    return result;
}

bool PeelTree::partitionSubtrees(
    const std::unordered_map<std::string, int>& participating_ips) {

    // ── Step 1: slot parameters ───────────────────────────────────────────────
    if (max_switch_ports_ < 2) {
        std::cerr << "peel_tree[" << config_.rank
                  << "]: max_switch_ports_ too small (" << max_switch_ports_ << ")\n";
        return false;
    }

    // bits_for_value = ceil(log2(max_switch_ports_))
    // Number of bits required to index any port on the widest switch.
    // Works for non-power-of-2 port counts: e.g. 6 ports → 3 bits (covers 0-7).
    int bits_for_value = 0;
    for (int p = 1; p < max_switch_ports_; p <<= 1) ++bits_for_value;

    // port_universe = 2^bits_for_value — the power-of-2 port space used by
    // decompose().  Ports >= max_switch_ports_ are phantom and never appear.
    const int port_universe = 1 << bits_for_value;

    // bits_for_length = ceil(log2(bits_for_value + 1))
    int bits_for_length = 0;
    for (int p = 1; p < bits_for_value + 1; p <<= 1) ++bits_for_length;

    const int bits_per_switch = bits_for_value + bits_for_length;

    int slot_size = 0;
    if      (bits_per_switch <= 8)  slot_size = 8;
    else if (bits_per_switch <= 16) slot_size = 16;
    else {
        std::cerr << "peel_tree[" << config_.rank << "]: " << bits_per_switch
                  << " bits/switch exceeds 16-bit slot limit\n";
        return false;
    }

    // ── Step 2: assign a slot index to every switch-containing layer ──────────
    // Layer 1 (innermost switch layer) → slot 0 → MAC MSBs.
    std::unordered_map<int, int> layer_to_slot; // layer number → 0-based slot index
    int num_switch_layers = 0;
    for (int L = 1; L < static_cast<int>(hop_layers_.size()); ++L) {
        for (Node* n : hop_layers_[L]) {
            if (isSwitch(n->node_id)) {
                layer_to_slot[L] = num_switch_layers++;
                break;
            }
        }
    }

    if (num_switch_layers * slot_size > 48) {
        std::cerr << "peel_tree[" << config_.rank << "]: " << num_switch_layers
                  << " switch layers × " << slot_size << " bits = "
                  << (num_switch_layers * slot_size)
                  << " — exceeds 48-bit MAC capacity\n";
        return false;
    }

    // ── Step 3: bottom-up record construction ────────────────────────────────
    // A Record is a partially-built subtree: the mac value accumulates one slot
    // per switch layer as we move inward toward the source.
    struct Record { uint64_t mac; std::vector<int> ranks; };
    std::unordered_map<Node*, std::vector<Record>> node_records;

    // Process from outermost switch layer to innermost (layer 1).
    for (int L = static_cast<int>(hop_layers_.size()) - 2; L >= 1; --L) {
        if (!layer_to_slot.count(L)) continue; // GPU-only layer — nothing to encode
        const int slot_idx = layer_to_slot[L];

        for (Node* sw : hop_layers_[L]) {
            if (!isSwitch(sw->node_id)) continue;

            // port_ranks_by_mac[mac][port] = ranks reachable via that port with mac
            std::unordered_map<uint64_t,
                std::unordered_map<int, std::vector<int>>> port_ranks_by_mac;

            // mac_groups[mac] = ports of sw that feed records with this mac value
            std::unordered_map<uint64_t, std::vector<int>> mac_groups;

            std::vector<int> gpu_ports;

            // 4a. Separate mst_children into switch-child mac groups and GPU ports.
            for (int port : sw->mst_children) {
                Node* child = sw->connections[port];
                if (isSwitch(child->node_id)) {
                    for (const Record& r : node_records[child]) {
                        mac_groups[r.mac].push_back(port);
                        auto& v = port_ranks_by_mac[r.mac][port];
                        v.insert(v.end(), r.ranks.begin(), r.ranks.end());
                    }
                } else {
                    gpu_ports.push_back(port);
                }
            }

            // 4b. Greedily absorb adjacent GPU ports into switch-child mac groups.
            //     Sort groups largest-first so the biggest group gets first pick.
            std::vector<uint64_t> sorted_macs;
            sorted_macs.reserve(mac_groups.size());
            for (auto& [mac, _] : mac_groups) sorted_macs.push_back(mac);
            std::sort(sorted_macs.begin(), sorted_macs.end(),
                      [&](uint64_t a, uint64_t b) {
                          return mac_groups[a].size() > mac_groups[b].size();
                      });

            std::unordered_set<int> unassigned(gpu_ports.begin(), gpu_ports.end());

            for (uint64_t mac : sorted_macs) {
                auto& port_set = mac_groups[mac];
                bool changed   = true;
                while (changed && !unassigned.empty()) {
                    changed = false;
                    for (auto it = unassigned.begin(); it != unassigned.end(); ++it) {
                        const int P = *it;
                        // Adjacent: differs by exactly 1 from any port already in group.
                        bool adj = false;
                        for (int q : port_set) {
                            if (std::abs(P - q) == 1) { adj = true; break; }
                        }
                        if (adj) {
                            port_set.push_back(P);
                            const int rank =
                                participating_ips.at(sw->connections[P]->node_id);
                            port_ranks_by_mac[mac][P] = { rank };
                            unassigned.erase(it);
                            changed = true;
                            break; // port_set grew — restart adjacency scan
                        }
                    }
                }
            }

            // Remaining unassigned GPU ports → own group with mac = 0.
            if (!unassigned.empty()) {
                for (int P : unassigned) {
                    mac_groups[0].push_back(P);
                    const int rank =
                        participating_ips.at(sw->connections[P]->node_id);
                    port_ranks_by_mac[0][P] = { rank };
                }
            }

            // 4c. Binary-prefix decompose each group's port set; encode slot; emit.
            for (auto& [mac, port_vec] : mac_groups) {
                std::sort(port_vec.begin(), port_vec.end());

                for (const PrefixGroup& g :
                         decompose(0, port_universe, bits_for_value, bits_for_value, port_vec)) {

                    // Build the fixed-width prefix ID:
                    //   upper bits_for_length bits = prefix length
                    //   lower bits_for_value bits  = prefix value, right-padded with 0s
                    const int padded_val = g.val << (bits_for_value - g.len);
                    const int prefix_id  = (g.len << bits_for_value) | padded_val;

                    // Place prefix ID in the upper bits of an 8/16-bit slot;
                    // lower (slot_size - bits_per_switch) bits stay zero.
                    const int slot_val = prefix_id << (slot_size - bits_per_switch);

                    // Layer 1 (slot 0) → MAC bits [47..40]; each slot shifts right.
                    const int      shift   = 48 - (slot_idx + 1) * slot_size;
                    const uint64_t new_mac = mac |
                        (static_cast<uint64_t>(slot_val) << shift);

                    // Collect all ranks reachable through the covered ports.
                    std::vector<int> new_ranks;
                    for (int P : g.ports) {
                        const auto& r = port_ranks_by_mac[mac][P];
                        new_ranks.insert(new_ranks.end(), r.begin(), r.end());
                    }

                    node_records[sw].push_back({ new_mac, std::move(new_ranks) });
                }
            }
        }
    }

    // ── Step 5: emit final subtrees from source's layer-1 children ───────────
    Node* source = hop_layers_[0][0];

    for (int port : source->mst_children) {
        Node* child = source->connections[port];

        if (!isSwitch(child->node_id)) {
            // GPU directly connected to source (no switch between them).
            // The bottom-up traversal only creates records for switch nodes, so
            // this GPU will not appear in any subtree.  This is a known
            // limitation of the current encoding: topologies that mix direct
            // GPU links and switch links at layer 1 are not supported.
            // In practice, use the flat (no-topology) fallback for such setups.
            std::cerr << "peel_tree[" << config_.rank
                      << "]: WARNING: GPU " << child->node_id
                      << " is directly connected to source at layer 1 — "
                         "no subtree can be created for it (unsupported topology). "
                         "Use flat mode (no topology file) instead.\n";
            continue;
        }

        for (const Record& r : node_records[child]) {
            PeelSubtree sub;
            sub.subtree_id     = static_cast<int>(subtrees_.size());
            sub.receiver_ranks = r.ranks;

            // Rank 0 (source) sends on every subtree but is never added to any
            // record during the bottom-up traversal (it sits above all switches).
            // Add it explicitly so PeelFullMesh includes it in every mesh.
            sub.receiver_ranks.push_back(0);

            // Each subtree occupies world_size ports starting from its own
            // offset so that no two subtrees share a port number on any rank.
            sub.base_port = static_cast<uint16_t>(
                config_.base_port + sub.subtree_id * config_.world_size);

            // Pack the 48-bit mac (stored in low 48 bits of uint64_t) into
            // cidr_rules_mac[6] in big-endian order (byte 0 = MSB).
            for (int i = 0; i < 6; ++i)
                sub.cidr_rules_mac[i] =
                    static_cast<uint8_t>((r.mac >> (40 - 8 * i)) & 0xFF);

            subtrees_.push_back(std::move(sub));
        }
    }

    // ── Step 6: identify which subtree this rank belongs to ──────────────────
    my_subtree_id_ = -1;
    for (const PeelSubtree& sub : subtrees_) {
        for (int rank : sub.receiver_ranks) {
            if (rank == config_.rank) {
                my_subtree_id_ = sub.subtree_id;
                break;
            }
        }
        if (my_subtree_id_ != -1) break;
    }

    std::cout << "peel_tree[" << config_.rank << "]: " << subtrees_.size()
              << " subtree(s) produced, this rank is in subtree "
              << my_subtree_id_ << "\n";
    return true;
}

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
