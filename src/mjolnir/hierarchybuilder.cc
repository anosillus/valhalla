#include "mjolnir/hierarchybuilder.h"
#include "valhalla/mjolnir/graphtilebuilder.h"

#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <boost/property_tree/ptree.hpp>

#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/encoded.h>
#include <valhalla/midgard/sequence.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphconstants.h>
#include <valhalla/baldr/graphtile.h>
#include <valhalla/baldr/graphreader.h>

#include <boost/format.hpp>
#include <ostream>
#include <set>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::mjolnir;

namespace {

// Sequence file name (as bin so it gets cleaned up)
std::string nodes_file = std::string("new_nodes_to_old_nodes.bin");

// Form tiles in the new level.
void FormTilesInNewLevel(GraphReader& reader,
         const std::unordered_map<GraphId, std::tuple<GraphId, GraphId, GraphId>>& old_to_new) {
  // Use the sequence that associate new nodes to old nodes
  sequence<std::pair<GraphId, GraphId>> new_to_old(nodes_file, false);

  // Sort the new nodes. Sort so highway level is first
  new_to_old.sort(
    [](const std::pair<GraphId, GraphId>& a, const std::pair<GraphId, GraphId>& b){
      if (a.first.level() == b.first.level()) {
        if (a.first.tileid() == b.first.tileid()) {
          return a.first.id() < b.first.id();
        }
        return a.first.tileid() < b.first.tileid();
      }
      return a.first.level() < b.first.level();
    }
  );

  // Hierarchy level information
  const auto& tile_hierarchy = reader.GetTileHierarchy();
  auto level = tile_hierarchy.levels().rbegin();
  auto& base_level = level->second;
  level++;
  auto& arterial_level = level->second;
  level++;
  auto& highway_level = level->second;
  RoadClass highway_class  = highway_level.importance;
  RoadClass arterial_class = arterial_level.importance;
  RoadClass local_class  = base_level.importance;

  // Iterate through the new nodes. They have been sorted by level so that
  // highway level is done first.
  reader.Clear();
  bool added = false;
  uint8_t current_level;
  GraphId tile_id;
  GraphId nodeb;
  GraphTileBuilder* tilebuilder = nullptr;
  for (auto new_node = new_to_old.begin(); new_node != new_to_old.end(); new_node++) {
    // Get the node - check if a new tile
    GraphId nodea = (*new_node).first;
    if (nodea.Tile_Base() != tile_id) {
      // Store the prior tile
      if (tilebuilder != nullptr) {
        tilebuilder->StoreTileData();
        delete tilebuilder;
      }

      // New tilebuilder for the next tile. Update current level.
      tile_id = nodea.Tile_Base();
      tilebuilder = new GraphTileBuilder(tile_hierarchy, tile_id, false);
      current_level = nodea.level();

      // Create a dummy admin at index 0. Used if admins are not used/created.
      tilebuilder->AddAdmin("None", "None", "", "");

      // Check if we need to clear the base/local tile cache
      if (reader.OverCommitted()) {
        reader.Clear();
      }
    }

    // Get the node in the base level
    GraphId base_node = (*new_node).second;
    const GraphTile* tile = reader.GetGraphTile(base_node);
    if (tile == nullptr) {
      LOG_ERROR("Base tile is null? ");
      continue;
    }

    // Copy node information
    NodeInfo baseni = *(tile->node(base_node.id()));
    tilebuilder->nodes().push_back(baseni);
    const auto& admin = tile->admininfo(baseni.admin_index());
    NodeInfo& node = tilebuilder->nodes().back();
    node.set_edge_index(tilebuilder->directededges().size());
    node.set_timezone(baseni.timezone());
    node.set_admin_index(tilebuilder->AddAdmin(admin.country_text(), admin.state_text(),
                                               admin.country_iso(), admin.state_iso()));

    // Current edge count
    size_t edge_count = tilebuilder->directededges().size();

    // Iterate through directed edges of the base node to get remaining
    // directed edges (based on classification/importance cutoff)
    RoadClass best_rc = RoadClass::kServiceOther;
    GraphId nodeb;
    GraphId base_edge_id(base_node.tileid(), base_node.level(), baseni.edge_index());
    for (uint32_t i = 0; i < baseni.edge_count(); i++, base_edge_id++) {
      // Store the directed edge if on the current level
      const DirectedEdge* directededge = tile->directededge(base_edge_id);
      if (tile_hierarchy.get_level(directededge->classification()) == current_level) {
        // Copy the directed edge information
        DirectedEdge newedge = *directededge;

        // Set the end node for this edge.
        auto f = old_to_new.find(directededge->endnode());
        if (current_level == 0) {
          nodeb = std::get<0>(f->second);
        } else if (current_level == 1) {
          nodeb = std::get<1>(f->second);
        } else {
          nodeb = std::get<2>(f->second);
        }
        if (!nodeb.Is_Valid()) {
          LOG_ERROR("Invalid end node - not found in old_to_new map");
        }
        newedge.set_endnode(nodeb);

        // Set opposing edge indexes to 0 (gets set in graph validator).
        newedge.set_opp_index(0);

        // Get signs from the base directed edge
        if (directededge->exitsign()) {
          std::vector<SignInfo> signs = tile->GetSigns(base_edge_id.id());
          if (signs.size() == 0) {
            LOG_ERROR("Base edge should have signs, but none found");
          }
          tilebuilder->AddSigns(tilebuilder->directededges().size(), signs);
        }

        // Get access restrictions from the base directed edge. Add these to
        // the list of access restrictions in the new tile. Update the
        // edge index in the restriction to be the current directed edge Id
        if (directededge->access_restriction()) {
          auto restrictions = tile->GetAccessRestrictions(base_edge_id.id(), kAllAccess);
          for (const auto& res : restrictions) {
            tilebuilder->AddAccessRestriction(
                AccessRestriction(tilebuilder->directededges().size(),
                   res.type(), res.modes(), res.days_of_week(), res.value()));
          }
        }

        // Get edge info, shape, and names from the old tile and add to the
        // new. Use the current edge info offset as the "index" to properly
        // create edge pairs in the same tile.
        uint32_t idx = directededge->edgeinfo_offset();
        auto edgeinfo = tile->edgeinfo(idx);
        uint32_t edge_info_offset = tilebuilder->AddEdgeInfo(idx,
                           nodea, nodeb, edgeinfo.wayid(), edgeinfo.shape(),
                           tile->GetNames(idx), added);
        newedge.set_edgeinfo_offset(edge_info_offset);

        // Update best road class at this node.
        if (directededge->classification() < best_rc) {
          best_rc = directededge->classification();
        }

        // Add directed edge
        tilebuilder->directededges().emplace_back(std::move(newedge));
      }
    }

    // Update the best road class at this node
    node.set_bestrc(best_rc);

    // Add transition edges
    auto f = old_to_new.find(base_node);

    // Downward transition edge
    GraphId downward_end_node;
    if (current_level == 0) {
      downward_end_node = std::get<1>(f->second);
      if (!downward_end_node.Is_Valid()) {
        downward_end_node = std::get<2>(f->second);
      }
    } else if (current_level == 1) {
      downward_end_node = std::get<2>(f->second);
    }
    if (downward_end_node.Is_Valid()) {
      DirectedEdge downwardedge;
      downwardedge.set_endnode(downward_end_node);
      downwardedge.set_trans_down(true);
      downwardedge.set_all_forward_access();
      tilebuilder->directededges().emplace_back(std::move(downwardedge));
    }

    // Upward transition edges
    GraphId upward_end_node;
    if (current_level == 2) {
      upward_end_node = std::get<1>(f->second);
      if (!upward_end_node.Is_Valid()) {
        upward_end_node = std::get<0>(f->second);
      }
    } else if (current_level == 1) {
      upward_end_node = std::get<0>(f->second);
    }
    if (upward_end_node.Is_Valid()) {
      DirectedEdge upwardedge;
      upwardedge.set_endnode(upward_end_node);
      upwardedge.set_trans_up(true);
      upwardedge.set_all_forward_access();
      tilebuilder->directededges().emplace_back(std::move(upwardedge));
    }

    // Set the edge count for the new node
    node.set_edge_count(tilebuilder->directededges().size() - edge_count);
  }

  // Delete the tile builder
  if (tilebuilder != nullptr) {
    delete tilebuilder;
  }
}

/**
 * Create node associations between "new" nodes placed into respective
 * hierarchy levels and the existing nodes on the base/local level. The
 * associations go both ways: from the "old" nodes on the base/local level
 * to new nodes (using a mapping in memory) and from new nodes to old nodes
 * using a sequence (file).
 */
void CreateNodeAssociations(GraphReader& reader,
     std::unordered_map<GraphId, std::tuple<GraphId, GraphId, GraphId>>& old_to_new) {
  // Map of tiles vs. count of nodes. Used to construct new node Ids.
  std::unordered_map<GraphId, uint32_t> new_nodes;

  // lambda to get the next node Id given a tile
  auto get_new_node = [&new_nodes](const GraphId& tile) {
    auto itr = new_nodes.find(tile);
    if (itr == new_nodes.end()) {
      GraphId new_node(tile.tileid(), tile.level(), 0);
      new_nodes[tile] = 1;
      return new_node;
    } else {
      GraphId new_node(tile.tileid(), tile.level(), itr->second);
      itr->second++;
      return new_node;
    }
  };

  // Create a sequence to associate new nodes to old nodes
  sequence<std::pair<GraphId, GraphId>> new_to_old(nodes_file, true);

  // Hierarchy level information
  const auto& tile_hierarchy = reader.GetTileHierarchy();
  auto tile_level = tile_hierarchy.levels().rbegin();
  auto& base_level = tile_level->second;
  tile_level++;
  auto& arterial_level = tile_level->second;
  tile_level++;
  auto& highway_level = tile_level->second;

  // Iterate through all tiles in the local level
  uint32_t ntiles = base_level.tiles.TileCount();
  uint32_t bl = static_cast<uint32_t>(base_level.level);
  uint32_t al = static_cast<uint32_t>(arterial_level.level);
  uint32_t hl = static_cast<uint32_t>(highway_level.level);
  for (uint32_t basetileid = 0; basetileid < ntiles; basetileid++) {
    // Get the graph tile. Skip if no tile exists (common case)
    const GraphTile* tile = reader.GetGraphTile(GraphId(basetileid, bl, 0));
    if (tile == nullptr || tile->header()->nodecount() == 0) {
      continue;
    }

    // Iterate through the nodes. Add nodes to the new level when
    // best road class <= the new level classification cutoff
    bool levels[3];
    uint32_t nodecount = tile->header()->nodecount();
    GraphId basenode(basetileid, bl, 0);
    GraphId edgeid(basetileid, bl, 0);
    const NodeInfo* nodeinfo = tile->node(basenode);
    for (uint32_t i = 0; i < nodecount; i++, nodeinfo++, basenode++) {
      // Iterate through the edges to see which levels this node exists.
      levels[0] = levels[1] = levels[2] = false;
      for (uint32_t j = 0; j < nodeinfo->edge_count(); j++, edgeid++) {
        // Skip transit connection edges
        const DirectedEdge* directededge = tile->directededge(edgeid);
        if (directededge->use() == Use::kTransitConnection) {
          continue;
        }

        // Update the flag for the level of this edge
        levels[tile_hierarchy.get_level(directededge->classification())] = true;
      }

      // Associate new nodes to base nodes and base node to new nodes
      GraphId highway_node, arterial_node, local_node;
      if (levels[0]) {
        // New node is on the highway level. Associate back to base/local node
        GraphId new_tile(highway_level.tiles.TileId(nodeinfo->latlng()), hl, 0);
        highway_node = get_new_node(new_tile);
        new_to_old.push_back(std::make_pair(highway_node, basenode));
      }
      if (levels[1]) {
        // New node is on the arterial level. Associate back to base/local node
        GraphId new_tile(arterial_level.tiles.TileId(nodeinfo->latlng()), al, 0);
        arterial_node = get_new_node(new_tile);
        new_to_old.push_back(std::make_pair(arterial_node, basenode));
      }
      if (levels[2]) {
        // New node is on the local level. Associate back to base/local node
        GraphId new_tile(basetileid, bl, 0);
        local_node = get_new_node(new_tile);
        new_to_old.push_back(std::make_pair(local_node, basenode));
      }

      // Associate the old node to the new node(s). Entries in the tuple
      // that are invalid nodes indicate no node exists in the new level.
      old_to_new[basenode] = std::make_tuple(highway_node, arterial_node, local_node);
    }

    // Check if we need to clear the tile cache
    if(reader.OverCommitted()) {
      reader.Clear();
    }
  }
}

}

