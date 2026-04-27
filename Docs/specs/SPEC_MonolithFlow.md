# Monolith — MonolithFlow Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)
**Status:** **Fork-only** — maintained downstream in `feature/flow-graph-handler`, not proposed upstream (Flow's user base is small and has overlapping scope with `MonolithLogicDriver`). Periodically rebased on `upstream/master`.

---

## MonolithFlow

**Dependencies:** Core, CoreUObject, Engine, Flow, FlowEditor, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, BlueprintGraph, Kismet, EditorSubsystem, Json, JsonUtilities, GameplayTags, AssetRegistry
**Namespace:** `flow` | **Tool:** `flow_query(action, params)` | **Actions:** 14
**Conditional:** MothCocoon Flow plugin features wrapped in `#if WITH_FLOW`. When Flow is absent, the module compiles to an empty stub (0 actions registered). Build.cs detection at 3 locations (project plugins, engine marketplace, engine plugins).
**Settings toggles:** `bEnableFlow` + `bIndexFlow` (both default: True)

MonolithFlow indexes the [MothCocoon Flow plugin](https://github.com/MothCocoon/FlowGraph) into the central Monolith SQLite database and exposes 14 read-only query actions. Three of four Flow asset types are covered: `UFlowAsset` (graph), `UFlowNodeBlueprint` (BP-defined node class), `UFlowNodeAddOnBlueprint` (BP-defined addon class). `UFlowAssetParams` is intentionally excluded as still experimental upstream.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Asset listing | 2 | `list_assets` (with glob filter), `get_asset_info` (counts, class breakdown, sample nodes) |
| Graph walk | 5 | `list_nodes`, `list_node_pins`, `list_connections`, `list_addons`, `get_node_info` |
| Cross-asset reverse lookup | 5 | `list_custom_events`, `find_subgraph_callers`, `find_node_class_usages`, `find_pins_by_type`, `find_nodes_by_property` |
| Class registry | 1 | `list_node_classes` (BP + native, with kind / source / glob filters) |
| Smoke | 1 | `ping` |

### Database Schema (8 tables)

| Table | Purpose | Cleanup key |
|-------|---------|-------------|
| `flow_assets` | One row per indexed UFlowAsset (counts, owner class, base params path, asset GUID) | `fa_path` |
| `flow_nodes` | One row per UFlowNode (class, display name, BP flag/path, pin counts, **`data` JSON snapshot of every UPROPERTY**) | `fa_asset_id` |
| `flow_node_pins` | One row per pin (input + output, current + orphan), with full type info from `FFlowPin` | `fa_asset_id` |
| `flow_node_addons` | Recursive addon tree (UFlowNodeAddOn under nodes; per-addon `data` JSON snapshot) | `fa_asset_id` |
| `flow_node_connections` | Flat outgoing-edge list (current + orphan, with `is_orphaned` flag) | `fa_asset_id` |
| `flow_subgraph_refs` | Cross-asset reverse index for `UFlowNode_SubGraph` references (host_fa_path → target_fa_path) | `host_fa_asset_id` |
| `flow_custom_events` | Custom Inputs / Custom Outputs of each Flow Asset (entry / exit names exposed to parent SubGraph nodes) | `fa_asset_id` |
| `flow_node_classes` | Registry of every UFlowNode / UFlowNodeAddOn class referenced (BP + opportunistic native, `class_path` UNIQUE) | `class_path` (BP refresh) / `INSERT OR IGNORE` (native) |

### Key Actions

> **`get_node_info` deep dive.** Joins `flow_nodes` row + per-pin info + outgoing connections + recursive addon tree + parsed `data` JSON (full UPROPERTY snapshot serialized via `FJsonObjectConverter::UStructToJsonObjectString` with `CPF_Transient | CPF_DuplicateTransient` skipped). Single call returns everything needed to reason about one node's design-time state.
>
> **`find_nodes_by_property`.** SQL `LIKE` substring search over the per-node `data` JSON. Optional `property_name` scopes the match to `"property_name":...value` (any nesting). LIKE wildcards in user input are escaped via `ESCAPE '\'` so the substring is treated literally. Powers questions like "find every Narrator referencing DataTable X" or "every node where rowName contains podoidite".
>
> **`find_subgraph_callers` + `find_node_class_usages`.** Cross-asset reverse lookups. The first asks "which Flow Assets host a `UFlowNode_SubGraph` pointing at target Y?", the second "which Flow Assets host a node of UClass X?". Both group results by host fa_path with per-node guids — useful for refactoring scope ("what scenarios use my custom Dialogue node?").
>
> **`list_node_classes`.** Registry combining BP-defined node classes (full row from UFlowNodeBlueprint asset indexing) and native classes encountered opportunistically during graph walks. Yields `display_name`, `category`, `description` extracted from CDO + UCLASS metadata. Optional filters: `kind` (node / addon / all), `source` (blueprint / native / all), `class_path_filter` (glob).

### Notes

> **Reflection-only access to private members.** `UFlowNode_SubGraph::Asset` and `::AssetParams` are private `TSoftObjectPtr` UPROPERTYs, and `UFlowNode::Connections` is a protected `TMap<FName, FConnectedPin>` UPROPERTY. All three are read via `FProperty::GetPropertyValue_InContainer` / `FMapProperty + FScriptMapHelper`, never linking against Flow internals beyond the public headers. Pattern matches MonolithLogicDriver — version-agnostic as long as property names stay stable.
>
> **Orphan output pins.** UFlowNode subclasses with dynamic pin generation (e.g. `UFlowNode_SwitchOnScenario` reading from a GameplayTag root) keep stale per-tag output pins after the underlying tag is removed (`bOrphanedPin=True` in the editor wrapper, `PinCategory="exec"` lowercase). The runtime `Connections` map still has entries for those orphan pin names. Flat `flow_node_pins` / `flow_node_connections` capture both current and orphan rows; the new `is_orphaned` column distinguishes them. The full topology also lives inside `flow_nodes.data.connections` as a backup. Orphan input pins are not addressable — they're an editor-wrapper artifact (CustomProperties Pin entries on FlowGraphNode) with no corresponding state on the runtime FlowNode.
>
> **No FK on `assets(id)`.** Mirrors the rest of Monolith — would block `FMonolithIndexDatabase::ResetDatabase()` on full reindex. Cleanup keyed on stable `fa_path` with previous `fa_asset_id` captured before the asset row is rewritten (the autoincrement restarts after a full reset).
>
> **Data JSON snapshot semantics.** Every node and addon stores a compact JSON of its design-time UPROPERTYs. SkipFlags = `CPF_Transient | CPF_DuplicateTransient`. `CPF_DisableEditOnInstance` is intentionally NOT skipped — some Flow-internal UPROPERTYs that drive runtime behavior have it set. Consumers should treat this column as the canonical full-fidelity record; flat tables (pins, connections, custom_events, subgraph_refs) are normalized views over the most useful subset.
>
> **`UFlowAssetParams` deferred.** The fourth Flow asset type is still experimental upstream and changes shape between minor versions. `flow_assets.base_params_path` carries `BaseAssetParams.AssetPtr.ToSoftObjectPath()` so callers can join, but the params asset itself isn't introspected.
>
> **Schema migration.** Schema additions (e.g. `is_orphaned` on `flow_node_pins` / `flow_node_connections` in 0.14.7) ship via `ALTER TABLE ADD COLUMN` inside `EnsureTablesExist`. The ALTER silently no-ops on fresh DBs where the column already exists, so existing project DBs upgrade without a forced full reindex.

---
