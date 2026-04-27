#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Flow Graph domain action handlers for Monolith.
 * Introspection of UFlowAsset graphs, UFlowNodeBlueprint and UFlowNodeAddOnBlueprint
 * assets from the MothCocoon Flow plugin (https://github.com/MothCocoon/FlowGraph).
 */
class FMonolithFlowActions
{
public:
	/** Register all flow actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Action handlers ---

	/** Smoke test — returns {status, module}. */
	static FMonolithActionResult Ping(const TSharedPtr<FJsonObject>& Params);

	/** All Flow Assets in the index, optional path glob. */
	static FMonolithActionResult ListAssets(const TSharedPtr<FJsonObject>& Params);

	/** One Flow Asset summary: counts, custom I/O, base params, sample nodes. */
	static FMonolithActionResult GetAssetInfo(const TSharedPtr<FJsonObject>& Params);

	/** Nodes inside one Flow Asset (optional node_class filter). */
	static FMonolithActionResult ListNodes(const TSharedPtr<FJsonObject>& Params);

	/** Pins for nodes in one Flow Asset (optional node_guid / pin_direction / pin_type_name filter). */
	static FMonolithActionResult ListNodePins(const TSharedPtr<FJsonObject>& Params);

	/** Full edge list of one Flow Asset (output pin -> input pin). */
	static FMonolithActionResult ListConnections(const TSharedPtr<FJsonObject>& Params);

	/** AddOns inside one Flow Asset (optional node_guid filter). */
	static FMonolithActionResult ListAddons(const TSharedPtr<FJsonObject>& Params);

	/** Single-node deep dive: row + parsed data + pins + connections + addon tree. */
	static FMonolithActionResult GetNodeInfo(const TSharedPtr<FJsonObject>& Params);

	/** Custom Inputs and Custom Outputs (subgraph entry/exit names) of one Flow Asset. */
	static FMonolithActionResult ListCustomEvents(const TSharedPtr<FJsonObject>& Params);

	/** Reverse: which Flow Assets reference a target Flow Asset via UFlowNode_SubGraph. */
	static FMonolithActionResult FindSubgraphCallers(const TSharedPtr<FJsonObject>& Params);

	/** Reverse: every Flow Asset using a node of the given UClass path. */
	static FMonolithActionResult FindNodeClassUsages(const TSharedPtr<FJsonObject>& Params);
};
