#pragma once
// Core only: graphs and custom nodes as JSON (graph files, custom node library files). nlohmann/json stays out of the
// headers front ends include.
#include "graph.hpp"

#include <nlohmann/json.hpp>

namespace remod {

nlohmann::json graph_json(const Graph& graph);
Graph graph_from_json(const nlohmann::json& j);  // no migration; throws GraphError or json::exception
nlohmann::json custom_json(const CustomNode& node);
CustomNode custom_from_json(const nlohmann::json& j);

}  // namespace remod
