#include "MonolithFlowActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace
{
	/** Fetch the SQLite raw handle, or set OutError + return null. */
	FSQLiteDatabase* GetRawDB(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor not available");
			return nullptr;
		}
		UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
		if (!IndexSS || !IndexSS->GetDatabase())
		{
			OutError = TEXT("Index database not ready");
			return nullptr;
		}
		FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
		if (!RawDB)
		{
			OutError = TEXT("Raw SQLite database not available");
			return nullptr;
		}
		return RawDB;
	}

	/** Convert a glob pattern (* / ?) to SQL LIKE with single-quote escape. */
	FString GlobToLike(const FString& Glob)
	{
		return Glob
			.Replace(TEXT("'"), TEXT("''"))
			.Replace(TEXT("*"), TEXT("%"))
			.Replace(TEXT("?"), TEXT("_"));
	}

	/** Lookup flow_assets row id + asset id by fa_path. Returns -1 if not found. */
	void SelectFlowAssetIds(FSQLiteDatabase* RawDB, const FString& FaPath, int64& OutRowId, int64& OutAssetId)
	{
		OutRowId = -1;
		OutAssetId = -1;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("SELECT id, fa_asset_id FROM flow_assets WHERE fa_path = ?")))
		{
			return;
		}
		Stmt.SetBindingValueByIndex(1, FaPath);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, OutRowId);
			Stmt.GetColumnValueByIndex(1, OutAssetId);
		}
		Stmt.Destroy();
	}

	/** Try parse a JSON string into a JsonValue (object/array); returns null JsonValue on failure. */
	TSharedPtr<FJsonValue> TryParseJsonValue(const FString& JsonStr)
	{
		if (JsonStr.IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		TSharedPtr<FJsonValue> Out;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		return Out;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithFlowActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("flow"), TEXT("ping"),
		TEXT("Smoke test — returns {status:ok, module:MonolithFlow} when the module is loaded."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::Ping),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_assets"),
		TEXT("List every UFlowAsset in the index, with node_count / addon_count / subgraph_count summary. Optional asset_path_filter is a glob (* and ?) matched against fa_path."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob pattern to filter fa_path (e.g., \"/Game/Flow/*\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("get_asset_info"),
		TEXT("Return summary information for a single Flow Asset: name, asset class, expected owner class, world-bound flag, base-params soft path, total counts (nodes / addons / subgraphs / pins), and a sample of up to 10 nodes for orientation."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::GetAssetInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path (e.g., \"/Game/Flow/MyScenario.MyScenario\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_nodes"),
		TEXT("List every UFlowNode inside one Flow Asset. Each row carries node_guid, node_class, display_name, is_blueprint flag, blueprint_path (if BP-defined), addon_count, input/output pin counts, and parsed `data` JSON of design-time UPROPERTYs. Optional node_class filter narrows the result."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Exact UClass path filter (e.g. \"/Script/Flow.FlowNode_Start\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_node_pins"),
		TEXT("List every pin (input + output) on nodes in one Flow Asset, with full type info (pin_type_name from FFlowPin, plus pin_subcategory_object such as the UClass path for Object pins). Optional filters narrow by node, direction, or pin type."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListNodePins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Optional(TEXT("node_guid"), TEXT("string"), TEXT("Restrict to one node by its FGuid string"))
			.Optional(TEXT("pin_direction"), TEXT("string"), TEXT("\"input\" | \"output\" | \"all\" (default)"))
			.Optional(TEXT("pin_type_name"), TEXT("string"), TEXT("Restrict to pins with this PinTypeName (e.g. \"Exec\", \"Bool\", \"Object\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_connections"),
		TEXT("List every output-pin -> input-pin connection inside one Flow Asset (one row per outgoing edge). Optional from_node_guid / to_node_guid filters narrow the result."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListConnections),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Optional(TEXT("from_node_guid"), TEXT("string"), TEXT("Restrict to edges originating at this node"))
			.Optional(TEXT("to_node_guid"), TEXT("string"), TEXT("Restrict to edges terminating at this node"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_addons"),
		TEXT("List every UFlowNodeAddOn attached to nodes in one Flow Asset (recursive — addons can have child addons). Each row carries owner_node_guid, parent_addon_id (NULL for top-level), depth, addon_class, and parsed `data` JSON of the addon's design-time UPROPERTYs. Optional node_guid filter restricts to one node's addon stack."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListAddons),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Optional(TEXT("node_guid"), TEXT("string"), TEXT("Restrict to addons under this node"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("get_node_info"),
		TEXT("Single-node deep dive. Joins the flow_nodes row with its parsed `data` JSON, every pin (with type info), every outgoing connection, and the recursive addon tree for one node identified by GUID inside one Flow Asset."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::GetNodeInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Node FGuid string (digits-with-hyphens, matches list_nodes output)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_custom_events"),
		TEXT("List Custom Inputs and Custom Outputs of one Flow Asset (the named entry/exit pins exposed to parent SubGraph nodes). Sourced from UFlowAsset::GatherCustomInputNodeEventNames / GatherCustomOutputNodeEventNames — runtime-safe, walks actual graph nodes."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListCustomEvents),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Optional(TEXT("kind"), TEXT("string"), TEXT("\"input\" | \"output\" | \"all\" (default)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_subgraph_callers"),
		TEXT("Cross-asset reverse lookup: every Flow Asset that hosts a UFlowNode_SubGraph referencing the target Flow Asset. Returns one row per call site (host_fa_path + host_node_guid + target_params_path). Optional asset_path_filter glob narrows the host search."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindSubgraphCallers),
		FParamSchemaBuilder()
			.Required(TEXT("target_asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path of the target subgraph (or its TopLevelAssetPath form for a soft reference)"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host_fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_node_class_usages"),
		TEXT("Cross-asset reverse lookup: every Flow Asset containing a node of the specified UClass path. Returns rows grouped by host_fa_path with per-node guids. Optional asset_path_filter glob narrows the host search."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindNodeClassUsages),
		FParamSchemaBuilder()
			.Required(TEXT("node_class"), TEXT("string"), TEXT("Exact UClass path (e.g. \"/Script/SIMULATOR.SimulatorFlowNode_Dialogue\")"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host_fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_pins_by_type"),
		TEXT("Cross-asset reverse lookup over flow_node_pins: every pin in the index whose pin_type_name matches. Optional pin_subcategory_object (UClass path for Object/Class pins or UScriptStruct path for Struct pins) further narrows results. Optional pin_direction restricts to input/output. Optional asset_path_filter glob restricts host fa_path. Rows grouped by host fa_path."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindPinsByType),
		FParamSchemaBuilder()
			.Required(TEXT("pin_type_name"), TEXT("string"), TEXT("FFlowPin pin_type_name (e.g. \"Exec\", \"Bool\", \"Object\")"))
			.Optional(TEXT("pin_subcategory_object"), TEXT("string"), TEXT("Path of the pin's sub-object (UClass / UScriptStruct)"))
			.Optional(TEXT("pin_direction"), TEXT("string"), TEXT("\"input\" | \"output\" | \"all\" (default)"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_nodes_by_property"),
		TEXT("Cross-asset reverse lookup over the per-node `data` JSON snapshot of design-time UPROPERTYs. Substring match (case-sensitive, SQL LIKE; reserved chars _ % \\ are escaped). Optional property_name scopes the match so it only fires inside `\"property_name\":...value_substring`. Optional node_class narrows to one UClass. Optional asset_path_filter glob narrows host fa_path. Returns matching nodes grouped by host fa_path with their parsed data."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindNodesByProperty),
		FParamSchemaBuilder()
			.Required(TEXT("value_substring"), TEXT("string"), TEXT("Literal substring to find inside the node `data` JSON"))
			.Optional(TEXT("property_name"), TEXT("string"), TEXT("If set, restrict the match to the value side of `\"property_name\":...` (any nesting)"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Restrict to nodes of this UClass path"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host fa_path"))
			.Build());
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithFlowActions::Ping(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("module"), TEXT("MonolithFlow"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);
	}

	FString SQL = TEXT(
		"SELECT fa_path, name, asset_class, expected_owner_class, base_params_path, "
		"world_bound, node_count, addon_count, subgraph_count "
		"FROM flow_assets");
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" WHERE fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY fa_path");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_assets SQL"));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Name, AssetClass, OwnerCls, BaseParams;
		int64 World = 1, NodeCount = 0, AddonCount = 0, SubgraphCount = 0;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Name);
		Stmt.GetColumnValueByIndex(2, AssetClass);
		Stmt.GetColumnValueByIndex(3, OwnerCls);
		Stmt.GetColumnValueByIndex(4, BaseParams);
		Stmt.GetColumnValueByIndex(5, World);
		Stmt.GetColumnValueByIndex(6, NodeCount);
		Stmt.GetColumnValueByIndex(7, AddonCount);
		Stmt.GetColumnValueByIndex(8, SubgraphCount);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("fa_path"), FaPath);
		Row->SetStringField(TEXT("name"), Name);
		Row->SetStringField(TEXT("asset_class"), AssetClass);
		if (!OwnerCls.IsEmpty()) Row->SetStringField(TEXT("expected_owner_class"), OwnerCls);
		if (!BaseParams.IsEmpty()) Row->SetStringField(TEXT("base_params_path"), BaseParams);
		Row->SetBoolField(TEXT("world_bound"), World != 0);
		Row->SetNumberField(TEXT("node_count"), NodeCount);
		Row->SetNumberField(TEXT("addon_count"), AddonCount);
		Row->SetNumberField(TEXT("subgraph_count"), SubgraphCount);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("assets"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::GetAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'. Path must match UFlowAsset::GetPathName() (e.g. \"/Game/Flow/Foo.Foo\")."),
			*AssetPath));
	}

	auto Result = MakeShared<FJsonObject>();

	// Header row from flow_assets
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT name, asset_class, asset_guid, expected_owner_class, base_params_path, "
			"world_bound, node_count, addon_count, subgraph_count "
			"FROM flow_assets WHERE id = ?")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare get_asset_info header SQL"));
		}
		Stmt.SetBindingValueByIndex(1, RowId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Name, AssetClass, Guid, Owner, BaseParams;
			int64 World = 1, NodeCount = 0, AddonCount = 0, SubgraphCount = 0;
			Stmt.GetColumnValueByIndex(0, Name);
			Stmt.GetColumnValueByIndex(1, AssetClass);
			Stmt.GetColumnValueByIndex(2, Guid);
			Stmt.GetColumnValueByIndex(3, Owner);
			Stmt.GetColumnValueByIndex(4, BaseParams);
			Stmt.GetColumnValueByIndex(5, World);
			Stmt.GetColumnValueByIndex(6, NodeCount);
			Stmt.GetColumnValueByIndex(7, AddonCount);
			Stmt.GetColumnValueByIndex(8, SubgraphCount);

			Result->SetStringField(TEXT("fa_path"), AssetPath);
			Result->SetStringField(TEXT("name"), Name);
			Result->SetStringField(TEXT("asset_class"), AssetClass);
			if (!Guid.IsEmpty()) Result->SetStringField(TEXT("asset_guid"), Guid);
			if (!Owner.IsEmpty()) Result->SetStringField(TEXT("expected_owner_class"), Owner);
			if (!BaseParams.IsEmpty()) Result->SetStringField(TEXT("base_params_path"), BaseParams);
			Result->SetBoolField(TEXT("world_bound"), World != 0);
			Result->SetNumberField(TEXT("node_count"), NodeCount);
			Result->SetNumberField(TEXT("addon_count"), AddonCount);
			Result->SetNumberField(TEXT("subgraph_count"), SubgraphCount);
		}
		Stmt.Destroy();
	}

	// Pin total
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT("SELECT COUNT(*) FROM flow_node_pins WHERE fa_asset_id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 PinCount = 0;
				Stmt.GetColumnValueByIndex(0, PinCount);
				Result->SetNumberField(TEXT("pin_count"), PinCount);
			}
			Stmt.Destroy();
		}
	}

	// Node-class breakdown
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT node_class, COUNT(*) AS n FROM flow_nodes WHERE fa_asset_id = ? "
			"GROUP BY node_class ORDER BY n DESC, node_class")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			TSharedRef<FJsonObject> Breakdown = MakeShared<FJsonObject>();
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Cls;
				int64 N = 0;
				Stmt.GetColumnValueByIndex(0, Cls);
				Stmt.GetColumnValueByIndex(1, N);
				Breakdown->SetNumberField(Cls, N);
			}
			Stmt.Destroy();
			Result->SetObjectField(TEXT("node_class_breakdown"), Breakdown);
		}
	}

	// Sample of up to 10 nodes
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT node_guid, node_class, display_name, input_pin_count, output_pin_count "
			"FROM flow_nodes WHERE fa_asset_id = ? "
			"ORDER BY node_class, node_guid LIMIT 10")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			TArray<TSharedPtr<FJsonValue>> Sample;
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Guid, Cls, Display;
				int64 InN = 0, OutN = 0;
				Stmt.GetColumnValueByIndex(0, Guid);
				Stmt.GetColumnValueByIndex(1, Cls);
				Stmt.GetColumnValueByIndex(2, Display);
				Stmt.GetColumnValueByIndex(3, InN);
				Stmt.GetColumnValueByIndex(4, OutN);

				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("node_guid"), Guid);
				Row->SetStringField(TEXT("node_class"), Cls);
				if (!Display.IsEmpty()) Row->SetStringField(TEXT("display_name"), Display);
				Row->SetNumberField(TEXT("input_pin_count"), InN);
				Row->SetNumberField(TEXT("output_pin_count"), OutN);
				Sample.Add(MakeShared<FJsonValueObject>(Row));
			}
			Stmt.Destroy();
			Result->SetArrayField(TEXT("sample_nodes"), Sample);
		}
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString NodeClassFilter;
	Params->TryGetStringField(TEXT("node_class"), NodeClassFilter);

	FString SQL = TEXT(
		"SELECT node_guid, node_class, display_name, is_blueprint, blueprint_path, "
		"addon_count, input_pin_count, output_pin_count, data "
		"FROM flow_nodes WHERE fa_asset_id = ?");
	if (!NodeClassFilter.IsEmpty())
	{
		SQL += TEXT(" AND node_class = ?");
	}
	SQL += TEXT(" ORDER BY node_class, node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_nodes SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!NodeClassFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(2, NodeClassFilter);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Guid, Cls, Display, BpPath, DataJson;
		int64 IsBp = 0, AddonN = 0, InN = 0, OutN = 0;
		Stmt.GetColumnValueByIndex(0, Guid);
		Stmt.GetColumnValueByIndex(1, Cls);
		Stmt.GetColumnValueByIndex(2, Display);
		Stmt.GetColumnValueByIndex(3, IsBp);
		Stmt.GetColumnValueByIndex(4, BpPath);
		Stmt.GetColumnValueByIndex(5, AddonN);
		Stmt.GetColumnValueByIndex(6, InN);
		Stmt.GetColumnValueByIndex(7, OutN);
		Stmt.GetColumnValueByIndex(8, DataJson);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("node_class"), Cls);
		if (!Display.IsEmpty()) Row->SetStringField(TEXT("display_name"), Display);
		Row->SetBoolField(TEXT("is_blueprint"), IsBp != 0);
		if (!BpPath.IsEmpty()) Row->SetStringField(TEXT("blueprint_path"), BpPath);
		Row->SetNumberField(TEXT("addon_count"), AddonN);
		Row->SetNumberField(TEXT("input_pin_count"), InN);
		Row->SetNumberField(TEXT("output_pin_count"), OutN);
		Row->SetField(TEXT("data"), TryParseJsonValue(DataJson));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("nodes"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

// ----- Internal helpers shared by Step 3 actions -----

namespace
{
	/** Read every pin row for one node into a JSON array (used by GetNodeInfo). */
	TArray<TSharedPtr<FJsonValue>> ReadPinsForNode(FSQLiteDatabase* RawDB, int64 FaAssetId, const FString& NodeGuid)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT pin_direction, pin_index, pin_name, pin_friendly_name, "
			"pin_type_name, pin_subcategory_object, container_type, tooltip "
			"FROM flow_node_pins WHERE fa_asset_id = ? AND node_guid = ? "
			"ORDER BY pin_direction, pin_index")))
		{
			return Out;
		}
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Dir, PinName, Friendly, TypeName, SubCat, Container, Tooltip;
			int64 PinIdx = 0;
			Stmt.GetColumnValueByIndex(0, Dir);
			Stmt.GetColumnValueByIndex(1, PinIdx);
			Stmt.GetColumnValueByIndex(2, PinName);
			Stmt.GetColumnValueByIndex(3, Friendly);
			Stmt.GetColumnValueByIndex(4, TypeName);
			Stmt.GetColumnValueByIndex(5, SubCat);
			Stmt.GetColumnValueByIndex(6, Container);
			Stmt.GetColumnValueByIndex(7, Tooltip);

			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("pin_direction"), Dir);
			Row->SetNumberField(TEXT("pin_index"), PinIdx);
			Row->SetStringField(TEXT("pin_name"), PinName);
			if (!Friendly.IsEmpty()) Row->SetStringField(TEXT("pin_friendly_name"), Friendly);
			Row->SetStringField(TEXT("pin_type_name"), TypeName);
			if (!SubCat.IsEmpty()) Row->SetStringField(TEXT("pin_subcategory_object"), SubCat);
			Row->SetStringField(TEXT("container_type"), Container);
			if (!Tooltip.IsEmpty()) Row->SetStringField(TEXT("tooltip"), Tooltip);
			Out.Add(MakeShared<FJsonValueObject>(Row));
		}
		Stmt.Destroy();
		return Out;
	}

	/** Read outgoing connections for one node into a JSON array. */
	TArray<TSharedPtr<FJsonValue>> ReadOutgoingConnections(FSQLiteDatabase* RawDB, int64 FaAssetId, const FString& NodeGuid)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT from_pin, to_node_guid, to_pin "
			"FROM flow_node_connections WHERE fa_asset_id = ? AND from_node_guid = ? "
			"ORDER BY from_pin, to_node_guid")))
		{
			return Out;
		}
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString FromPin, ToGuid, ToPin;
			Stmt.GetColumnValueByIndex(0, FromPin);
			Stmt.GetColumnValueByIndex(1, ToGuid);
			Stmt.GetColumnValueByIndex(2, ToPin);

			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("from_pin"), FromPin);
			Row->SetStringField(TEXT("to_node_guid"), ToGuid);
			Row->SetStringField(TEXT("to_pin"), ToPin);
			Out.Add(MakeShared<FJsonValueObject>(Row));
		}
		Stmt.Destroy();
		return Out;
	}

	/**
	 * Read all addons under a node and assemble a tree of JSON objects.
	 * Each addon row becomes:
	 *   { id, addon_class, addon_index, depth, data, children: [...recursive...] }
	 */
	TArray<TSharedPtr<FJsonValue>> ReadAddonTreeForNode(FSQLiteDatabase* RawDB, int64 FaAssetId, const FString& NodeGuid)
	{
		// Single SELECT, then assemble in memory by parent_addon_id.
		struct FAddonRow
		{
			int64 Id;
			int64 ParentId; // -1 when NULL
			FString AddonClass;
			int64 AddonIndex;
			int64 Depth;
			FString DataJson;
		};
		TArray<FAddonRow> Rows;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT id, parent_addon_id, addon_class, addon_index, depth, data "
			"FROM flow_node_addons WHERE fa_asset_id = ? AND owner_node_guid = ? "
			"ORDER BY depth, addon_index")))
		{
			return {};
		}
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FAddonRow R;
			Stmt.GetColumnValueByIndex(0, R.Id);
			// SQLite returns 0 for NULL via the int64 overload — we re-check with a separate IS NULL helper if needed.
			// Trick: read parent as text then re-parse, or use IsColumnValueNull. Simpler: use IFNULL in SQL.
			int64 ParentRaw = 0;
			Stmt.GetColumnValueByIndex(1, ParentRaw);
			R.ParentId = (ParentRaw > 0) ? ParentRaw : -1;
			Stmt.GetColumnValueByIndex(2, R.AddonClass);
			Stmt.GetColumnValueByIndex(3, R.AddonIndex);
			Stmt.GetColumnValueByIndex(4, R.Depth);
			Stmt.GetColumnValueByIndex(5, R.DataJson);
			Rows.Add(MoveTemp(R));
		}
		Stmt.Destroy();

		// Build JSON objects keyed by id.
		TMap<int64, TSharedPtr<FJsonObject>> NodesById;
		for (const FAddonRow& R : Rows)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("id"), R.Id);
			Obj->SetStringField(TEXT("addon_class"), R.AddonClass);
			Obj->SetNumberField(TEXT("addon_index"), R.AddonIndex);
			Obj->SetNumberField(TEXT("depth"), R.Depth);
			TSharedPtr<FJsonValue> DataVal;
			if (!R.DataJson.IsEmpty())
			{
				TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(R.DataJson);
				if (!FJsonSerializer::Deserialize(Reader, DataVal) || !DataVal.IsValid())
				{
					DataVal = MakeShared<FJsonValueNull>();
				}
			}
			else
			{
				DataVal = MakeShared<FJsonValueNull>();
			}
			Obj->SetField(TEXT("data"), DataVal);
			Obj->SetArrayField(TEXT("children"), {});
			NodesById.Add(R.Id, Obj);
		}

		// Link children to parents.
		TArray<TSharedPtr<FJsonValue>> Roots;
		for (const FAddonRow& R : Rows)
		{
			TSharedPtr<FJsonObject> Self = NodesById.FindRef(R.Id);
			if (!Self.IsValid()) continue;

			if (R.ParentId > 0)
			{
				if (TSharedPtr<FJsonObject> Parent = NodesById.FindRef(R.ParentId))
				{
					TArray<TSharedPtr<FJsonValue>> Children = Parent->GetArrayField(TEXT("children"));
					Children.Add(MakeShared<FJsonValueObject>(Self));
					Parent->SetArrayField(TEXT("children"), Children);
				}
			}
			else
			{
				Roots.Add(MakeShared<FJsonValueObject>(Self));
			}
		}

		return Roots;
	}
}

