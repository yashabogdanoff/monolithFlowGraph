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
