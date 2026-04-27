#pragma once

#include "CoreMinimal.h"

#if WITH_FLOW

#include "MonolithIndexer.h"

class FMonolithIndexDatabase;

/**
 * Indexes Flow plugin assets:
 *   - UFlowAsset                 → flow_assets, flow_nodes, flow_node_pins
 *   - UFlowNodeBlueprint         → flow_node_classes (kind='node', is_native=0)
 *   - UFlowNodeAddOnBlueprint    → flow_node_classes (kind='addon', is_native=0)
 *
 * Per-node UPROPERTY snapshots are written to flow_nodes.data as compact JSON
 * (FJsonObjectConverter::UStructToJsonObjectString with CPF_Transient skipped).
 *
 * Tables created (Step 2 subset; Steps 3+ extend):
 *   flow_assets                 (this step)
 *   flow_nodes                  (this step, with .data JSON column)
 *   flow_node_pins              (this step)
 *   flow_node_addons            (Step 3, with .data JSON column)
 *   flow_node_connections       (Step 3)
 *   flow_subgraph_refs          (Step 4)
 *   flow_custom_events          (Step 4)
 *   flow_node_classes           (Step 6 — native rows opportunistic during graph walk)
 *
 * NB: no FK on assets(id) (would block ResetDatabase on full reindex).
 * Cleanup keyed by stable fa_path (assets.id autoincrement restarts).
 */
class FMonolithFlowIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override;
	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("Flow"); }

private:
	void EnsureTablesExist(FMonolithIndexDatabase& DB);
	bool bTablesCreated = false;
};

#endif // WITH_FLOW
