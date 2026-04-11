// gloo/transport/tcp/peel/peel_tree.h

#pragma once

#include "peel_protocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gloo {
namespace transport {
namespace tcp {
namespace peel {

// =============================================================================
// Node  (internal topology graph node)
// =============================================================================

// Represents one physical node in the network: either a GPU/server (identified
// by its unicast IP string, e.g. "10.0.0.1") or a switch (identified by
// "switch_<num>", e.g. "switch_0").
struct Node {
    std::string        node_id;

    // Neighbouring nodes indexed by the local port they connect on.
    // connections[0] is the neighbour on port 0, connections[1] on port 1, etc.
    // Populated by PeelTree::loadTopology(); never modified after that.
    std::vector<Node*> connections;

    // Port indices (into connections[]) that are active in the MST.
    // Populated during the set-cover step in build().
    // To get the child node: connections[mst_children[i]].
    // Storing port numbers rather than raw pointers lets callers know exactly
    // which physical port to multicast on without a separate lookup.
    std::vector<int> mst_children;

    // True once this node has been claimed by a parent in the MST.
    // Used during set-cover to distinguish uncovered outer-layer nodes from
    // already-covered ones, avoiding duplicate parent assignments.
    bool               connected = false;

    // Depth in the rooted spanning tree. Assigned during buildHopLayers().
    int                layer_number = 0;
};

// =============================================================================
// PeelSubtree
// =============================================================================

// Describes one subtree of the multicast spanning tree.
// Each subtree maps to exactly one PeelTransport with its own multicast
// group, port range, and fabric routing MAC.
struct PeelSubtree {
    int              subtree_id = -1;

    // All ranks in this subtree (including the root/sender for this subtree).
    // Passed directly to PeelTransportConfig::participant_ranks.
    std::vector<int> receiver_ranks;

    // Ethernet destination MAC for this subtree's fabric CIDR ruleset.
    // Passed to PeelTransportConfig::cidr_rules_mac.
    uint8_t          cidr_rules_mac[6] = {};

    // Base UDP port for this subtree's channels.
    // Each rank in the subtree gets port base_port + rank.
    uint16_t         base_port = PEEL_DEFAULT_BASE_PORT;
};

// =============================================================================
// PeelTreeConfig
// =============================================================================

struct PeelTreeConfig {
    int rank       = 0;
    int world_size = 1;

    // Unicast IP for every rank, populated by PeelDiscovery before tree
    // construction. Key: rank, Value: dotted-decimal string (e.g. "10.0.0.1").
    std::unordered_map<int, std::string> peer_ips;

    // Base UDP port; each subtree derives its own port range from this.
    uint16_t base_port = PEEL_DEFAULT_BASE_PORT;

    // Optional path to a topology file (adjacency matrix, rack/switch map, etc.).
    // When empty, build() uses peer_ips alone (e.g. latency or prefix heuristics).
    std::string topology_file;
};

// =============================================================================
// PeelTree
// =============================================================================

// Builds a multicast spanning tree over all ranks and partitions it into
// subtrees, each of which is handed to one PeelTransport.
//
// Typical usage:
//
//   PeelTree tree(config);
//   tree.loadTopology(config.topology_file);   // optional
//   tree.build();
//   for (const PeelSubtree& sub : tree.subtrees()) {
//       // configure and create one PeelTransport per sub
//   }
class PeelTree {
public:
    explicit PeelTree(const PeelTreeConfig& config);
    ~PeelTree();

    PeelTree(const PeelTree&) = delete;
    PeelTree& operator=(const PeelTree&) = delete;

    // Load network topology from a file to guide tree construction.
    // Must be called before build(). No-op (returns true) when path is empty.
    bool loadTopology(const std::string& path);

    // Construct the spanning tree and partition it into subtrees.
    // Populates subtrees_ and my_subtree_id_.
    // Returns false if the tree cannot be built (e.g. missing peer IPs).
    bool build();

    // All subtrees produced by build(). One PeelTransport per entry.
    const std::vector<PeelSubtree>& subtrees() const { return subtrees_; }