namespace valhalla {
namespace mjolnir {

// Build successive levels of the hierarchy, starting at the local
// base level. Each successive level of the hierarchy is based on
// and connected to the next.
void HierarchyBuilder::Build(const boost::property_tree::ptree& pt) {

  // TODO: thread this. Might be more possible now that we don't create
  // shortcuts in the HierarchyBuilder

  // Construct GraphReader
  LOG_INFO("HierarchyBuilder");
  GraphReader reader(pt.get_child("mjolnir"));
  const auto& tile_hierarchy = reader.GetTileHierarchy();

  // Association of old nodes to new nodes
  std::unordered_map<GraphId, std::tuple<GraphId, GraphId, GraphId>> old_to_new;
  CreateNodeAssociations(reader, old_to_new);

  // Iterate through the hierarchy (from highway down to local) and build
  // new tiles
  FormTilesInNewLevel(reader, old_to_new);

  // Store an empty tile for any base tiles that no longer have any data
  // (nodes and edges only exist on arterial and highway levels)
  std::unordered_map<GraphId, bool> tile_map;
  for (auto itr = old_to_new.begin(); itr != old_to_new.end(); itr++) {
    auto f = tile_map.find(itr->first.Tile_Base());
    if (f == tile_map.end()) {
      tile_map[itr->first.Tile_Base()] = std::get<2>(itr->second).Is_Valid();
    } else {
      if (std::get<2>(itr->second).Is_Valid()) {
        f->second = true;
      }
    }
  }
  for (auto itr = tile_map.begin(); itr != tile_map.end(); itr++) {
    if (!itr->second ) {
      GraphId empty_tile = itr->first;
 //     GraphTileBuilder tilebuilder(tile_hierarchy, empty_tile, false);
 //     tilebuilder.StoreTileData();

      std::string file_location = tile_hierarchy.tile_dir() + "/" +
          GraphTile::FileSuffix(empty_tile.Tile_Base(), tile_hierarchy);
remove(file_location.c_str());  // IF WE WANT TO REMOVE FILE
      LOG_INFO("Remove file: " + file_location);
    }
  }

  // Update the end nodes to all transit connections in the transit hierarchy
//TODO!!!!

  LOG_INFO("Done HierarchyBuilder");
}

}
}
