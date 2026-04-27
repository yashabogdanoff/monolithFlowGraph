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
		const FString& ToPin,
		bool bIsOrphaned = false)
	{
		Stmt.Reset();
		Stmt.ClearBindings();
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, FromNodeGuid);
		Stmt.SetBindingValueByIndex(3, FromPin);
		Stmt.SetBindingValueByIndex(4, ToNodeGuid);
		Stmt.SetBindingValueByIndex(5, ToPin);
		Stmt.SetBindingValueByIndex(6, bIsOrphaned ? 1 : 0);
		Stmt.Execute();
	}

	// Forward decl — defined further down (used by WalkAndInsertAddOns for opportunistic native registry rows).
	void TryRegisterNativeNodeClass(
		FSQLiteDatabase* RawDB,
		FSQLitePreparedStatement& Stmt,
		UClass* NodeCls,
		const FString& ClassPath,
		const TCHAR* Kind);

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
		FSQLitePreparedStatement* NativeClassRegStmt,
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

			// Opportunistic native-class registration. BP-defined addons are registered fully
			// when the BP asset itself is indexed; we skip them here to avoid stale is_native=1 rows.
			if (NativeClassRegStmt)
			{
				FString DummyBpPath;
				if (!IsBlueprintNodeClass(AddOn->GetClass(), DummyBpPath))
				{
					TryRegisterNativeNodeClass(RawDB, *NativeClassRegStmt, AddOn->GetClass(), AddOnClassPath, TEXT("addon"));
				}
			}

			// Recurse — addons can have child addons.
			Total += WalkAndInsertAddOns(RawDB, InsertStmt, NativeClassRegStmt, FaAssetId, AddOn, OwnerNodeGuid, NewRowId, Depth + 1);
		}
		return Total;
	}

	/** Read a string-typed UPROPERTY (FString or FText) by name on a class CDO; empty if missing. */
	FString ReadStringPropertyOnCDO(UClass* Cls, const TCHAR* PropName)
	{
		if (!Cls) return FString();
		UObject* CDO = Cls->GetDefaultObject(/*bCreateIfNeeded=*/false);
		if (!CDO) return FString();
		FProperty* Prop = Cls->FindPropertyByName(PropName);
		if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return StrProp->GetPropertyValue_InContainer(CDO);
		}
		if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			return TextProp->GetPropertyValue_InContainer(CDO).ToString();
		}
		return FString();
	}

	/** Extract registry metadata for a UFlowNodeBase-derived UClass (display, category, description, super, abstract flag). */
	void GatherFlowClassMetadata(
		UClass* Cls,
		FString& OutDisplayName,
		FString& OutCategory,
		FString& OutDescription,
		FString& OutParentClass,
		bool& OutIsAbstract)
	{
		OutDisplayName.Reset();
		OutCategory.Reset();
		OutDescription.Reset();
		OutParentClass.Reset();
		OutIsAbstract = false;
		if (!Cls) return;

		OutDisplayName = Cls->GetDisplayNameText().ToString();
		if (UClass* Super = Cls->GetSuperClass())
		{
			OutParentClass = Super->GetPathName();
		}
		OutIsAbstract = Cls->HasAnyClassFlags(CLASS_Abstract);

		// UFlowNodeBase exposes Category as a CDO field on most versions; fall back to UCLASS metadata.
		OutCategory = ReadStringPropertyOnCDO(Cls, TEXT("Category"));
#if WITH_EDITORONLY_DATA
		if (OutCategory.IsEmpty() && Cls->HasMetaData(TEXT("Category")))
		{
			OutCategory = Cls->GetMetaData(TEXT("Category"));
		}
#endif

		OutDescription = ReadStringPropertyOnCDO(Cls, TEXT("NodeDescription"));
#if WITH_EDITORONLY_DATA
		if (OutDescription.IsEmpty() && Cls->HasMetaData(TEXT("ToolTip")))
		{
			OutDescription = Cls->GetMetaData(TEXT("ToolTip"));
		}
#endif
	}

	/** INSERT OR IGNORE one native node-class row into flow_node_classes (used during graph walk). */
	void TryRegisterNativeNodeClass(
		FSQLiteDatabase* RawDB,
		FSQLitePreparedStatement& Stmt,
		UClass* NodeCls,
		const FString& ClassPath,
		const TCHAR* Kind)
	{
		if (!RawDB || !NodeCls) return;
		FString Disp, Cat, Desc, Parent;
		bool bAbs = false;
		GatherFlowClassMetadata(NodeCls, Disp, Cat, Desc, Parent, bAbs);

		Stmt.Reset();
		Stmt.ClearBindings();
		Stmt.SetBindingValueByIndex(1, ClassPath);
		Stmt.SetBindingValueByIndex(2, FString(Kind));
		BindNullableString(Stmt, 3, Parent);
		BindNullableString(Stmt, 4, Disp);
		BindNullableString(Stmt, 5, Cat);
		BindNullableString(Stmt, 6, Desc);
		Stmt.SetBindingValueByIndex(7, bAbs ? 1 : 0);
		Stmt.Execute();
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
			"pin_type_name, pin_subcategory_object, container_type, tooltip, is_orphaned"
			") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0)")))
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

	/** Insert one orphan output pin row (pin name from Connections map that isn't in current OutputPins). */
	void InsertOrphanOutputPin(
		FSQLitePreparedStatement& Stmt,
		int64 FaAssetId,
		const FString& NodeGuid,
		int32 PinIdx,
		const FName& PinName)
	{
		Stmt.Reset();
		Stmt.ClearBindings();
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		Stmt.SetBindingValueByIndex(3, FString(TEXT("output")));
		Stmt.SetBindingValueByIndex(4, PinIdx);
		Stmt.SetBindingValueByIndex(5, PinName.ToString());
		Stmt.Execute();
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
		"  tooltip TEXT,"
		"  is_orphaned INTEGER NOT NULL DEFAULT 0"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_pins_fa ON flow_node_pins(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_pins_node ON flow_node_pins(node_guid)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_pins_type ON flow_node_pins(pin_type_name)"));
	// Migrate existing tables (silently no-ops on fresh DB where column already exists).
	RawDB->Execute(TEXT("ALTER TABLE flow_node_pins ADD COLUMN is_orphaned INTEGER NOT NULL DEFAULT 0"));

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
		"  to_pin TEXT NOT NULL,"
		"  is_orphaned INTEGER NOT NULL DEFAULT 0"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_conn_fa ON flow_node_connections(fa_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_conn_from ON flow_node_connections(from_node_guid)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_conn_to ON flow_node_connections(to_node_guid)"));
	// Migrate existing tables (silently no-ops on fresh DB).
	RawDB->Execute(TEXT("ALTER TABLE flow_node_connections ADD COLUMN is_orphaned INTEGER NOT NULL DEFAULT 0"));

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

	// Registry of every UFlowNode / UFlowNodeAddOn UClass referenced by the project.
	// Two sources populate this:
	//   - UFlowNodeBlueprint / UFlowNodeAddOnBlueprint assets indexed via IndexAsset (full BP row, is_native=0)
	//   - native classes encountered during a Flow Asset graph walk (INSERT OR IGNORE, is_native=1)
	// Lookup key is class_path so flow_nodes.node_class joins straight here.
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS flow_node_classes ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  bp_asset_id INTEGER NOT NULL DEFAULT 0,"
		"  class_path TEXT NOT NULL UNIQUE,"
		"  bp_path TEXT,"
		"  kind TEXT NOT NULL,"
		"  parent_class_path TEXT,"
		"  display_name TEXT,"
		"  category TEXT,"
		"  description TEXT,"
		"  is_native INTEGER NOT NULL DEFAULT 0,"
		"  is_abstract INTEGER NOT NULL DEFAULT 0"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_classes_kind ON flow_node_classes(kind)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_flow_classes_native ON flow_node_classes(is_native)"));

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
		// UFlowNodeBlueprint / UFlowNodeAddOnBlueprint — populate flow_node_classes (BP row).
		UBlueprint* BP = Cast<UBlueprint>(LoadedAsset);
		if (!BP) return true;

		UClass* GeneratedClass = BP->GeneratedClass;
		if (!GeneratedClass) return true;

		const bool bIsAddOn = GeneratedClass->IsChildOf(UFlowNodeAddOn::StaticClass());
		const bool bIsNode  = !bIsAddOn && GeneratedClass->IsChildOf(UFlowNode::StaticClass());
		if (!bIsNode && !bIsAddOn) return true;
		const TCHAR* KindStr = bIsAddOn ? TEXT("addon") : TEXT("node");

		const FString BpPath    = BP->GetPathName();
		const FString ClassPath = GeneratedClass->GetPathName();

		FString DisplayName, Category, Description, ParentClass;
		bool bIsAbstract = false;
		GatherFlowClassMetadata(GeneratedClass, DisplayName, Category, Description, ParentClass, bIsAbstract);

		// Refresh: drop any prior row keyed on the same class_path or bp_path.
		{
			FSQLitePreparedStatement Stmt;
			if (Stmt.Create(*RawDB, TEXT("DELETE FROM flow_node_classes WHERE class_path = ? OR bp_path = ?")))
			{
				Stmt.SetBindingValueByIndex(1, ClassPath);
				Stmt.SetBindingValueByIndex(2, BpPath);
				Stmt.Execute();
				Stmt.Destroy();
			}
		}

		FSQLitePreparedStatement InsStmt;
		if (!InsStmt.Create(*RawDB, TEXT(
			"INSERT INTO flow_node_classes ("
			"bp_asset_id, class_path, bp_path, kind, parent_class_path, "
			"display_name, category, description, is_native, is_abstract"
			") VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?)")))
		{
			return true;
		}
		InsStmt.SetBindingValueByIndex(1, AssetId);
		InsStmt.SetBindingValueByIndex(2, ClassPath);
		InsStmt.SetBindingValueByIndex(3, BpPath);
		InsStmt.SetBindingValueByIndex(4, FString(KindStr));
		BindNullableString(InsStmt, 5, ParentClass);
		BindNullableString(InsStmt, 6, DisplayName);
		BindNullableString(InsStmt, 7, Category);
		BindNullableString(InsStmt, 8, Description);
		InsStmt.SetBindingValueByIndex(9, bIsAbstract ? 1 : 0);
		InsStmt.Execute();
		InsStmt.Destroy();

		UE_LOG(LogMonolithFlowIndexer, Verbose, TEXT("FMonolithFlowIndexer: registered Flow %s blueprint %s -> %s"),
			KindStr, *BpPath, *ClassPath);
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

	// Opportunistic native node-class registry. INSERT OR IGNORE keeps it idempotent
	// across re-indexing of multiple Flow Assets that share the same node classes.
	FSQLitePreparedStatement NativeClassRegStmt;
	const bool bNativeRegStmtOk = NativeClassRegStmt.Create(*RawDB, TEXT(
		"INSERT OR IGNORE INTO flow_node_classes ("
		"bp_asset_id, class_path, bp_path, kind, parent_class_path, "
		"display_name, category, description, is_native, is_abstract"
		") VALUES (0, ?, NULL, ?, ?, ?, ?, ?, 1, ?)"));

	if (!bConnStmtOk || !bAddOnStmtOk || !bSubGraphStmtOk || !bNativeRegStmtOk)
	{
		UE_LOG(LogMonolithFlowIndexer, Warning, TEXT("FMonolithFlowIndexer: failed to prepare connections/addons/subgraph/class-reg INSERTs"));
		NodeStmt.Destroy();
		if (bConnStmtOk) ConnStmt.Destroy();
		if (bAddOnStmtOk) AddOnStmt.Destroy();
		if (bSubGraphStmtOk) SubGraphStmt.Destroy();
		if (bNativeRegStmtOk) NativeClassRegStmt.Destroy();
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

		// Opportunistic native node-class registration (BP-defined nodes are registered fully
		// on UFlowNodeBlueprint asset indexing — skipping them here keeps is_native correct).
		if (!bIsBp)
		{
			TryRegisterNativeNodeClass(RawDB, NativeClassRegStmt, Node->GetClass(), NodeClassPath, TEXT("node"));
		}

		// Walk addons first so we know the count before inserting the node row.
		const int32 NodeAddonCount = WalkAndInsertAddOns(
			RawDB, AddOnStmt, &NativeClassRegStmt, AssetId, Node, NodeGuidStr,
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
		TSet<FName> CurrentOutPinNames;
		CurrentOutPinNames.Reserve(OutPins.Num());
		for (const FFlowPin& OutPin : OutPins)
		{
			CurrentOutPinNames.Add(OutPin.PinName);

			const FConnectedPin Connected = Node->GetConnection(OutPin.PinName);
			if (!Connected.NodeGuid.IsValid()) continue;

			InsertConnection(ConnStmt, AssetId,
				NodeGuidStr,
				OutPin.PinName.ToString(),
				Connected.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
				Connected.PinName.ToString(),
				/*bIsOrphaned=*/false);
		}

		// Orphan output pins — entries in the Connections map whose key isn't a current OutputPin.
		// These are stale edges left over from previous versions of the node's pin set
		// (e.g. SwitchOnScenario keeps per-scenario output pins even after a scenario was removed,
		// flagged as bOrphanedPin=True in the editor wrapper). Read Connections via FMapProperty
		// reflection (it's protected, no public accessor for the whole map).
		if (FMapProperty* ConnectionsProp = CastField<FMapProperty>(
				Node->GetClass()->FindPropertyByName(TEXT("Connections"))))
		{
			void* MapAddr = ConnectionsProp->ContainerPtrToValuePtr<void>(Node);
			FScriptMapHelper Helper(ConnectionsProp, MapAddr);

			FSQLitePreparedStatement OrphanPinStmt;
			const bool bOrphanStmtOk = OrphanPinStmt.Create(*RawDB, TEXT(
				"INSERT INTO flow_node_pins ("
				"fa_asset_id, node_guid, pin_direction, pin_index, pin_name, "
				"pin_type_name, container_type, is_orphaned"
				") VALUES (?, ?, ?, ?, ?, 'Exec', 'None', 1)"));

			int32 OrphanIdx = OutPins.Num();
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i)) continue;
				const FName* PinNamePtr = reinterpret_cast<const FName*>(Helper.GetKeyPtr(i));
				const FConnectedPin* ConnectedPtr = reinterpret_cast<const FConnectedPin*>(Helper.GetValuePtr(i));
				if (!PinNamePtr || !ConnectedPtr) continue;
				if (CurrentOutPinNames.Contains(*PinNamePtr)) continue;
				if (!ConnectedPtr->NodeGuid.IsValid()) continue;

				if (bOrphanStmtOk)
				{
					InsertOrphanOutputPin(OrphanPinStmt, AssetId, NodeGuidStr, OrphanIdx, *PinNamePtr);
				}

				InsertConnection(ConnStmt, AssetId,
					NodeGuidStr,
					PinNamePtr->ToString(),
					ConnectedPtr->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					ConnectedPtr->PinName.ToString(),
					/*bIsOrphaned=*/true);

				++OrphanIdx;
			}
			if (bOrphanStmtOk) OrphanPinStmt.Destroy();
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
	NativeClassRegStmt.Destroy();

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