    // The subtree this rank belongs to as a receiver.
    // Rank 0 (source) is added to every subtree's receiver_ranks during
    // partitionSubtrees(), so it will return 0 (the first subtree found).
    // peel_context.cc never calls this for rank 0 — it creates transports for
    // ALL subtrees when config_.rank == 0 regardless of this value.
    // Non-zero ranks appear in exactly one subtree and return that subtree's id.
    int mySubtreeId() const { return my_subtree_id_; }

private:
    // Returns true when node_id belongs to a switch ("switch_<num>").
    static bool isSwitch(const std::string& id);

    // One contiguous binary-prefix group produced by decompose().
    //   len  — number of fixed high bits (0 = wildcard all, bits_for_value = exact port)
    //   val  — the fixed bits in their natural position (e.g. for "1*" val=1, len=1)
    //   ports — physical port indices covered by this prefix
    struct PrefixGroup {
        int              len;
        int              val;
        std::vector<int> ports;
    };

    // Decomposes ports (all in [lo, hi)) into the minimum set of binary
    // prefix groups.  depth = log2(hi - lo) = remaining bit levels.
    // bits_for_value is passed through so base-case prefix lengths are correct.
    static std::vector<PrefixGroup> decompose(
        int lo, int hi, int depth, int bits_for_value,
        const std::vector<int>& ports);

    // BFS from source outward. Populates hop_layers_ and layer_number on each
    // node. Only participating GPU IPs are added to layers; non-participating
    // GPUs are skipped (they are leaf nodes so nothing is reachable through
    // them). Returns false if not all participating IPs are reachable.
    // Key: IP string, Value: rank number — allows O(1) IP→rank lookup during
    // subtree partitioning without a reverse scan of config_.peer_ips.
    bool buildHopLayers(Node* source,
                        const std::unordered_map<std::string, int>& participating_ips);

    // Greedy set-cover pass that prunes hop_layers_ into the MST.
    // Works from the outermost layer inward. At each layer L, switches are
    // selected to exclusively cover active nodes in layer L+1 — "exclusively"
    // meaning each outer-layer node is claimed by exactly one switch, giving
    // exactly one parent per node and guaranteeing a tree (not a DAG).
    // Populates Node::mst_children and Node::connected on kept nodes.
    // Returns false if any layer cannot be fully covered.
    bool pruneToMST();

    // Bottom-up subtree partitioning.  Called by build() after pruneToMST().
    //
    // For each switch layer (outermost → innermost) and each switch node:
    //   1. Group mst_children ports by the already-encoded outer-layer mac value
    //      coming from each child switch's records (outer-mac groups).
    //   2. Greedily absorb adjacent unassigned GPU ports into those groups,
    //      largest group first (flood-fill: |P-q|==1 for some q in group).
    //      Remaining unassigned GPU ports form their own mac=0 group.
    //   3. Binary-prefix decompose each group's final port set and encode the
    //      current layer's slot into the running mac value.
    //
    // Populates subtrees_ and my_subtree_id_.
    // Returns false on encoding or capacity errors.
    bool partitionSubtrees(
        const std::unordered_map<std::string, int>& participating_ips);

    PeelTreeConfig           config_;
    std::vector<PeelSubtree> subtrees_;
    int                      my_subtree_id_ = -1;

    // All nodes in the topology graph.  unique_ptr gives stable heap addresses
    // so that Node* pointers stored in connections[] remain valid even if the
    // map is rehashed.
    std::unordered_map<std::string, std::unique_ptr<Node>> nodes_;

    // Highest number of connections seen on any single switch.
    // Determines bits_for_value used in MAC encoding.
    int max_switch_ports_ = 0;

    // hop_layers_[l] holds every node assigned to hop distance l from source.
    // Populated by buildHopLayers(). Switches are always included; GPUs are
    // included only if they are a participating rank.
    std::vector<std::vector<Node*>> hop_layers_;
};

} // namespace peel
} // namespace tcp
} // namespace transport
} // namespace gloo
