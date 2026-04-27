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
};
