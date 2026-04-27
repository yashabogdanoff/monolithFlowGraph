---
name: unreal-flow
description: Use when reading or analyzing MothCocoon Flow plugin assets via Monolith MCP — listing scenarios, walking node graphs, finding cross-asset references, locating dynamic-pin nodes, inspecting per-node design-time properties. Triggers on Flow, FlowAsset, FlowNode, FlowGraph, scenario, narrative, SubGraph, custom event, FlowComponent, IdentityTag.
---

# Unreal Flow Workflows

**14 read-only Flow actions** via `flow_query()`. Indexes the [MothCocoon Flow plugin](https://github.com/MothCocoon/FlowGraph) into Monolith's central SQLite DB. Discover with `monolith_discover({ namespace: "flow" })`.

**Conditional on the Flow plugin.** When Flow is absent, the module compiles to an empty stub (`#if WITH_FLOW`); 0 actions registered. Build.cs detects at 3 locations (project plugins, engine marketplace, engine plugins).

**Read-only.** This handler does not write — no asset CRUD, no graph editing. For graph authoring, use the Flow editor directly.

**Status:** Fork-only handler in `feature/flow-graph-handler` of `yashabogdanoff/monolithFlowGraph` — not in upstream Monolith.

## Key Parameters

- `asset_path` — Full UFlowAsset object path with the trailing `.AssetName` (e.g. `/Game/Flow/MyScenario.MyScenario`). Must match `UFlowAsset::GetPathName()`.
- `node_guid` — Node FGuid string in `DigitsWithHyphens` format (e.g. `4EBF9E55-4CE1-C76C-BB8C-7BA7C8DD9BAF`). Returned by every action that lists nodes.
- `node_class` — Full UClass path (e.g. `/Script/SIMULATOR.MyFlowNode_Custom` for native, `/Game/Foo/BPNode.BPNode_C` for BP-defined).
- `pin_type_name` — `FFlowPin` pin type (typically `Exec`; Flow plugin also defines typed data pins).
- `pin_subcategory_object` — Object/Class/Struct path for typed pins (e.g. `/Script/Engine.DataTable` for an Object pin pinned to UDataTable).
- `asset_path_filter` / `class_path_filter` — Glob (`*` and `?`) restricting host fa_path or class_path.

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Smoke (1)** | | |
| `ping` | — | Returns `{status:"ok", module:"MonolithFlow"}` |
| **Asset listing (2)** | | |
| `list_assets` | `asset_path_filter`? | Every UFlowAsset with node/addon/subgraph counts |
| `get_asset_info` | `asset_path` | Per-asset summary: counts (nodes, pins, addons, subgraphs), node-class breakdown, sample of 10 nodes, owner class, asset GUID, base params soft path |
| **Graph walk (5)** | | |
| `list_nodes` | `asset_path`, `node_class`? | Every UFlowNode in one asset with full `data` JSON snapshot of design-time UPROPERTYs |
| `list_node_pins` | `asset_path`, `node_guid`?, `pin_direction`?, `pin_type_name`? | Every pin (input + output, current + orphan) with `pin_type_name` + `pin_subcategory_object` + `is_orphaned` |
| `list_connections` | `asset_path`, `from_node_guid`?, `to_node_guid`? | Flat outgoing-edge list (current + orphan, with `is_orphaned` flag) |
| `list_addons` | `asset_path`, `node_guid`? | Recursive UFlowNodeAddOn tree under nodes (per-addon `data` JSON) |
| `get_node_info` | `asset_path`, `node_guid` | Single-node deep dive: row + parsed `data` + pins + outgoing connections + addon tree |
| **Cross-asset reverse lookup (5)** | | |
| `list_custom_events` | `asset_path`, `kind`? (`input`/`output`/`all`) | Custom Inputs / Custom Outputs of one Flow Asset (entry/exit names visible to parent SubGraph nodes) |
| `find_subgraph_callers` | `target_asset_path`, `asset_path_filter`? | Every Flow Asset that hosts a `UFlowNode_SubGraph` referencing the target |
| `find_node_class_usages` | `node_class`, `asset_path_filter`? | Every Flow Asset containing a node of the specified UClass, grouped by host fa_path with per-node guids + display names |
| `find_pins_by_type` | `pin_type_name`, `pin_subcategory_object`?, `pin_direction`?, `asset_path_filter`? | Every pin in the index whose `pin_type_name` matches |
| `find_nodes_by_property` | `value_substring`, `property_name`?, `node_class`?, `asset_path_filter`? | SQL `LIKE` substring search over the per-node `data` JSON. `property_name` scopes the match to `"property_name":...value` (any nesting). LIKE wildcards in input are escaped (`ESCAPE '\'`) so the substring is treated literally |
| **Class registry (1)** | | |
| `list_node_classes` | `kind`? (`node`/`addon`/`all`), `source`? (`blueprint`/`native`/`all`), `class_path_filter`? | Every UFlowNode / UFlowNodeAddOn class referenced by the project (BP rows from `UFlowNodeBlueprint` indexing + opportunistic native rows from graph walks). Returns class metadata extracted from CDO + UCLASS metadata: `display_name`, `category`, `description`, `parent_class_path`, `is_native`, `is_abstract` |

## Typical Workflow: "What's in this scenario?"

```
1. flow_query("get_asset_info", { "asset_path": "/Game/Flow/MyScenario.MyScenario" })
   → counts, node-class breakdown, sample nodes
2. flow_query("list_nodes", { "asset_path": "/Game/Flow/MyScenario.MyScenario" })
   → every node with full data JSON
3. flow_query("list_connections", { "asset_path": "/Game/Flow/MyScenario.MyScenario" })
   → full edge graph
4. flow_query("list_custom_events", { "asset_path": "/Game/Flow/MyScenario.MyScenario" })
   → custom entry/exit points (if used as a subgraph)
```

## Typical Workflow: "Who calls this scenario?"

```
1. flow_query("find_subgraph_callers", { "target_asset_path": "/Game/Flow/Shared/Dialogue.Dialogue" })
   → list of host fa_path + host_node_guid + target_params_path
```

## Typical Workflow: "What scenarios use my custom node?"

```
1. flow_query("find_node_class_usages", { "node_class": "/Script/MyGame.MyFlowNode_Custom" })
   → every host with per-node guids and display names
```

## Typical Workflow: "Find every Narrator referencing DataTable X"

```
1. flow_query("find_nodes_by_property", {
     "value_substring": "DT_MyNarrations",
     "property_name": "dataTable",
     "node_class": "/Script/MyGame.MyFlowNode_Narrator"
   })
   → matching nodes grouped by host fa_path with full data
```

## Typical Workflow: "Find every Object pin of class X"

```
1. flow_query("find_pins_by_type", {
     "pin_type_name": "Object",
     "pin_subcategory_object": "/Script/Engine.DataTable",
     "asset_path_filter": "/Game/Flow/*"
   })
   → matching pins grouped by host fa_path
```

## Caveats

- **`asset_path` requires the full object path** — `/Game/Foo/Bar.Bar`, not `/Game/Foo/Bar`. Mismatches return a clean error with a hint.
- **All Flow pins in most projects are `Exec`** — typed data pins are a newer Flow feature; `find_pins_by_type` may return empty for non-`Exec` types depending on project usage.
- **Orphan output pins** (e.g. on `UFlowNode_SwitchOnScenario` after a scenario tag is removed) are captured with `is_orphaned: true` flag. They're real ghost edges in the graph — useful for cleanup audits. The full topology also lives inside `flow_nodes.data.connections` as backup.
- **`UFlowAssetParams` is not indexed** — only `BaseAssetParams.AssetPtr.ToSoftObjectPath()` is captured on `flow_assets.base_params_path` so callers can join, but the params asset itself isn't introspected (still experimental upstream).
- **`data` JSON is the canonical record** — flat tables (pins, connections, custom_events, subgraph_refs) are normalized views over the most useful subset. For full-fidelity inspection of any UPROPERTY, read `flow_nodes.data` / `flow_node_addons.data`.
- **Read-only.** No asset writing, no graph editing. For graph CRUD, use the Flow editor directly.
- **Re-index trigger.** After saving Flow assets in the editor, the file watcher catches the change automatically. To force a re-index without saving, touch the `.uasset` file's mtime (e.g. `touch FLOW_*.uasset`) and call `monolith_reindex`.
