#include "MonolithFlowIndexer.h"

#if WITH_FLOW

#include "MonolithIndexDatabase.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/FlowNodeBase.h"
#include "AddOns/FlowNodeAddOn.h"
#include "Nodes/FlowPin.h"
#include "Nodes/FlowNodeBlueprint.h"
#include "Nodes/FlowNodeAddOnBlueprint.h"
#include "Nodes/Graph/FlowNode_SubGraph.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "AssetRegistry/AssetData.h"
#include "JsonObjectConverter.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithFlowIndexer, Log, All);

// ─────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────

namespace
{
	/** Bind a string parameter, mapping empty to SQL NULL. */
	void BindNullableString(FSQLitePreparedStatement& Stmt, int32 Index, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			Stmt.SetBindingValueByIndex(Index);
		}
		else
		{
			Stmt.SetBindingValueByIndex(Index, Value);
		}
	}

	/** Bind a positive int64 row id; bind NULL when value <= 0. */
	void BindOptionalRowId(FSQLitePreparedStatement& Stmt, int32 Index, int64 RowId)
	{
		if (RowId > 0)
		{
			Stmt.SetBindingValueByIndex(Index, RowId);
		}
		else
		{
			Stmt.SetBindingValueByIndex(Index);
		}
	}

	/** Run a single-int64-param prepared statement. */
	void ExecWithInt64(FSQLiteDatabase* RawDB, const TCHAR* Sql, int64 Param1)
	{
		if (!RawDB) return;
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, Sql))
		{
			Stmt.SetBindingValueByIndex(1, Param1);
			Stmt.Execute();
		}
		Stmt.Destroy();
	}

	/**
	 * Look up an existing flow_assets row by stable path. Returns
	 * { row_id, prev_fa_asset_id } so callers can wipe orphan child
	 * rows that key off the previous asset id (core assets table
	 * autoincrement restarts on full reindex).
	 */
	void SelectExistingFlowAssetIds(FSQLiteDatabase* RawDB, const FString& FaPath, int64& OutRowId, int64& OutPrevAssetId)
	{
		OutRowId = -1;
		OutPrevAssetId = -1;
		if (!RawDB) return;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("SELECT id, fa_asset_id FROM flow_assets WHERE fa_path = ?"), ESQLitePreparedStatementFlags::Persistent))
		{
			return;
		}
		Stmt.SetBindingValueByIndex(1, FaPath);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, OutRowId);
			Stmt.GetColumnValueByIndex(1, OutPrevAssetId);
		}
		Stmt.Destroy();
	}

	/**
	 * Serialize all non-transient UPROPERTYs on an instance to compact JSON.
	 * Used for flow_nodes.data and flow_node_addons.data.
	 *
	 * Skips: CPF_Transient, CPF_DuplicateTransient — design-time-only properties survive.
	 * Skips: CPF_DisableEditOnInstance is intentionally NOT skipped (some Flow-internal
	 * UPROPERTYs that drive runtime behaviour have it set).
	 */
	FString SerializeInstancePropertiesToJson(UObject* Instance)
	{
		if (!Instance) return FString();
		const UClass* Cls = Instance->GetClass();
		if (!Cls) return FString();

		FString Out;
		const int64 SkipFlags = CPF_Transient | CPF_DuplicateTransient;
		if (!FJsonObjectConverter::UStructToJsonObjectString(
			Cls, Instance, Out,
			/*CheckFlags=*/0,
			/*SkipFlags=*/SkipFlags,
			/*Indent=*/0,
			/*ExportCb=*/nullptr,
			/*bPrettyPrint=*/false))
		{
			return FString();
		}
		return Out;
	}

	/** True if this UClass derives from a UFlowNodeBlueprint::GeneratedClass (i.e. node is BP-defined). */
	bool IsBlueprintNodeClass(const UClass* NodeClass, FString& OutBpAssetPath)
	{
		OutBpAssetPath.Reset();
		if (!NodeClass) return false;
		UBlueprint* BP = Cast<UBlueprint>(NodeClass->ClassGeneratedBy);
		if (!BP) return false;
		OutBpAssetPath = BP->GetPathName();
		return true;
	}

	/** Best-effort display name for a node — class display name (FlowNode subclasses set NodeDisplayName via UCLASS metadata). */
	FString ResolveNodeDisplayName(UFlowNode* Node)
	{
		if (!Node) return FString();
		if (const UClass* Cls = Node->GetClass())
		{
			return Cls->GetDisplayNameText().ToString();
		}
		return FString();
	}

	/** Insert one outgoing connection (output pin -> connected input pin on another node). */
	void InsertConnection(
		FSQLitePreparedStatement& Stmt,
		int64 FaAssetId,
		const FString& FromNodeGuid,
		const FString& FromPin,
		const FString& ToNodeGuid,
		const FString& ToPin)
	{
		Stmt.Reset();
		Stmt.ClearBindings();
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, FromNodeGuid);
		Stmt.SetBindingValueByIndex(3, FromPin);
		Stmt.SetBindingValueByIndex(4, ToNodeGuid);
		Stmt.SetBindingValueByIndex(5, ToPin);
		Stmt.Execute();
	}

	/**
	 * Recursively walk a node/addon's child AddOns, writing each into
	 * flow_node_addons. Returns the total addon count under this owner.
	 *
	 * - OwnerNodeGuid stays the same all the way down the recursion
	 *   (the top-level UFlowNode this addon stack ultimately belongs to).
	 * - ParentAddonRowId is -1 for direct children of the node, then the
	 *   row id of the parent addon for nested addons.
	 * - Depth: 0 for direct children of the node, +1 per level.
	 */
	int32 WalkAndInsertAddOns(
		FSQLiteDatabase* RawDB,
		FSQLitePreparedStatement& InsertStmt,
		int64 FaAssetId,
		UFlowNodeBase* Owner,
		const FString& OwnerNodeGuid,
		int64 ParentAddonRowId,
		int32 Depth)
	{
		if (!RawDB || !Owner) return 0;

		int32 Total = 0;
		const TArray<UFlowNodeAddOn*>& AddOns = Owner->GetFlowNodeAddOnChildren();
		for (int32 Idx = 0; Idx < AddOns.Num(); ++Idx)
		{
			UFlowNodeAddOn* AddOn = AddOns[Idx];
			if (!AddOn) continue;

			const FString AddOnClassPath = AddOn->GetClass()->GetPathName();
			const FString DataJson = SerializeInstancePropertiesToJson(AddOn);

			InsertStmt.Reset();
			InsertStmt.ClearBindings();
			InsertStmt.SetBindingValueByIndex(1, FaAssetId);
			InsertStmt.SetBindingValueByIndex(2, OwnerNodeGuid);
			BindOptionalRowId(InsertStmt, 3, ParentAddonRowId);
			InsertStmt.SetBindingValueByIndex(4, AddOnClassPath);
			InsertStmt.SetBindingValueByIndex(5, Idx);
			InsertStmt.SetBindingValueByIndex(6, Depth);
			BindNullableString(InsertStmt, 7, DataJson);
			InsertStmt.Execute();

			const int64 NewRowId = RawDB->GetLastInsertRowId();
			++Total;

			// Recurse — addons can have child addons.
			Total += WalkAndInsertAddOns(RawDB, InsertStmt, FaAssetId, AddOn, OwnerNodeGuid, NewRowId, Depth + 1);
		}
		return Total;
	}

	/** Walk a pin array writing one row per pin. */
	void InsertPinsForDirection(
		FSQLiteDatabase* RawDB,
		int64 FaAssetId,
		const FString& NodeGuid,
		const TArray<FFlowPin>& Pins,
		const TCHAR* Direction)
	{
		if (!RawDB || Pins.Num() == 0) return;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"INSERT INTO flow_node_pins ("
			"fa_asset_id, node_guid, pin_direction, pin_index, pin_name, pin_friendly_name, "
			"pin_type_name, pin_subcategory_object, container_type, tooltip"
			") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")))
		{
			return;
		}

		for (int32 PinIdx = 0; PinIdx < Pins.Num(); ++PinIdx)
		{
			const FFlowPin& Pin = Pins[PinIdx];
			Stmt.Reset();
			Stmt.ClearBindings();

			Stmt.SetBindingValueByIndex(1, FaAssetId);
			Stmt.SetBindingValueByIndex(2, NodeGuid);
			Stmt.SetBindingValueByIndex(3, FString(Direction));
			Stmt.SetBindingValueByIndex(4, PinIdx);
			Stmt.SetBindingValueByIndex(5, Pin.PinName.ToString());
			BindNullableString(Stmt, 6, Pin.PinFriendlyName.ToString());
			Stmt.SetBindingValueByIndex(7, Pin.GetPinTypeName().Name.ToString());

			FString SubCatPath;
			if (UObject* SubCatObj = Pin.GetPinSubCategoryObject().Get())
			{
				SubCatPath = SubCatObj->GetPathName();
			}
			BindNullableString(Stmt, 8, SubCatPath);

			const TCHAR* ContainerStr = (Pin.ContainerType == EPinContainerType::Array) ? TEXT("Array") : TEXT("None");
			Stmt.SetBindingValueByIndex(9, FString(ContainerStr));

			BindNullableString(Stmt, 10, Pin.PinToolTip);

			Stmt.Execute();
		}

		Stmt.Destroy();
	}
}