FMonolithActionResult FMonolithFlowActions::ListNodePins(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString NodeGuid, Direction, PinTypeName;
	Params->TryGetStringField(TEXT("node_guid"), NodeGuid);
	Params->TryGetStringField(TEXT("pin_direction"), Direction);
	Params->TryGetStringField(TEXT("pin_type_name"), PinTypeName);

	FString SQL = TEXT(
		"SELECT node_guid, pin_direction, pin_index, pin_name, pin_friendly_name, "
		"pin_type_name, pin_subcategory_object, container_type, tooltip "
		"FROM flow_node_pins WHERE fa_asset_id = ?");
	int32 NextBind = 2;
	if (!NodeGuid.IsEmpty())
	{
		SQL += TEXT(" AND node_guid = ?");
	}
	if (!Direction.IsEmpty() && Direction != TEXT("all"))
	{
		SQL += TEXT(" AND pin_direction = ?");
	}
	if (!PinTypeName.IsEmpty())
	{
		SQL += TEXT(" AND pin_type_name = ?");
	}
	SQL += TEXT(" ORDER BY node_guid, pin_direction, pin_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_node_pins SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!NodeGuid.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, NodeGuid);
	if (!Direction.IsEmpty() && Direction != TEXT("all")) Stmt.SetBindingValueByIndex(NextBind++, Direction);
	if (!PinTypeName.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, PinTypeName);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Guid, Dir, PinName, FriendlyName, TypeName, SubCat, Container, Tooltip;
		int64 PinIdx = 0;
		Stmt.GetColumnValueByIndex(0, Guid);
		Stmt.GetColumnValueByIndex(1, Dir);
		Stmt.GetColumnValueByIndex(2, PinIdx);
		Stmt.GetColumnValueByIndex(3, PinName);
		Stmt.GetColumnValueByIndex(4, FriendlyName);
		Stmt.GetColumnValueByIndex(5, TypeName);
		Stmt.GetColumnValueByIndex(6, SubCat);
		Stmt.GetColumnValueByIndex(7, Container);
		Stmt.GetColumnValueByIndex(8, Tooltip);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("pin_direction"), Dir);
		Row->SetNumberField(TEXT("pin_index"), PinIdx);
		Row->SetStringField(TEXT("pin_name"), PinName);
		if (!FriendlyName.IsEmpty()) Row->SetStringField(TEXT("pin_friendly_name"), FriendlyName);
		Row->SetStringField(TEXT("pin_type_name"), TypeName);
		if (!SubCat.IsEmpty()) Row->SetStringField(TEXT("pin_subcategory_object"), SubCat);
		Row->SetStringField(TEXT("container_type"), Container);
		if (!Tooltip.IsEmpty()) Row->SetStringField(TEXT("tooltip"), Tooltip);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("pins"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListConnections(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString FromGuid, ToGuid;
	Params->TryGetStringField(TEXT("from_node_guid"), FromGuid);
	Params->TryGetStringField(TEXT("to_node_guid"), ToGuid);

	FString SQL = TEXT(
		"SELECT from_node_guid, from_pin, to_node_guid, to_pin "
		"FROM flow_node_connections WHERE fa_asset_id = ?");
	int32 NextBind = 2;
	if (!FromGuid.IsEmpty()) SQL += TEXT(" AND from_node_guid = ?");
	if (!ToGuid.IsEmpty())   SQL += TEXT(" AND to_node_guid = ?");
	SQL += TEXT(" ORDER BY from_node_guid, from_pin, to_node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_connections SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!FromGuid.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, FromGuid);
	if (!ToGuid.IsEmpty())   Stmt.SetBindingValueByIndex(NextBind++, ToGuid);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FromG, FromP, ToG, ToP;
		Stmt.GetColumnValueByIndex(0, FromG);
		Stmt.GetColumnValueByIndex(1, FromP);
		Stmt.GetColumnValueByIndex(2, ToG);
		Stmt.GetColumnValueByIndex(3, ToP);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("from_node_guid"), FromG);
		Row->SetStringField(TEXT("from_pin"), FromP);
		Row->SetStringField(TEXT("to_node_guid"), ToG);
		Row->SetStringField(TEXT("to_pin"), ToP);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("connections"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListAddons(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString NodeGuid;
	Params->TryGetStringField(TEXT("node_guid"), NodeGuid);

	FString SQL = TEXT(
		"SELECT id, owner_node_guid, parent_addon_id, addon_class, addon_index, depth, data "
		"FROM flow_node_addons WHERE fa_asset_id = ?");
	if (!NodeGuid.IsEmpty()) SQL += TEXT(" AND owner_node_guid = ?");
	SQL += TEXT(" ORDER BY owner_node_guid, depth, addon_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_addons SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!NodeGuid.IsEmpty()) Stmt.SetBindingValueByIndex(2, NodeGuid);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0, ParentId = 0, AddonIdx = 0, Depth = 0;
		FString Owner, Cls, DataJson;
		Stmt.GetColumnValueByIndex(0, Id);
		Stmt.GetColumnValueByIndex(1, Owner);
		Stmt.GetColumnValueByIndex(2, ParentId);
		Stmt.GetColumnValueByIndex(3, Cls);
		Stmt.GetColumnValueByIndex(4, AddonIdx);
		Stmt.GetColumnValueByIndex(5, Depth);
		Stmt.GetColumnValueByIndex(6, DataJson);

		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("id"), Id);
		Row->SetStringField(TEXT("owner_node_guid"), Owner);
		if (ParentId > 0) Row->SetNumberField(TEXT("parent_addon_id"), ParentId);
		Row->SetStringField(TEXT("addon_class"), Cls);
		Row->SetNumberField(TEXT("addon_index"), AddonIdx);
		Row->SetNumberField(TEXT("depth"), Depth);
		Row->SetField(TEXT("data"), TryParseJsonValue(DataJson));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("addons"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::GetNodeInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath, NodeGuid;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_guid is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetStringField(TEXT("node_guid"), NodeGuid);

	bool bFound = false;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT node_class, display_name, is_blueprint, blueprint_path, "
			"addon_count, input_pin_count, output_pin_count, data "
			"FROM flow_nodes WHERE fa_asset_id = ? AND node_guid = ?")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare get_node_info SQL"));
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			bFound = true;
			FString Cls, Display, BpPath, DataJson;
			int64 IsBp = 0, AddonN = 0, InN = 0, OutN = 0;
			Stmt.GetColumnValueByIndex(0, Cls);
			Stmt.GetColumnValueByIndex(1, Display);
			Stmt.GetColumnValueByIndex(2, IsBp);
			Stmt.GetColumnValueByIndex(3, BpPath);
			Stmt.GetColumnValueByIndex(4, AddonN);
			Stmt.GetColumnValueByIndex(5, InN);
			Stmt.GetColumnValueByIndex(6, OutN);
			Stmt.GetColumnValueByIndex(7, DataJson);

			Result->SetStringField(TEXT("node_class"), Cls);
			if (!Display.IsEmpty()) Result->SetStringField(TEXT("display_name"), Display);
			Result->SetBoolField(TEXT("is_blueprint"), IsBp != 0);
			if (!BpPath.IsEmpty()) Result->SetStringField(TEXT("blueprint_path"), BpPath);
			Result->SetNumberField(TEXT("addon_count"), AddonN);
			Result->SetNumberField(TEXT("input_pin_count"), InN);
			Result->SetNumberField(TEXT("output_pin_count"), OutN);
			Result->SetField(TEXT("data"), TryParseJsonValue(DataJson));
		}
		Stmt.Destroy();
	}

	if (!bFound)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No node with guid '%s' in '%s'."), *NodeGuid, *AssetPath));
	}

	Result->SetArrayField(TEXT("pins"), ReadPinsForNode(RawDB, AssetId, NodeGuid));
	Result->SetArrayField(TEXT("outgoing_connections"), ReadOutgoingConnections(RawDB, AssetId, NodeGuid));
	Result->SetArrayField(TEXT("addons"), ReadAddonTreeForNode(RawDB, AssetId, NodeGuid));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListCustomEvents(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString Kind;
	Params->TryGetStringField(TEXT("kind"), Kind);

	FString SQL = TEXT("SELECT kind, event_name FROM flow_custom_events WHERE fa_asset_id = ?");
	if (!Kind.IsEmpty() && Kind != TEXT("all"))
	{
		SQL += TEXT(" AND kind = ?");
	}
	SQL += TEXT(" ORDER BY kind, event_name");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_custom_events SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!Kind.IsEmpty() && Kind != TEXT("all"))
	{
		Stmt.SetBindingValueByIndex(2, Kind);
	}

	TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString K, Name;
		Stmt.GetColumnValueByIndex(0, K);
		Stmt.GetColumnValueByIndex(1, Name);
		auto Val = MakeShared<FJsonValueString>(Name);
		if (K == TEXT("input")) Inputs.Add(Val); else Outputs.Add(Val);
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("custom_inputs"), Inputs);
	Result->SetArrayField(TEXT("custom_outputs"), Outputs);
	Result->SetNumberField(TEXT("count"), Inputs.Num() + Outputs.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindSubgraphCallers(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString TargetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("target_asset_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("target_asset_path is required"));
	}

	FString PathFilter;
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	FString SQL = TEXT(
		"SELECT host_fa_path, host_node_guid, target_params_path "
		"FROM flow_subgraph_refs WHERE target_fa_path = ?");
	int32 NextBind = 2;
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND host_fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY host_fa_path, host_node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_subgraph_callers SQL"));
	}
	Stmt.SetBindingValueByIndex(1, TargetPath);
	(void)NextBind;

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString HostPath, HostGuid, ParamsPath;
		Stmt.GetColumnValueByIndex(0, HostPath);
		Stmt.GetColumnValueByIndex(1, HostGuid);
		Stmt.GetColumnValueByIndex(2, ParamsPath);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("host_fa_path"), HostPath);
		Row->SetStringField(TEXT("host_node_guid"), HostGuid);
		if (!ParamsPath.IsEmpty()) Row->SetStringField(TEXT("target_params_path"), ParamsPath);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("target_asset_path"), TargetPath);
	Result->SetArrayField(TEXT("callers"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindNodeClassUsages(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString NodeClass;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_class is required"));
	}

	FString PathFilter;
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	// Join flow_nodes with flow_assets to get host fa_path per matching node.
	FString SQL = TEXT(
		"SELECT fa.fa_path, n.node_guid, n.display_name "
		"FROM flow_nodes n "
		"JOIN flow_assets fa ON fa.fa_asset_id = n.fa_asset_id "
		"WHERE n.node_class = ?");
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND fa.fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY fa.fa_path, n.node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_node_class_usages SQL"));
	}
	Stmt.SetBindingValueByIndex(1, NodeClass);

	// Group on the way out by fa_path.
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> NodesByFa;
	int32 TotalNodes = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Guid, Display;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Guid);
		Stmt.GetColumnValueByIndex(2, Display);

		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_guid"), Guid);
		if (!Display.IsEmpty()) NodeObj->SetStringField(TEXT("display_name"), Display);

		NodesByFa.FindOrAdd(FaPath).Add(MakeShared<FJsonValueObject>(NodeObj));
		++TotalNodes;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> Hosts;
	for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : NodesByFa)
	{
		auto HostObj = MakeShared<FJsonObject>();
		HostObj->SetStringField(TEXT("fa_path"), Pair.Key);
		HostObj->SetArrayField(TEXT("nodes"), Pair.Value);
		HostObj->SetNumberField(TEXT("node_count"), Pair.Value.Num());
		Hosts.Add(MakeShared<FJsonValueObject>(HostObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_class"), NodeClass);
	Result->SetArrayField(TEXT("hosts"), Hosts);
	Result->SetNumberField(TEXT("host_count"), Hosts.Num());
	Result->SetNumberField(TEXT("total_node_count"), TotalNodes);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindPinsByType(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString PinTypeName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("pin_type_name"), PinTypeName) || PinTypeName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("pin_type_name is required"));
	}

	FString SubCat, Direction, PathFilter;
	Params->TryGetStringField(TEXT("pin_subcategory_object"), SubCat);
	Params->TryGetStringField(TEXT("pin_direction"), Direction);
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	FString SQL = TEXT(
		"SELECT fa.fa_path, p.node_guid, p.pin_direction, p.pin_index, p.pin_name, "
		"p.pin_type_name, p.pin_subcategory_object, p.container_type "
		"FROM flow_node_pins p "
		"JOIN flow_assets fa ON fa.fa_asset_id = p.fa_asset_id "
		"WHERE p.pin_type_name = ?");
	int32 NextBind = 2;
	if (!SubCat.IsEmpty())
	{
		SQL += TEXT(" AND p.pin_subcategory_object = ?");
	}
	if (!Direction.IsEmpty() && Direction != TEXT("all"))
	{
		SQL += TEXT(" AND p.pin_direction = ?");
	}
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND fa.fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY fa.fa_path, p.node_guid, p.pin_direction, p.pin_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_pins_by_type SQL"));
	}
	Stmt.SetBindingValueByIndex(1, PinTypeName);
	if (!SubCat.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, SubCat);
	if (!Direction.IsEmpty() && Direction != TEXT("all")) Stmt.SetBindingValueByIndex(NextBind++, Direction);

	TMap<FString, TArray<TSharedPtr<FJsonValue>>> PinsByFa;
	int32 TotalPins = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Guid, Dir, PinName, TypeName, SubCatRow, Container;
		int64 PinIdx = 0;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Guid);
		Stmt.GetColumnValueByIndex(2, Dir);
		Stmt.GetColumnValueByIndex(3, PinIdx);
		Stmt.GetColumnValueByIndex(4, PinName);
		Stmt.GetColumnValueByIndex(5, TypeName);
		Stmt.GetColumnValueByIndex(6, SubCatRow);
		Stmt.GetColumnValueByIndex(7, Container);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("pin_direction"), Dir);
		Row->SetNumberField(TEXT("pin_index"), PinIdx);
		Row->SetStringField(TEXT("pin_name"), PinName);
		Row->SetStringField(TEXT("pin_type_name"), TypeName);
		if (!SubCatRow.IsEmpty()) Row->SetStringField(TEXT("pin_subcategory_object"), SubCatRow);
		Row->SetStringField(TEXT("container_type"), Container);

		PinsByFa.FindOrAdd(FaPath).Add(MakeShared<FJsonValueObject>(Row));
		++TotalPins;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> Hosts;
	for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : PinsByFa)
	{
		auto HostObj = MakeShared<FJsonObject>();
		HostObj->SetStringField(TEXT("fa_path"), Pair.Key);
		HostObj->SetArrayField(TEXT("pins"), Pair.Value);
		HostObj->SetNumberField(TEXT("pin_count"), Pair.Value.Num());
		Hosts.Add(MakeShared<FJsonValueObject>(HostObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("pin_type_name"), PinTypeName);
	if (!SubCat.IsEmpty()) Result->SetStringField(TEXT("pin_subcategory_object"), SubCat);
	Result->SetArrayField(TEXT("hosts"), Hosts);
	Result->SetNumberField(TEXT("host_count"), Hosts.Num());
	Result->SetNumberField(TEXT("total_pin_count"), TotalPins);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindNodesByProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString ValueSub;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("value_substring"), ValueSub) || ValueSub.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("value_substring is required"));
	}

	FString PropName, NodeClassFilter, PathFilter;
	Params->TryGetStringField(TEXT("property_name"), PropName);
	Params->TryGetStringField(TEXT("node_class"), NodeClassFilter);
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	// Escape SQL LIKE wildcards in user input — we want literal substring match. ESCAPE char is backslash.
	auto EscapeLike = [](const FString& In) -> FString
	{
		return In
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("%"),  TEXT("\\%"))
			.Replace(TEXT("_"),  TEXT("\\_"));
	};

	const FString EscValue = EscapeLike(ValueSub);
	FString DataPattern;
	if (!PropName.IsEmpty())
	{
		const FString EscProp = EscapeLike(PropName);
		DataPattern = FString::Printf(TEXT("%%\"%s\"%%%s%%"), *EscProp, *EscValue);
	}
	else
	{
		DataPattern = FString::Printf(TEXT("%%%s%%"), *EscValue);
	}

	FString SQL = TEXT(
		"SELECT fa.fa_path, n.node_guid, n.node_class, n.display_name, n.data "
		"FROM flow_nodes n "
		"JOIN flow_assets fa ON fa.fa_asset_id = n.fa_asset_id "
		"WHERE n.data LIKE ? ESCAPE '\\'");
	int32 NextBind = 2;
	if (!NodeClassFilter.IsEmpty())
	{
		SQL += TEXT(" AND n.node_class = ?");
	}
	if (!PathFilter.IsEmpty())
	{
		const FString FaLike = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND fa.fa_path LIKE '%s'"), *FaLike);
	}
	SQL += TEXT(" ORDER BY fa.fa_path, n.node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_nodes_by_property SQL"));
	}
	Stmt.SetBindingValueByIndex(1, DataPattern);
	if (!NodeClassFilter.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, NodeClassFilter);

	TMap<FString, TArray<TSharedPtr<FJsonValue>>> NodesByFa;
	int32 TotalNodes = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Guid, Cls, Display, DataJson;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Guid);
		Stmt.GetColumnValueByIndex(2, Cls);
		Stmt.GetColumnValueByIndex(3, Display);
		Stmt.GetColumnValueByIndex(4, DataJson);

		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_guid"), Guid);
		NodeObj->SetStringField(TEXT("node_class"), Cls);
		if (!Display.IsEmpty()) NodeObj->SetStringField(TEXT("display_name"), Display);
		NodeObj->SetField(TEXT("data"), TryParseJsonValue(DataJson));

		NodesByFa.FindOrAdd(FaPath).Add(MakeShared<FJsonValueObject>(NodeObj));
		++TotalNodes;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> Hosts;
	for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : NodesByFa)
	{
		auto HostObj = MakeShared<FJsonObject>();
		HostObj->SetStringField(TEXT("fa_path"), Pair.Key);
		HostObj->SetArrayField(TEXT("nodes"), Pair.Value);
		HostObj->SetNumberField(TEXT("node_count"), Pair.Value.Num());
		Hosts.Add(MakeShared<FJsonValueObject>(HostObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("value_substring"), ValueSub);
	if (!PropName.IsEmpty()) Result->SetStringField(TEXT("property_name"), PropName);
	Result->SetArrayField(TEXT("hosts"), Hosts);
	Result->SetNumberField(TEXT("host_count"), Hosts.Num());
	Result->SetNumberField(TEXT("total_node_count"), TotalNodes);
	return FMonolithActionResult::Success(Result);
}