// ─────────────────────────────────────────────────────────────
// IMonolithIndexer
// ─────────────────────────────────────────────────────────────

TArray<FString> FMonolithFlowIndexer::GetSupportedClasses() const
{
	return {
		TEXT("FlowAsset"),
		TEXT("FlowNodeBlueprint"),
		TEXT("FlowNodeAddOnBlueprint")
	};
}

void FMonolithFlowIndexer::EnsureTablesExist(FMonolithIndexDatabase& DB)
{
	if (bTablesCreated) return;

	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB) return;

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_assets ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  fa_asset_id INTEGER NOT NULL,"
		"  fa_path TEXT NOT NULL,"
		"  name TEXT NOT NULL,"
		"  asset_class TEXT NOT NULL,"
		"  asset_guid TEXT,"
		"  expected_owner_class TEXT,"
		"  world_bound INTEGER NOT NULL DEFAULT 1,"
		"  base_params_path TEXT,"
		"  node_count INTEGER NOT NULL DEFAULT 0,"
		"  addon_count INTEGER NOT NULL DEFAULT 0,"
		"  subgraph_count INTEGER NOT NULL DEFAULT 0"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_assets_path ON flow_assets(fa_path)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_nodes ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  fa_asset_id INTEGER NOT NULL,"
		"  node_guid TEXT NOT NULL,"
		"  node_class TEXT NOT NULL,"
		"  display_name TEXT,"
		"  is_blueprint INTEGER NOT NULL DEFAULT 0,"
		"  blueprint_path TEXT,"
		"  addon_count INTEGER NOT NULL DEFAULT 0,"
		"  input_pin_count INTEGER NOT NULL DEFAULT 0,"
		"  output_pin_count INTEGER NOT NULL DEFAULT 0,"
		"  data TEXT"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_nodes_fa ON flow_nodes(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_nodes_class ON flow_nodes(node_class)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_node_pins ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  fa_asset_id INTEGER NOT NULL,"
		"  node_guid TEXT NOT NULL,"
		"  pin_direction TEXT NOT NULL,"
		"  pin_index INTEGER NOT NULL,"
		"  pin_name TEXT NOT NULL,"
		"  pin_friendly_name TEXT,"
		"  pin_type_name TEXT NOT NULL,"
		"  pin_subcategory_object TEXT,"
		"  container_type TEXT NOT NULL DEFAULT 'None',"
		"  tooltip TEXT"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_pins_fa ON flow_node_pins(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_pins_node ON flow_node_pins(node_guid)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_pins_type ON flow_node_pins(pin_type_name)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_node_addons ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  fa_asset_id INTEGER NOT NULL,"
		"  owner_node_guid TEXT NOT NULL,"
		"  parent_addon_id INTEGER,"
		"  addon_class TEXT NOT NULL,"
		"  addon_index INTEGER NOT NULL,"
		"  depth INTEGER NOT NULL DEFAULT 0,"
		"  data TEXT"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_addons_fa ON flow_node_addons(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_addons_owner ON flow_node_addons(owner_node_guid)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_addons_class ON flow_node_addons(addon_class)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_node_connections ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  fa_asset_id INTEGER NOT NULL,"
		"  from_node_guid TEXT NOT NULL,"
		"  from_pin TEXT NOT NULL,"
		"  to_node_guid TEXT NOT NULL,"
		"  to_pin TEXT NOT NULL"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_conn_fa ON flow_node_connections(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_conn_from ON flow_node_connections(from_node_guid)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_conn_to ON flow_node_connections(to_node_guid)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_subgraph_refs ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  host_fa_asset_id INTEGER NOT NULL,"
		"  host_fa_path TEXT NOT NULL,"
		"  host_node_guid TEXT NOT NULL,"
		"  target_fa_path TEXT,"
		"  target_params_path TEXT"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_subref_host_fa ON flow_subgraph_refs(host_fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_subref_target ON flow_subgraph_refs(target_fa_path)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_custom_events ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  fa_asset_id INTEGER NOT NULL,"
		"  kind TEXT NOT NULL,"
		"  event_name TEXT NOT NULL"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_evt_fa ON flow_custom_events(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_evt_name ON flow_custom_events(event_name)"));

	bTablesCreated = true;
}

bool FMonolithFlowIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	EnsureTablesExist(DB);

	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB) return false;

	UFlowAsset* FA = Cast<UFlowAsset>(LoadedAsset);
	if (!FA)
	{
		// FlowNodeBlueprint / FlowNodeAddOnBlueprint indexing comes in Step 6.
		return true;
	}

	const FString FaPath = FA->GetPathName();

	// ----- Cleanup of previous rows (stable key on fa_path) -----
	int64 PrevRowId = -1;
	int64 PrevAssetId = -1;
	SelectExistingFlowAssetIds(RawDB, FaPath, PrevRowId, PrevAssetId);

	if (PrevAssetId > 0)
	{
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_node_pins WHERE fa_asset_id = ?"), PrevAssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_node_addons WHERE fa_asset_id = ?"), PrevAssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_node_connections WHERE fa_asset_id = ?"), PrevAssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_subgraph_refs WHERE host_fa_asset_id = ?"), PrevAssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_custom_events WHERE fa_asset_id = ?"), PrevAssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_nodes WHERE fa_asset_id = ?"), PrevAssetId);
	}
	// Also clean by current AssetId in case PrevAssetId differs (autoincrement restart)
	if (AssetId != PrevAssetId)
	{
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_node_pins WHERE fa_asset_id = ?"), AssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_node_addons WHERE fa_asset_id = ?"), AssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_node_connections WHERE fa_asset_id = ?"), AssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_subgraph_refs WHERE host_fa_asset_id = ?"), AssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_custom_events WHERE fa_asset_id = ?"), AssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_nodes WHERE fa_asset_id = ?"), AssetId);
	}

	if (PrevRowId > 0)
	{
		ExecWithInt64(RawDB, TEXT("DELETE FROM flow_assets WHERE id = ?"), PrevRowId);
	}

	// ----- Insert flow_assets row (counts updated at end) -----
	FString AssetGuidStr = FA->AssetGuid.IsValid() ? FA->AssetGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString();

	FString ExpectedOwnerClassPath;
	if (UClass* OwnerCls = FA->GetExpectedOwnerClass())
	{
		ExpectedOwnerClassPath = OwnerCls->GetPathName();
	}

	FString BaseParamsPath = FA->BaseAssetParams.AssetPtr.ToSoftObjectPath().ToString();

	int64 FaRowId = -1;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"INSERT INTO flow_assets ("
			"fa_asset_id, fa_path, name, asset_class, asset_guid, expected_owner_class, world_bound, "
			"base_params_path, node_count, addon_count, subgraph_count"
			") VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0)")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			Stmt.SetBindingValueByIndex(2, FaPath);
			Stmt.SetBindingValueByIndex(3, FA->GetName());
			Stmt.SetBindingValueByIndex(4, FA->GetClass()->GetName());
			BindNullableString(Stmt, 5, AssetGuidStr);
			BindNullableString(Stmt, 6, ExpectedOwnerClassPath);
			Stmt.SetBindingValueByIndex(7, FA->bWorldBound ? 1 : 0);
			BindNullableString(Stmt, 8, BaseParamsPath);
			Stmt.Execute();
			Stmt.Destroy();
			FaRowId = RawDB->GetLastInsertRowId();
		}
	}

	if (FaRowId <= 0)
	{
		UE_LOG(LogMonolithFlowIndexer, Warning, TEXT("FMonolithFlowIndexer: failed to insert flow_assets row for %s"), *FaPath);
		return false;
	}

	// ----- Walk nodes -----
	int32 NodeCount = 0;
	int32 TotalAddonCount = 0;
	int32 SubGraphCount = 0;

	FSQLitePreparedStatement NodeStmt;
	const bool bNodeStmtOk = NodeStmt.Create(*RawDB, TEXT(
		"INSERT INTO flow_nodes ("
		"fa_asset_id, node_guid, node_class, display_name, is_blueprint, blueprint_path, "
		"addon_count, input_pin_count, output_pin_count, data"
		") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

	if (!bNodeStmtOk)
	{
		UE_LOG(LogMonolithFlowIndexer, Warning, TEXT("FMonolithFlowIndexer: failed to prepare flow_nodes INSERT"));
		return false;
	}

	FSQLitePreparedStatement ConnStmt;
	const bool bConnStmtOk = ConnStmt.Create(*RawDB, TEXT(
		"INSERT INTO flow_node_connections ("
		"fa_asset_id, from_node_guid, from_pin, to_node_guid, to_pin"
		") VALUES (?, ?, ?, ?, ?)"));

	FSQLitePreparedStatement AddOnStmt;
	const bool bAddOnStmtOk = AddOnStmt.Create(*RawDB, TEXT(
		"INSERT INTO flow_node_addons ("
		"fa_asset_id, owner_node_guid, parent_addon_id, addon_class, addon_index, depth, data"
		") VALUES (?, ?, ?, ?, ?, ?, ?)"));

	FSQLitePreparedStatement SubGraphStmt;
	const bool bSubGraphStmtOk = SubGraphStmt.Create(*RawDB, TEXT(
		"INSERT INTO flow_subgraph_refs ("
		"host_fa_asset_id, host_fa_path, host_node_guid, target_fa_path, target_params_path"
		") VALUES (?, ?, ?, ?, ?)"));

	if (!bConnStmtOk || !bAddOnStmtOk || !bSubGraphStmtOk)
	{
		UE_LOG(LogMonolithFlowIndexer, Warning, TEXT("FMonolithFlowIndexer: failed to prepare connections/addons/subgraph INSERTs"));
		NodeStmt.Destroy();
		if (bConnStmtOk) ConnStmt.Destroy();
		if (bAddOnStmtOk) AddOnStmt.Destroy();
		if (bSubGraphStmtOk) SubGraphStmt.Destroy();
		return false;
	}

	for (const TPair<FGuid, UFlowNode*>& Pair : FA->GetNodes())
	{
		UFlowNode* Node = Pair.Value;
		if (!Node) continue;
		++NodeCount;

		const FString NodeGuidStr = Node->GetGuid().ToString(EGuidFormats::DigitsWithHyphens);
		const FString NodeClassPath = Node->GetClass()->GetPathName();
		const FString DisplayName = ResolveNodeDisplayName(Node);

		FString BpPath;
		const bool bIsBp = IsBlueprintNodeClass(Node->GetClass(), BpPath);

		const TArray<FFlowPin>& InPins = Node->GetInputPins();
		const TArray<FFlowPin>& OutPins = Node->GetOutputPins();

		const FString DataJson = SerializeInstancePropertiesToJson(Node);

		// Walk addons first so we know the count before inserting the node row.
		const int32 NodeAddonCount = WalkAndInsertAddOns(
			RawDB, AddOnStmt, AssetId, Node, NodeGuidStr,
			/*ParentAddonRowId=*/-1, /*Depth=*/0);
		TotalAddonCount += NodeAddonCount;

		NodeStmt.Reset();
		NodeStmt.ClearBindings();
		NodeStmt.SetBindingValueByIndex(1, AssetId);
		NodeStmt.SetBindingValueByIndex(2, NodeGuidStr);
		NodeStmt.SetBindingValueByIndex(3, NodeClassPath);
		BindNullableString(NodeStmt, 4, DisplayName);
		NodeStmt.SetBindingValueByIndex(5, bIsBp ? 1 : 0);
		BindNullableString(NodeStmt, 6, BpPath);
		NodeStmt.SetBindingValueByIndex(7, NodeAddonCount);
		NodeStmt.SetBindingValueByIndex(8, InPins.Num());
		NodeStmt.SetBindingValueByIndex(9, OutPins.Num());
		BindNullableString(NodeStmt, 10, DataJson);
		NodeStmt.Execute();

		// Pins
		InsertPinsForDirection(RawDB, AssetId, NodeGuidStr, InPins, TEXT("input"));
		InsertPinsForDirection(RawDB, AssetId, NodeGuidStr, OutPins, TEXT("output"));

		// Outgoing connections — UFlowNode::Connections is protected; only
		// per-pin GetConnection(FName) is public. Walk output pins, query
		// each. Connections are stored on the source side of an edge per
		// the FlowNode.h comment, so this catches every edge originating here.
		for (const FFlowPin& OutPin : OutPins)
		{
			const FConnectedPin Connected = Node->GetConnection(OutPin.PinName);
			if (!Connected.NodeGuid.IsValid()) continue;

			InsertConnection(ConnStmt, AssetId,
				NodeGuidStr,
				OutPin.PinName.ToString(),
				Connected.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
				Connected.PinName.ToString());
		}

		// SubGraph reference — Asset / AssetParams are private UPROPERTYs
		// on UFlowNode_SubGraph with no public accessor; read via reflection.
		if (UFlowNode_SubGraph* SubGraphNode = Cast<UFlowNode_SubGraph>(Node))
		{
			auto ReadSoftPath = [SubGraphNode](const TCHAR* PropName) -> FString
			{
				if (FSoftObjectProperty* Prop = CastField<FSoftObjectProperty>(
						SubGraphNode->GetClass()->FindPropertyByName(PropName)))
				{
					const FSoftObjectPtr SoftPtr = Prop->GetPropertyValue_InContainer(SubGraphNode);
					return SoftPtr.ToSoftObjectPath().ToString();
				}
				return FString();
			};

			const FString TargetFaPath     = ReadSoftPath(TEXT("Asset"));
			const FString TargetParamsPath = ReadSoftPath(TEXT("AssetParams"));

			SubGraphStmt.Reset();
			SubGraphStmt.ClearBindings();
			SubGraphStmt.SetBindingValueByIndex(1, AssetId);
			SubGraphStmt.SetBindingValueByIndex(2, FaPath);
			SubGraphStmt.SetBindingValueByIndex(3, NodeGuidStr);
			BindNullableString(SubGraphStmt, 4, TargetFaPath);
			BindNullableString(SubGraphStmt, 5, TargetParamsPath);
			SubGraphStmt.Execute();
			++SubGraphCount;
		}
	}

	NodeStmt.Destroy();
	ConnStmt.Destroy();
	AddOnStmt.Destroy();
	SubGraphStmt.Destroy();

	// ----- Custom Inputs / Custom Outputs (runtime-safe accessors that
	//       walk UFlowNode_CustomInput / UFlowNode_CustomOutput nodes) -----
	{
		FSQLitePreparedStatement EvtStmt;
		if (EvtStmt.Create(*RawDB, TEXT(
			"INSERT INTO flow_custom_events (fa_asset_id, kind, event_name) VALUES (?, ?, ?)")))
		{
			for (const FName& InputName : FA->GatherCustomInputNodeEventNames())
			{
				if (InputName.IsNone()) continue;
				EvtStmt.Reset();
				EvtStmt.ClearBindings();
				EvtStmt.SetBindingValueByIndex(1, AssetId);
				EvtStmt.SetBindingValueByIndex(2, FString(TEXT("input")));
				EvtStmt.SetBindingValueByIndex(3, InputName.ToString());
				EvtStmt.Execute();
			}
			for (const FName& OutputName : FA->GatherCustomOutputNodeEventNames())
			{
				if (OutputName.IsNone()) continue;
				EvtStmt.Reset();
				EvtStmt.ClearBindings();
				EvtStmt.SetBindingValueByIndex(1, AssetId);
				EvtStmt.SetBindingValueByIndex(2, FString(TEXT("output")));
				EvtStmt.SetBindingValueByIndex(3, OutputName.ToString());
				EvtStmt.Execute();
			}
			EvtStmt.Destroy();
		}
	}

	// ----- Update counts on flow_assets row -----
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"UPDATE flow_assets SET node_count = ?, addon_count = ?, subgraph_count = ? WHERE id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, NodeCount);
			Stmt.SetBindingValueByIndex(2, TotalAddonCount);
			Stmt.SetBindingValueByIndex(3, SubGraphCount);
			Stmt.SetBindingValueByIndex(4, FaRowId);
			Stmt.Execute();
			Stmt.Destroy();
		}
	}

	UE_LOG(LogMonolithFlowIndexer, Verbose, TEXT("FMonolithFlowIndexer: indexed %s — %d nodes, %d addons, %d subgraph refs"),
		*FaPath, NodeCount, TotalAddonCount, SubGraphCount);
	return true;
}

#endif // WITH_FLOW
