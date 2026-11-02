#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""devtree_viz.py — 设备树 / 打通状态可视化

用法:
  python devtree_viz.py tree.yaml state.yaml [-o devtree.html] [--format html]
  python devtree_viz.py --import-dts extract/shennong_fdt.dts -o tree.yaml
  python devtree_viz.py tree.yaml state.yaml --check --strict

树文件描述硬件/驱动节点(基址、大小、IRQ、依赖等)，状态文件描述每个节点
当前是否打通。两者以节点 id 关联，状态文件可只覆盖部分节点。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import webbrowser
from dataclasses import dataclass, field
from datetime import datetime
from difflib import get_close_matches
from html import escape as h
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

VERSION = "1.0.0"

# --------------------------------------------------------------------------
# 状态定义（沿用项目 DXE-STATUS.md 的判定口径）
# --------------------------------------------------------------------------

STATUS_ORDER = ["ok", "partial", "patched", "fail", "todo", "unknown", "unused"]

STATUS: Dict[str, Dict[str, Any]] = {
    "ok":      {"icon": "✅", "label": "已打通", "color": "#3fb950", "avail": True,
                "desc": "有实机现象支撑，直接观察到或由可见结果传递性证明"},
    "partial": {"icon": "⚠️", "label": "部分可用", "color": "#d29922", "avail": False,
                "desc": "观察到部分成功，但有已知缺陷"},
    "patched": {"icon": "🔧", "label": "需补丁", "color": "#bc8cff", "avail": True,
                "desc": "默认路径不可用，打过补丁后才工作"},
    "fail":    {"icon": "⛔", "label": "实测失败", "color": "#f85149", "avail": False,
                "desc": "实测失败，有日志/寄存器证据"},
    "todo":    {"icon": "⏳", "label": "未开始", "color": "#58a6ff", "avail": False,
                "desc": "尚未开始验证或实现"},
    "unknown": {"icon": "❓", "label": "无证据", "color": "#8b949e", "avail": False,
                "desc": "没有任何实机证据，不要当成能用"},
    "unused":  {"icon": "🗑️", "label": "不适用", "color": "#6e7681", "avail": True,
                "desc": "本硬件/本阶段不需要"},
}

STATUS_ALIASES: Dict[str, str] = {
    "ok": "ok", "okay": "ok", "done": "ok", "pass": "ok", "works": "ok",
    "working": "ok", "up": "ok", "good": "ok", "✅": "ok", "打通": "ok",
    "完成": "ok", "正常": "ok", "已通": "ok",
    "partial": "partial", "part": "partial", "wip": "partial", "progress": "partial",
    "degraded": "partial", "⚠️": "partial", "⚠": "partial", "部分": "partial",
    "部分可用": "partial", "半通": "partial",
    "patched": "patched", "patch": "patched", "hack": "patched", "🔧": "patched",
    "补丁": "patched", "需补丁": "patched",
    "fail": "fail", "failed": "fail", "broken": "fail", "error": "fail",
    "⛔": "fail", "失败": "fail", "不通": "fail", "blocked": "fail",
    "todo": "todo", "pending": "todo", "not-started": "todo", "notstarted": "todo",
    "planned": "todo", "⏳": "todo", "待做": "todo", "未开始": "todo",
    "unknown": "unknown", "?": "unknown", "??": "unknown", "❓": "unknown",
    "未知": "unknown", "无证据": "unknown", "待查": "unknown",
    "unused": "unused", "n/a": "unused", "na": "unused", "skip": "unused",
    "irrelevant": "unused", "🗑️": "unused", "🗑": "unused", "不适用": "unused",
    "无需": "unused",
}

DEFAULT_BLOCKING = {"fail", "todo", "unknown"}
PATCH_KINDS = ("none", "stock", "binary", "source", "config", "disabled", "unknown")
PALETTE = ["#58a6ff", "#3fb950", "#d29922", "#bc8cff", "#f778ba", "#79c0ff",
           "#56d364", "#ffa657", "#a5d6ff", "#ff7b72", "#7ee787", "#d2a8ff"]

NODE_W, NODE_H, GAP_X, GAP_Y, MARGIN = 280, 98, 112, 20, 56
MAX_PER_COL = 12


class DataError(Exception):
    pass


# --------------------------------------------------------------------------
# 小工具
# --------------------------------------------------------------------------

def parse_int(value: Any, what: str) -> int:
    if isinstance(value, bool):
        raise DataError(f"{what}: 不能是布尔值 {value!r}")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        s = value.strip().replace("_", "")
        try:
            return int(s, 0)
        except ValueError:
            raise DataError(f"{what}: 无法解析数值 {value!r}（支持 0x1D84000 / 1234）") from None
    raise DataError(f"{what}: 期望整数或十六进制字符串，得到 {type(value).__name__}")


def fmt_addr(v: Optional[int]) -> str:
    if v is None:
        return "—"
    if v < 0x10000:
        return f"0x{v:X}"
    return f"0x{v:X}"


def fmt_size(v: Optional[int]) -> str:
    if v is None:
        return "—"
    if v == 0:
        return "0"
    units = [(1 << 30, "GiB"), (1 << 20, "MiB"), (1 << 10, "KiB")]
    for unit, name in units:
        if v >= unit and v % unit == 0:
            return f"{v // unit} {name}"
        if v >= unit:
            return f"{v / unit:.2f} {name}"
    return f"{v} B"


def text_width(s: str) -> int:
    return sum(2 if ord(c) > 0x2E7F else 1 for c in s)


def truncate(s: str, budget: int) -> str:
    if text_width(s) <= budget:
        return s
    out = ""
    for c in s:
        if text_width(out + c) > budget - 1:
            break
        out += c
    return out + "…"


def norm_status(value: Any) -> Optional[str]:
    if value is None:
        return None
    key = str(value).strip().lower()
    return STATUS_ALIASES.get(key)


def as_list(value: Any) -> List[Any]:
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return list(value)
    return [value]


def load_doc(path: Path) -> Any:
    if yaml is None:
        raise DataError("需要 PyYAML：pip install pyyaml")
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise DataError(f"无法读取 {path}: {exc}") from None
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise DataError(f"{path} 不是合法的 YAML/JSON:\n{exc}") from None


# --------------------------------------------------------------------------
# 数据模型
# --------------------------------------------------------------------------

@dataclass
class Node:
    id: str
    name: str
    group: str = "other"
    base: Optional[int] = None
    size: Optional[int] = None
    irq: List[int] = field(default_factory=list)
    compatible: str = ""
    driver: str = ""
    dt_path: str = ""
    deps: List[str] = field(default_factory=list)
    soft_deps: List[str] = field(default_factory=list)
    tags: List[str] = field(default_factory=list)
    patch: Dict[str, Any] = field(default_factory=dict)
    notes: str = ""
    status: str = "unknown"
    evidence: str = ""
    state_note: str = ""
    updated: str = ""
    no_overlap: bool = False
    index: int = -1

    @property
    def end(self) -> Optional[int]:
        if self.base is None or self.size is None or self.size <= 0:
            return None
        return self.base + self.size


@dataclass
class Model:
    nodes: List[Node]
    by_id: Dict[str, Node]
    groups: List[Dict[str, Any]]
    group_by_id: Dict[str, Dict[str, Any]]
    title: str
    subtitle: str
    updated: str
    tree_path: str
    state_path: str
    blocking: set = field(default_factory=lambda: set(DEFAULT_BLOCKING))
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    dependents: Dict[str, List[str]] = field(default_factory=dict)
    blockers: Dict[str, List[str]] = field(default_factory=dict)
    direct_blockers: Dict[str, List[str]] = field(default_factory=dict)
    layer_of: Dict[str, int] = field(default_factory=dict)
    pos: Dict[str, Tuple[int, int]] = field(default_factory=dict)
    conflicts: List[Dict[str, Any]] = field(default_factory=list)
    stats: Dict[str, Any] = field(default_factory=dict)
    canvas: Tuple[int, int] = (0, 0)


def build_model(tree_path: Path, state_path: Optional[Path], blocking: set) -> Model:
    doc = load_doc(tree_path)
    if not isinstance(doc, dict):
        raise DataError(f"{tree_path}: 顶层应为映射（meta/groups/nodes）")
    meta = doc.get("meta") or {}
    if not isinstance(meta, dict):
        raise DataError(f"{tree_path}: meta 应为映射")

    raw_nodes = doc.get("nodes")
    if not isinstance(raw_nodes, list) or not raw_nodes:
        raise DataError(f"{tree_path}: nodes 必须是非空列表")

    model = Model(
        nodes=[],
        by_id={},
        groups=[],
        group_by_id={},
        title=str(meta.get("title") or tree_path.stem),
        subtitle=str(meta.get("subtitle") or meta.get("platform") or ""),
        updated=str(meta.get("updated") or ""),
        tree_path=str(tree_path),
        state_path=str(state_path) if state_path else "",
        blocking=blocking,
    )

    _parse_groups(model, doc.get("groups"))
    _parse_nodes(model, raw_nodes, tree_path)
    if state_path is not None:
        _apply_state(model, state_path)

    _finalize(model)
    return model


def _parse_groups(model: Model, raw: Any) -> None:
    seen = set()
    if raw is not None and not isinstance(raw, list):
        raise DataError("groups 应为列表")
    for i, item in enumerate(raw or []):
        if isinstance(item, str):
            item = {"id": item}
        if not isinstance(item, dict) or not item.get("id"):
            raise DataError(f"groups[{i}]: 需要 id")
        gid = str(item["id"])
        if gid in seen:
            model.errors.append(f"groups: 重复的分组 id {gid!r}")
            continue
        seen.add(gid)
        model.groups.append({
            "id": gid,
            "label": str(item.get("label") or gid),
            "color": str(item.get("color") or PALETTE[len(model.groups) % len(PALETTE)]),
            "desc": str(item.get("desc") or ""),
        })
    model.group_by_id = {g["id"]: g for g in model.groups}


def _parse_nodes(model: Model, raw_nodes: List[Any], path: Path) -> None:
    for i, raw in enumerate(raw_nodes):
        if not isinstance(raw, dict):
            raise DataError(f"nodes[{i}]: 应为映射")
        nid = str(raw.get("id") or "").strip()
        if not nid:
            raise DataError(f"nodes[{i}]: 缺少 id")
        if nid in model.by_id:
            model.errors.append(f"节点 id 重复: {nid!r}（nodes[{i}]）")
            continue
        if not re.fullmatch(r"[A-Za-z0-9_.\-:/@]+", nid):
            model.warnings.append(f"节点 id {nid!r} 含特殊字符，建议只用字母/数字/._-:/@")

        name = str(raw.get("name") or nid)
        group = str(raw.get("group") or "other")
        base = raw.get("base")
        size = raw.get("size")
        base_i = parse_int(base, f"{nid}.base") if base is not None else None
        size_i = parse_int(size, f"{nid}.size") if size is not None else None
        if base_i is not None and size_i is None:
            model.warnings.append(f"{nid}: 有 base 但没有 size，地址重叠检测将跳过该项")
        if size_i is not None and base_i is None:
            model.warnings.append(f"{nid}: 有 size 但没有 base，已忽略 size")
            size_i = None
        if size_i is not None and size_i < 0:
            raise DataError(f"{nid}.size 不能为负")
        if base_i is not None and base_i < 0:
            raise DataError(f"{nid}.base 不能为负")

        irq: List[int] = []
        for j, v in enumerate(as_list(raw.get("irq"))):
            if v is None:
                continue
            irq.append(parse_int(v, f"{nid}.irq[{j}]"))
        if "irq" in raw and raw.get("irq") is not None and not isinstance(raw.get("irq"), (list, int, str)):
            model.warnings.append(f"{nid}.irq: 期望整数或列表")

        compat = raw.get("compatible")
        if isinstance(compat, (list, tuple)):
            compat_s = ", ".join(str(x) for x in compat)
        else:
            compat_s = str(compat or "")

        patch_raw = raw.get("patch")
        patch: Dict[str, Any] = {}
        if isinstance(patch_raw, str):
            patch = {"kind": patch_raw}
        elif isinstance(patch_raw, dict):
            patch = {str(k): v for k, v in patch_raw.items()}
        elif patch_raw is not None:
            model.warnings.append(f"{nid}.patch: 期望字符串或映射，已忽略")
        if patch:
            kind = str(patch.get("kind") or "").strip().lower()
            if kind not in PATCH_KINDS:
                model.errors.append(
                    f"{nid}.patch.kind 未知: {kind!r}（可用: {', '.join(PATCH_KINDS)}）")
            patch["kind"] = kind
            patch.setdefault("enabled", kind not in ("none", "disabled"))
            for key in ("ref", "note", "delta"):
                if patch.get(key) is not None:
                    patch[key] = str(patch[key])

        node = Node(
            id=nid,
            name=name,
            group=group,
            base=base_i,
            size=size_i,
            irq=irq,
            compatible=compat_s,
            driver=str(raw.get("driver") or ""),
            dt_path=str(raw.get("dt_path") or raw.get("device_path") or raw.get("path") or ""),
            deps=[str(x) for x in as_list(raw.get("deps")) if x],
            soft_deps=[str(x) for x in as_list(raw.get("soft_deps") or raw.get("optional_deps")) if x],
            tags=[str(x) for x in as_list(raw.get("tags")) if x],
            patch=patch,
            notes=str(raw.get("notes") or ""),
            evidence=str(raw.get("evidence") or ""),
            no_overlap=bool(raw.get("no_overlap")),
        )
        inline = raw.get("state", raw.get("status"))
        if inline is not None:
            st = norm_status(inline)
            if st is None:
                model.errors.append(f"{nid}: 未知状态 {inline!r}")
            else:
                node.status = st

        model.nodes.append(node)
        model.by_id[nid] = node

    if not model.nodes:
        raise DataError(f"{path}: 没有有效节点")

    for idx, node in enumerate(model.nodes):
        node.index = idx
        if node.group not in model.group_by_id:
            if model.groups:
                model.warnings.append(f"{node.id}: 分组 {node.group!r} 未在 groups 中声明")
            model.group_by_id[node.group] = {
                "id": node.group,
                "label": node.group,
                "color": PALETTE[len(model.group_by_id) % len(PALETTE)],
                "desc": "",
            }
    for g in model.group_by_id.values():
        if g not in model.groups:
            model.groups.append(g)


def _resolve_state_key(model: Model, key: str) -> Optional[Node]:
    if key in model.by_id:
        return model.by_id[key]
    for node in model.nodes:
        if node.dt_path and node.dt_path == key:
            return node
    low = key.lower()
    for node in model.nodes:
        if node.id.lower() == low:
            return node
    return None


def _apply_state(model: Model, state_path: Path) -> None:
    doc = load_doc(state_path)
    if not isinstance(doc, dict):
        raise DataError(f"{state_path}: 顶层应为映射（meta/states）")
    meta = doc.get("meta") or {}
    if isinstance(meta, dict) and meta.get("source") and not model.subtitle:
        model.subtitle = str(meta["source"])
    entries = doc.get("states", doc.get("state", doc.get("status")))
    if entries is None:
        raise DataError(f"{state_path}: 缺少 states 段")
    if not isinstance(entries, dict):
        raise DataError(f"{state_path}: states 应为 节点id -> 状态 的映射")

    ids = list(model.by_id)
    for key, value in entries.items():
        key_s = str(key)
        node = _resolve_state_key(model, key_s)
        if node is None:
            hint = get_close_matches(key_s, ids, n=2, cutoff=0.5)
            suffix = f"（是不是: {', '.join(hint)}）" if hint else ""
            model.errors.append(f"状态文件中的 {key_s!r} 在树里不存在{suffix}")
            continue

        if isinstance(value, str):
            st = norm_status(value)
            evidence = note = updated = ""
        elif isinstance(value, dict):
            st = norm_status(value.get("state", value.get("status")))
            evidence = str(value.get("evidence") or value.get("proof") or "")
            note = str(value.get("note") or value.get("notes") or "")
            updated = str(value.get("updated") or "")
            unknown = set(value) - {"state", "status", "evidence", "proof", "note", "notes",
                                    "updated", "owner", "since"}
            if unknown:
                model.warnings.append(f"{key_s}: 忽略未知字段 {sorted(unknown)}")
        else:
            model.errors.append(f"{key_s}: 状态值应为字符串或映射")
            continue
        if st is None:
            model.errors.append(f"{key_s}: 未知状态 {value!r}（可用: {', '.join(STATUS_ORDER)}）")
            continue
        node.status = st
        if evidence:
            node.evidence = evidence
        if note:
            node.state_note = note
        if updated:
            node.updated = updated


def _finalize(model: Model) -> None:
    ids = set(model.by_id)
    for node in model.nodes:
        seen = set()
        clean: List[str] = []
        for dep in node.deps:
            if dep == node.id:
                model.errors.append(f"{node.id}: 依赖自身")
                continue
            if dep not in ids:
                hint = get_close_matches(dep, list(ids), n=2, cutoff=0.5)
                suffix = f"（是不是: {', '.join(hint)}）" if hint else ""
                model.errors.append(f"{node.id}: 依赖 {dep!r} 不存在{suffix}")
                continue
            if dep in seen:
                model.warnings.append(f"{node.id}: 重复依赖 {dep!r}，已去重")
                continue
            seen.add(dep)
            clean.append(dep)
        node.deps = clean

        soft_seen = set()
        soft_clean: List[str] = []
        for dep in node.soft_deps:
            if dep == node.id:
                model.errors.append(f"{node.id}: 软依赖自身")
                continue
            if dep not in ids:
                model.errors.append(f"{node.id}: 软依赖 {dep!r} 不存在")
                continue
            if dep in seen:
                model.warnings.append(f"{node.id}: {dep!r} 同时是硬依赖和软依赖，按硬依赖处理")
                continue
            if dep in soft_seen:
                continue
            soft_seen.add(dep)
            soft_clean.append(dep)
        node.soft_deps = soft_clean

    if model.errors:
        return

    cycle = _find_cycle(model.by_id)
    if cycle:
        model.errors.append("依赖存在环: " + " -> ".join(cycle))

    for node in model.nodes:
        for dep in node.deps:
            model.dependents.setdefault(dep, []).append(node.id)
        for dep in node.soft_deps:
            model.dependents.setdefault(dep, []).append(node.id)

    if model.errors:
        return

    for node in model.nodes:
        upstream = _upstream(node.id, model.by_id)
        model.blockers[node.id] = sorted(
            (u for u in upstream if model.by_id[u].status in model.blocking),
            key=lambda x: model.by_id[x].index,
        )
        model.direct_blockers[node.id] = [
            d for d in node.deps if model.by_id[d].status in model.blocking
        ]

    model.layer_of = _compute_layers(model)
    _compute_conflicts(model)
    _compute_stats(model)
    _layout(model)


def _find_cycle(by_id: Dict[str, Node]) -> Optional[List[str]]:
    color: Dict[str, int] = {}
    stack: List[str] = []

    def dfs(u: str) -> Optional[List[str]]:
        color[u] = 1
        stack.append(u)
        for v in by_id[u].deps:
            if v not in by_id:
                continue
            if color.get(v, 0) == 1:
                return stack[stack.index(v):] + [v]
            if color.get(v, 0) == 0:
                found = dfs(v)
                if found:
                    return found
        color[u] = 2
        stack.pop()
        return None

    for nid in by_id:
        if color.get(nid, 0) == 0:
            found = dfs(nid)
            if found:
                return found
    return None


def _upstream(nid: str, by_id: Dict[str, Node]) -> List[str]:
    seen: set = set()
    queue = list(by_id[nid].deps)
    while queue:
        cur = queue.pop()
        if cur in seen or cur not in by_id:
            continue
        seen.add(cur)
        queue.extend(by_id[cur].deps)
    return list(seen)


def _compute_layers(model: Model) -> Dict[str, int]:
    memo: Dict[str, int] = {}

    def layer(nid: str, depth: int = 0) -> int:
        if nid in memo:
            return memo[nid]
        if depth > len(model.nodes) + 2:
            return 0
        node = model.by_id[nid]
        value = 0
        for dep in node.deps + node.soft_deps:
            if dep in model.by_id:
                value = max(value, layer(dep, depth + 1) + 1)
        memo[nid] = value
        return value

    sys.setrecursionlimit(max(10000, len(model.nodes) * 10))
    return {n.id: layer(n.id) for n in model.nodes}


def _compute_conflicts(model: Model) -> None:
    nodes = [n for n in model.nodes
             if n.base is not None and n.size and n.size > 0 and not n.no_overlap]
    for i, a in enumerate(nodes):
        for b in nodes[i + 1:]:
            lo = max(a.base, b.base)
            hi = min(a.end, b.end)
            if hi <= lo:
                continue
            contains = (a.base <= b.base and a.end >= b.end) or (b.base <= a.base and b.end >= a.end)
            model.conflicts.append({
                "a": a.id,
                "b": b.id,
                "kind": "contains" if contains else "partial",
                "bytes": hi - lo,
            })
    model.conflicts.sort(key=lambda c: (c["kind"] != "partial", -c["bytes"]))


def _compute_stats(model: Model) -> None:
    counts = {st: 0 for st in STATUS_ORDER}
    for node in model.nodes:
        counts[node.status] += 1
    total = len(model.nodes)
    unused = counts["unused"]
    applicable = total - unused
    done = counts["ok"] + counts["patched"]
    blocked = [n.id for n in model.nodes if model.blockers[n.id] and n.status != "unused"]
    contradictions = [n.id for n in model.nodes
                      if model.blockers[n.id] and STATUS[n.status]["avail"] and n.status != "unused"]
    partial_blocked = [n.id for n in model.nodes if model.direct_blockers[n.id] and n.status == "partial"]
    patch_counts: Dict[str, int] = {}
    for node in model.nodes:
        kind = node.patch.get("kind")
        if kind and kind != "none":
            patch_counts[kind] = patch_counts.get(kind, 0) + 1
    model.stats = {
        "total": total,
        "counts": counts,
        "applicable": applicable,
        "done": done,
        "progress": round(100.0 * done / applicable, 1) if applicable else 0.0,
        "blocked": len(blocked),
        "blocked_ids": blocked,
        "contradictions": contradictions,
        "partial_blocked": partial_blocked,
        "conflicts": len(model.conflicts),
        "hard_edges": sum(len(n.deps) for n in model.nodes),
        "soft_edges": sum(len(n.soft_deps) for n in model.nodes),
        "layers": (max(model.layer_of.values()) + 1) if model.layer_of else 0,
        "patches": patch_counts,
    }


def _layout(model: Model) -> None:
    if not model.layer_of:
        return
    max_layer = max(model.layer_of.values())
    layers: Dict[int, List[Node]] = {i: [] for i in range(max_layer + 1)}
    group_order = {g["id"]: i for i, g in enumerate(model.groups)}
    for node in model.nodes:
        layers[model.layer_of[node.id]].append(node)

    for items in layers.values():
        items.sort(key=lambda n: (group_order.get(n.group, 999), n.index))
    order = {n.id: i for items in layers.values() for i, n in enumerate(items)}

    for _ in range(3):
        for li in range(1, max_layer + 1):
            items = layers[li]
            if not items:
                continue

            def bary(n: Node) -> float:
                refs = [d for d in (n.deps + n.soft_deps) if d in model.by_id]
                if not refs:
                    return order.get(n.id, 0)
                return sum(order.get(d, 0) for d in refs) / len(refs)

            items.sort(key=lambda n: (bary(n), order.get(n.id, 0)))
            for i, n in enumerate(items):
                order[n.id] = i

    heights: Dict[int, int] = {}
    col_counts: Dict[int, int] = {}
    for li, items in layers.items():
        col_counts[li] = max(1, (len(items) + MAX_PER_COL - 1) // MAX_PER_COL)
        heights[li] = min(len(items), MAX_PER_COL) * (NODE_H + GAP_Y) - GAP_Y

    max_h = max(heights.values()) if heights else 0
    col_x: Dict[int, int] = {}
    x = MARGIN
    for li in range(max_layer + 1):
        col_x[li] = x
        x += col_counts.get(li, 1) * (NODE_W + GAP_X)

    for li, items in layers.items():
        for k, node in enumerate(items):
            chunk = k // MAX_PER_COL
            slot = k % MAX_PER_COL
            chunk_len = min(MAX_PER_COL, len(items) - chunk * MAX_PER_COL)
            chunk_h = chunk_len * (NODE_H + GAP_Y) - GAP_Y
            offset = (max_h - chunk_h) / 2
            nx = col_x[li] + chunk * (NODE_W + GAP_X)
            ny = MARGIN + offset + slot * (NODE_H + GAP_Y)
            model.pos[node.id] = (int(nx), int(ny))

    width = x - GAP_X + MARGIN
    height = MARGIN * 2 + max_h
    model.canvas = (int(width), int(height))


# --------------------------------------------------------------------------
# 渲染：HTML
# --------------------------------------------------------------------------

def _status_meta(st: str) -> Dict[str, Any]:
    return STATUS[st]


def _node_payload(model: Model, node: Node) -> Dict[str, Any]:
    x, y = model.pos.get(node.id, (0, 0))
    upstream = _upstream(node.id, model.by_id)
    blocked = bool(model.blockers[node.id])
    return {
        "i": node.index,
        "id": node.id,
        "name": node.name,
        "group": node.group,
        "group_label": model.group_by_id[node.group]["label"],
        "group_color": model.group_by_id[node.group]["color"],
        "status": node.status,
        "status_label": STATUS[node.status]["label"],
        "icon": STATUS[node.status]["icon"],
        "color": STATUS[node.status]["color"],
        "base": node.base,
        "size": node.size,
        "base_str": fmt_addr(node.base),
        "size_str": fmt_size(node.size),
        "range_str": (f"{fmt_addr(node.base)} – {fmt_addr(node.end)}"
                      if node.base is not None and node.end is not None else ""),
        "irq": node.irq,
        "compatible": node.compatible,
        "driver": node.driver,
        "dt_path": node.dt_path,
        "deps": node.deps,
        "soft_deps": node.soft_deps,
        "dependents": model.dependents.get(node.id, []),
        "blockers": model.blockers[node.id],
        "direct_blockers": model.direct_blockers[node.id],
        "blocked": blocked,
        "contradiction": blocked and STATUS[node.status]["avail"] and node.status != "unused",
        "notes": node.notes,
        "evidence": node.evidence,
        "state_note": node.state_note,
        "updated": node.updated,
        "tags": node.tags,
        "patch": node.patch,
        "no_overlap": node.no_overlap,
        "layer": model.layer_of.get(node.id, 0),
        "x": x,
        "y": y,
        "upstream": sorted(upstream, key=lambda x: model.by_id[x].index),
    }


def _payload(model: Model) -> Dict[str, Any]:
    nodes = [_node_payload(model, n) for n in model.nodes]
    edges = []
    for n in model.nodes:
        for dep in n.deps:
            edges.append({"f": model.by_id[dep].index, "t": n.index, "soft": False})
        for dep in n.soft_deps:
            edges.append({"f": model.by_id[dep].index, "t": n.index, "soft": True})
    group_counts = {g["id"]: 0 for g in model.groups}
    for n in model.nodes:
        group_counts[n.group] = group_counts.get(n.group, 0) + 1
    return {
        "title": model.title,
        "subtitle": model.subtitle,
        "updated": model.updated,
        "statuses": {st: {k: STATUS[st][k] for k in ("icon", "label", "color", "desc", "avail")}
                     for st in STATUS_ORDER},
        "status_order": STATUS_ORDER,
        "nodes": nodes,
        "edges": edges,
        "groups": [{"id": g["id"], "label": g["label"], "color": g["color"],
                    "desc": g["desc"], "count": group_counts.get(g["id"], 0)}
                   for g in model.groups],
        "stats": model.stats,
        "conflicts": model.conflicts,
        "layout": {"w": model.canvas[0], "h": model.canvas[1],
                   "node_w": NODE_W, "node_h": NODE_H, "gap": GAP_X},
        "sources": {"tree": model.tree_path, "state": model.state_path},
        "generated": datetime.now().strftime("%Y-%m-%d %H:%M"),
    }


def _svg(model: Model) -> str:
    out: List[str] = []
    out.append(f'<svg id="svg" width="100%" height="100%" xmlns="http://www.w3.org/2000/svg">')
    out.append("<defs>")
    out.append('<marker id="ar" viewBox="0 0 8 8" refX="7.2" refY="4" markerWidth="7" markerHeight="7" '
               'orient="auto-start-reverse"><path d="M0,0 L8,4 L0,8 z" fill="#4b545e"/></marker>')
    out.append('<marker id="ar-soft" viewBox="0 0 8 8" refX="7.2" refY="4" markerWidth="7" markerHeight="7" '
               'orient="auto-start-reverse"><path d="M0,0 L8,4 L0,8 z" fill="#3d444d"/></marker>')
    out.append('<marker id="ar-bad" viewBox="0 0 8 8" refX="7.2" refY="4" markerWidth="7" markerHeight="7" '
               'orient="auto-start-reverse"><path d="M0,0 L8,4 L0,8 z" fill="#f85149"/></marker>')
    out.append("</defs>")
    out.append('<g id="viewport">')

    for n in model.nodes:
        x1, y1 = model.pos[n.id]
        x1 += NODE_W
        y1 += NODE_H / 2
        for dep in n.deps + n.soft_deps:
            dx_, dy_ = model.pos[dep]
            x2, y2 = dx_, dy_ + NODE_H / 2
            gap = x2 - x1
            c1 = x1 + gap * 0.45
            c2 = x2 - gap * 0.45
            soft = dep in n.soft_deps
            bad = (not soft) and model.by_id[dep].status == "fail"
            cls = "edge" + (" soft" if soft else "") + (" bad" if bad else "")
            marker = "ar-soft" if soft else ("ar-bad" if bad else "ar")
            out.append(
                f'<path class="{cls}" data-f="{model.by_id[dep].index}" data-t="{n.index}" '
                f'marker-end="url(#{marker})" d="M{x1:.1f},{y1:.1f} C{c1:.1f},{y1:.1f} '
                f'{c2:.1f},{y2:.1f} {x2:.1f},{y2:.1f}"/>'
            )

    for node in model.nodes:
        x, y = model.pos[node.id]
        meta = STATUS[node.status]
        g = model.group_by_id[node.group]
        name = truncate(node.name, 30)
        addr = fmt_addr(node.base)
        size = fmt_size(node.size)
        line2 = f"{addr} · {size}" if node.base is not None else "无 MMIO 基址"
        driver = truncate(node.driver or "—", 34)
        group_label = truncate(g["label"], 18)
        patch_kind = str(node.patch.get("kind") or "")
        has_patch = patch_kind not in ("", "none")
        patch_enabled = bool(node.patch.get("enabled", True))
        status_label = meta["label"]
        blocked = bool(model.blockers[node.id])
        blocked_visual = blocked and node.status in ("ok", "patched", "partial", "todo")
        classes = "node" + (" blocked" if blocked_visual else "")
        out.append(f'<g class="{classes}" data-i="{node.index}" data-id="{h(node.id)}" '
                   f'transform="translate({x},{y})">')
        out.append(f'<rect class="card" width="{NODE_W}" height="{NODE_H}" rx="10"/>')
        out.append(f'<rect class="stripe" width="5" height="{NODE_H}" rx="2.5" fill="{meta["color"]}"/>')
        out.append(f'<circle cx="19" cy="17" r="3.5" fill="{g["color"]}"/>')
        out.append(f'<text class="nm" x="30" y="21">{h(name)}</text>')
        out.append(f'<text class="st" x="{NODE_W - 14}" y="20" text-anchor="end">{meta["icon"]}</text>')
        if blocked_visual:
            out.append(f'<text class="chain" x="{NODE_W - 40}" y="20" text-anchor="end">⛓</text>')
        out.append(f'<text class="addr" x="16" y="47">{h(line2)}</text>')
        out.append(f'<text class="meta" x="16" y="67">{h(driver)}</text>')
        if has_patch:
            mark_color = "#bc8cff" if patch_enabled else "#f85149"
            out.append(f'<text class="pmark" x="16" y="87" fill="{mark_color}">⚙</text>')
        out.append(f'<text class="grp" x="{30 if has_patch else 16}" y="87"'
                   f' fill="{g["color"]}">{h(group_label)}</text>')
        out.append(f'<text class="stl" x="{NODE_W - 14}" y="87" text-anchor="end" '
                   f'fill="{meta["color"]}">{h(status_label)}</text>')
        out.append("</g>")

    out.append("</g></svg>")
    return "\n".join(out)


HTML_CSS = r"""
:root{--bg:#0d1117;--panel:#0f141b;--card:#161b22;--border:#30363d;--fg:#e6edf3;--dim:#8b949e;--accent:#58a6ff}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);
  font-family:"Segoe UI","Microsoft YaHei","PingFang SC",system-ui,sans-serif;font-size:13px}
#app{display:flex;flex-direction:column;height:100vh;min-height:0}
header{display:flex;align-items:center;gap:14px;padding:10px 16px;border-bottom:1px solid #21262d;
  background:linear-gradient(180deg,#11161d,#0d1117);flex:0 0 auto;flex-wrap:wrap}
h1{font-size:15px;margin:0;font-weight:600;letter-spacing:.2px;white-space:nowrap}
.sub{color:var(--dim);font-size:11.5px;max-width:420px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
header .spacer{flex:1}
.hstat{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--dim);white-space:nowrap}
.hstat b{color:var(--fg);font-weight:600;font-variant-numeric:tabular-nums}
.sep{width:1px;height:18px;background:#21262d}
main{display:flex;flex:1;min-height:0}
#canvas{position:relative;flex:1;min-width:0;overflow:hidden;
  background-color:#0d1117;background-image:radial-gradient(#1c2330 1px,transparent 1px);background-size:22px 22px}
#svg{width:100%;height:100%;display:block;cursor:grab;touch-action:none}
#svg.drag{cursor:grabbing}
#mini-wrap{position:absolute;left:14px;bottom:14px;z-index:2;background:rgba(13,17,23,.88);
  border:1px solid #21262d;border-radius:8px;padding:4px;box-shadow:0 4px 14px rgba(0,0,0,.45)}
#mini{display:block;width:228px;height:62px;cursor:pointer}
#mini .vp{fill:rgba(88,166,255,.14);stroke:#58a6ff;stroke-width:1.5;vector-effect:non-scaling-stroke}
aside{width:372px;flex:0 0 372px;border-left:1px solid #21262d;background:var(--panel);overflow:auto;padding:14px 16px 40px}
.toolbar{position:absolute;left:14px;top:12px;right:14px;display:flex;gap:8px;flex-wrap:wrap;z-index:3;pointer-events:none}
.toolbar>*{pointer-events:auto}
input,select,button{background:#161b22;color:var(--fg);border:1px solid var(--border);border-radius:7px;
  padding:6px 9px;font:inherit;font-size:12px;outline:none;transition:border-color .12s}
input:focus,select:focus{border-color:var(--accent)}
input#q{width:250px}
button{cursor:pointer}
button:hover{border-color:var(--accent)}
button.icon{padding:5px 9px;font-size:13px}
button.ghost{background:transparent}
.hint{position:absolute;right:14px;bottom:12px;color:#484f58;font-size:11px;z-index:2;pointer-events:none}
.count{color:var(--dim);font-size:11.5px;background:#161b22;border:1px solid var(--border);
  border-radius:20px;padding:4px 10px;white-space:nowrap}

/* ---- SVG ---- */
.node{cursor:pointer}
.node .card{fill:#151b23;stroke:#2d333b;stroke-width:1;transition:stroke .1s,filter .1s}
.node:hover .card{stroke:#58a6ff}
.node.sel .card{stroke:#58a6ff;stroke-width:2}
.node.blocked .card{stroke:#f85149;stroke-dasharray:5 3}
.node.match .card{stroke:#d29922;stroke-width:2}
.node.flash .card{stroke:#f778ba;stroke-width:3}
.node.dim{opacity:.13;pointer-events:none}
.node.hidden{display:none}
.node text{pointer-events:none;user-select:none}
.nm{fill:#e6edf3;font-size:13px;font-weight:600}
.st{font-size:13px}
.chain{font-size:11px;fill:#f85149}
.addr{fill:#c9d1d9;font-size:11.5px;font-family:ui-monospace,Consolas,"Courier New",monospace}
.meta{fill:#8b949e;font-size:10.5px}
.grp{font-size:10px;font-weight:600;letter-spacing:.4px}
.pmark{font-size:10px}
.stl{font-size:10px;font-weight:600}
.edge{fill:none;stroke:#3b434c;stroke-width:1.2;opacity:.85}
.edge.soft{stroke-dasharray:4 4;opacity:.6}
.edge.bad{stroke:#f85149;opacity:.5}
.edge.hl{stroke:#58a6ff;stroke-width:2.4;opacity:1}
.edge.dim{opacity:.05}
.edge.hidden{display:none}
#svg.far .node text{opacity:0}
#svg.far .node .card{stroke-opacity:.4}
#svg.far .edge{opacity:.22}

/* ---- 侧栏 ---- */
.ptitle{display:flex;align-items:center;gap:8px;margin:2px 0 10px}
.ptitle .bar{width:4px;height:20px;border-radius:2px}
.ptitle h2{font-size:15px;margin:0;font-weight:600;line-height:1.3}
.pill{display:inline-flex;align-items:center;gap:5px;border-radius:20px;padding:2px 9px;
  font-size:11px;font-weight:600;border:1px solid currentColor;white-space:nowrap}
.mono{font-family:ui-monospace,Consolas,"Courier New",monospace;font-size:11.5px;word-break:break-all}
.dim{color:var(--dim)}
.sec{margin:16px 0 7px;font-size:11px;letter-spacing:.6px;color:var(--dim);
  text-transform:uppercase;font-weight:600}
.banner{border-radius:8px;padding:9px 11px;font-size:12px;line-height:1.55;margin:10px 0}
.banner.bad{background:rgba(248,81,73,.1);border:1px solid rgba(248,81,73,.4)}
.banner.warn{background:rgba(210,153,34,.09);border:1px solid rgba(210,153,34,.35)}
.banner b{color:#fff}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0 2px}
.chip{display:inline-flex;align-items:center;gap:5px;background:#161b22;border:1px solid var(--border);
  border-radius:6px;padding:3px 7px;font-size:11.5px;cursor:pointer}
.chip:hover{border-color:var(--accent)}
.kv{display:grid;grid-template-columns:82px 1fr;gap:5px 10px;font-size:12px;margin:4px 0}
.kv span{color:var(--dim)}
.kv div{word-break:break-word}
pre.note{white-space:pre-wrap;word-break:break-word;background:#0c1117;border:1px solid #21262d;
  border-radius:7px;padding:8px 10px;font-size:11.5px;line-height:1.55;margin:4px 0;color:#c9d1d9;
  font-family:ui-monospace,Consolas,monospace}
.pbar{display:flex;height:9px;border-radius:5px;overflow:hidden;background:#21262d;margin:10px 0 8px}
.pbar div{height:100%}
.tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;margin:10px 0}
.tile{background:#121821;border:1px solid #21262d;border-radius:8px;padding:8px 9px}
.tile b{display:block;font-size:17px;font-weight:600;font-variant-numeric:tabular-nums;line-height:1.2}
.tile span{font-size:10.5px;color:var(--dim)}
.grow{display:flex;align-items:center;gap:8px;padding:5px 6px;border-radius:6px;cursor:pointer;font-size:12px}
.grow:hover{background:#161b22}
.grow .dot{width:8px;height:8px;border-radius:50%;flex:0 0 auto}
.grow .n{margin-left:auto;color:var(--dim);font-variant-numeric:tabular-nums}
.issue{display:flex;align-items:flex-start;gap:7px;padding:6px;border-radius:6px;cursor:pointer;font-size:12px;line-height:1.4}
.issue:hover{background:#161b22}
.issue .ic{flex:0 0 auto;font-size:11px;margin-top:1px}
.issue .why{color:var(--dim);font-size:11px}
.conf{display:flex;gap:8px;align-items:flex-start;padding:7px;border-radius:6px;cursor:pointer;font-size:11.5px;line-height:1.45}
.conf:hover{background:#161b22}
.conf .tag{flex:0 0 auto;font-size:10px;border-radius:4px;padding:1px 5px;margin-top:1px}
.conf .tag.partial{background:rgba(248,81,73,.15);color:#ff7b72;border:1px solid rgba(248,81,73,.4)}
.conf .tag.contains{background:rgba(139,148,158,.12);color:#8b949e;border:1px solid #30363d}
.empty{color:#484f58;font-size:12px;padding:8px 4px}
.foot{margin-top:22px;padding-top:12px;border-top:1px solid #21262d;color:#484f58;font-size:10.5px;line-height:1.7}
.tip{position:fixed;pointer-events:none;z-index:20;background:#1b222c;border:1px solid #30363d;
  border-radius:8px;padding:8px 10px;font-size:11.5px;line-height:1.5;box-shadow:0 8px 24px rgba(0,0,0,.45);
  max-width:280px;display:none;color:#c9d1d9}
.tip b{color:#fff;font-weight:600}
.tip .m{color:#8b949e;font-family:ui-monospace,Consolas,monospace;font-size:11px}
::-webkit-scrollbar{width:10px;height:10px}
::-webkit-scrollbar-thumb{background:#30363d;border-radius:6px;border:2px solid #0f141b}
::-webkit-scrollbar-track{background:transparent}
"""


HTML_JS = r"""
const DATA = __DATA__;
const N = DATA.nodes;
const byId = new Map(N.map(n => [n.id, n]));
const nodeEls = new Map();
N.forEach(n => nodeEls.set(n.i, document.querySelector('.node[data-i="' + n.i + '"]')));
const edgeEls = Array.from(document.querySelectorAll('.edge'));
const svg = document.getElementById('svg');
const vp = document.getElementById('viewport');
const panel = document.getElementById('panel');
const tip = document.getElementById('tip');
const q = document.getElementById('q');
const fstatus = document.getElementById('fstatus');
const fgroup = document.getElementById('fgroup');
const LW = DATA.layout.w, LH = DATA.layout.h;
let scale = 1, tx = 0, ty = 0, sel = null, drag = null, moved = 0;

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
function node(i) { return N[i]; }
function apply() {
  vp.setAttribute('transform', 'translate(' + tx + ',' + ty + ') scale(' + scale + ')');
  svg.classList.toggle('far', scale < 0.3);
  updateMini();
}

function fit() {
  const r = svg.getBoundingClientRect();
  const pad = 60;
  scale = Math.min((r.width - pad * 2) / LW, (r.height - pad * 2) / LH, 1.15);
  scale = Math.max(scale, 0.06);
  tx = (r.width - LW * scale) / 2;
  ty = (r.height - LH * scale) / 2;
  apply();
}
function initialView() {
  const r = svg.getBoundingClientRect();
  const bar = document.querySelector('.toolbar');
  const top = (bar ? bar.getBoundingClientRect().height : 60) + 26;
  scale = Math.max(0.4, Math.min(0.85, (r.height - top - 30) / LH, (r.width - 110) / (2.3 * (DATA.layout.node_w + DATA.layout.gap))));
  tx = 44;
  ty = Math.max(top, (r.height - LH * scale) / 2);
  apply();
}
function zoomBy(k, cx, cy) {
  const r = svg.getBoundingClientRect();
  cx = cx == null ? r.width / 2 : cx;
  cy = cy == null ? r.height / 2 : cy;
  const ns = Math.min(3, Math.max(0.1, scale * k));
  tx = cx - (cx - tx) * (ns / scale);
  ty = cy - (cy - ty) * (ns / scale);
  scale = ns;
  apply();
}

svg.addEventListener('wheel', e => {
  e.preventDefault();
  const r = svg.getBoundingClientRect();
  zoomBy(Math.exp(-e.deltaY * 0.0015), e.clientX - r.left, e.clientY - r.top);
}, {passive: false});

svg.addEventListener('pointerdown', e => {
  if (e.target.closest('.node')) return;
  drag = {x: e.clientX, y: e.clientY, tx: tx, ty: ty};
  moved = 0;
  svg.classList.add('drag');
  svg.setPointerCapture(e.pointerId);
});
svg.addEventListener('pointermove', e => {
  if (!drag) return;
  tx = drag.tx + e.clientX - drag.x;
  ty = drag.ty + e.clientY - drag.y;
  moved = Math.max(moved, Math.abs(e.clientX - drag.x) + Math.abs(e.clientY - drag.y));
  apply();
});
function endDrag(e) {
  if (!drag) return;
  drag = null;
  svg.classList.remove('drag');
  if (moved > 5) suppress = Date.now() + 200;
}
let suppress = 0;
svg.addEventListener('pointerup', endDrag);
svg.addEventListener('pointercancel', endDrag);

svg.addEventListener('click', e => {
  if (Date.now() < suppress) return;
  const g = e.target.closest('.node');
  if (g) { select(+g.dataset.i); return; }
  clearSel();
});
svg.addEventListener('dblclick', e => {
  const g = e.target.closest('.node');
  if (g) focusNode(+g.dataset.i);
});

svg.addEventListener('mouseover', e => {
  const g = e.target.closest('.node');
  if (!g) return;
  const n = node(+g.dataset.i);
  tip.innerHTML = '<b>' + esc(n.name) + '</b> <span class="m">' + esc(n.status_label) + '</span><br>'
    + '<span class="m">' + esc(n.base_str) + (n.size_str && n.size_str !== '—' ? ' · ' + esc(n.size_str) : '') + '</span>'
    + (n.blocked ? '<br><span style="color:#f85149">上游未通 ' + n.blockers.length + ' 项</span>' : '');
  tip.style.display = 'block';
});
svg.addEventListener('mousemove', e => {
  if (tip.style.display === 'block') {
    const pad = 16;
    let x = e.clientX + pad, y = e.clientY + pad;
    if (x + 290 > window.innerWidth) x = e.clientX - 290;
    if (y + 90 > window.innerHeight) y = e.clientY - 90;
    tip.style.left = x + 'px'; tip.style.top = y + 'px';
  }
});
svg.addEventListener('mouseout', e => {
  if (e.target.closest('.node')) tip.style.display = 'none';
});

function focusNode(i) {
  const r = svg.getBoundingClientRect();
  const n = node(i);
  scale = Math.max(scale, 0.75);
  tx = r.width / 2 - (n.x + DATA.layout.node_w / 2) * scale;
  ty = r.height / 2 - (n.y + DATA.layout.node_h / 2) * scale;
  apply();
}

function select(i, flash) {
  sel = i;
  document.querySelectorAll('.node.sel').forEach(x => x.classList.remove('sel'));
  const el = nodeEls.get(i);
  if (el) el.classList.add('sel');
  renderDetail(i);
  highlight();
  panel.scrollTop = 0;
  if (flash && flash.length) flashNodes(flash);
}
function clearSel() {
  sel = null;
  document.querySelectorAll('.node.sel').forEach(x => x.classList.remove('sel'));
  highlight();
  renderOverview();
}
function flashNodes(ids) {
  ids.forEach(i => {
    const el = nodeEls.get(i);
    if (!el) return;
    el.classList.add('flash');
    setTimeout(() => el.classList.remove('flash'), 1400);
  });
}

function highlight() {
  const all = new Set();
  if (sel != null) {
    const up = [sel], down = [sel];
    const seen = new Set([sel]), seen2 = new Set([sel]);
    while (up.length) { const x = up.pop(); node(x).deps.forEach(d => { const j = byId.get(d).i; if (!seen.has(j)) { seen.add(j); up.push(j); } }); }
    while (down.length) { const x = down.pop(); node(x).dependents.forEach(d => { const j = byId.get(d).i; if (!seen2.has(j)) { seen2.add(j); down.push(j); } }); }
    seen.forEach(x => all.add(x)); seen2.forEach(x => all.add(x));
  }
  N.forEach(n => {
    const el = nodeEls.get(n.i);
    if (!el) return;
    el.classList.toggle('dim', sel != null && !all.has(n.i));
  });
  edgeEls.forEach(e => {
    const f = +e.dataset.f, t = +e.dataset.t;
    const on = sel == null ? true : all.has(f) && all.has(t);
    e.classList.toggle('hl', sel != null && on);
    e.classList.toggle('dim', sel != null && !on);
  });
}

const PATCH_LABEL = {binary: '二进制补丁', source: '源码替换', config: '配置/顺序', disabled: '补丁未启用', stock: '原厂', none: '无', unknown: '未知'};
function patchColor(kind) { return kind === 'disabled' ? '#f85149' : (kind === 'binary' ? '#bc8cff' : '#58a6ff'); }

function statusPill(st) {
  const m = DATA.statuses[st];
  return '<span class="pill" style="color:' + m.color + '">' + m.icon + ' ' + esc(m.label) + '</span>';
}
function chipRef(ref, arrow) {
  const n = typeof ref === 'number' ? node(ref) : byId.get(ref);
  if (!n) return '';
  return '<span class="chip" data-i="' + n.i + '">' + (arrow || '') + n.icon + ' ' + esc(n.name) + '</span>';
}

function renderOverview() {
  const s = DATA.stats, c = s.counts;
  let html = '<div class="ptitle"><span class="bar" style="background:#58a6ff"></span><div>'
    + '<h2>打通总览</h2><div class="dim" style="font-size:11.5px">节点 / 依赖 / 地址冲突</div></div></div>';

  html += '<div class="dim" style="font-size:11.5px;margin-top:4px">可用进度 '
    + '<b style="color:#e6edf3">' + s.done + ' / ' + s.applicable + '</b>（' + s.progress + '%）'
    + (c.unused ? '，另有 ' + c.unused + ' 项不适用' : '') + '</div>';
  html += '<div class="pbar">';
  DATA.status_order.forEach(st => {
    if (!c[st]) return;
    const m = DATA.statuses[st];
    html += '<div title="' + esc(m.label) + ' ' + c[st] + '" style="width:' + (100 * c[st] / s.total)
      + '%;background:' + m.color + '"></div>';
  });
  html += '</div>';

  const pk = s.patches || {};
  const pkLabel = {binary: '二进制', source: '源码', config: '配置', disabled: '补丁未启用', stock: '原厂', unknown: '未知'};
  const pkParts = Object.keys(pkLabel).filter(k => pk[k]).map(k => pkLabel[k] + ' ' + pk[k]);
  if (pkParts.length) {
    html += '<div class="dim" style="font-size:11.5px;margin:2px 0 4px">补丁记录：' + pkParts.join(' · ') + '</div>';
  }

  html += '<div class="tiles">'
    + '<div class="tile"><b>' + s.total + '</b><span>节点</span></div>'
    + '<div class="tile"><b style="color:#3fb950">' + c.ok + '</b><span>✅ 已打通</span></div>'
    + '<div class="tile"><b style="color:#d29922">' + c.partial + '</b><span>⚠️ 部分可用</span></div>'
    + '<div class="tile"><b style="color:#f85149">' + (c.fail) + '</b><span>⛔ 实测失败</span></div>'
    + '<div class="tile"><b style="color:#58a6ff">' + c.todo + '</b><span>⏳ 未开始</span></div>'
    + '<div class="tile"><b style="color:#f85149">' + s.blocked + '</b><span>⛓ 上游未通</span></div>'
    + '</div>';

  html += '<div class="sec">分组</div>';
  DATA.groups.forEach(g => {
    if (!g.count) return;
    html += '<div class="grow" data-group="' + esc(g.id) + '"><span class="dot" style="background:'
      + g.color + '"></span><span>' + esc(g.label) + '</span><span class="n">' + g.count + '</span></div>';
  });

  const issues = N.filter(n => n.status === 'fail' || n.status === 'todo' || n.contradiction
    || (n.status === 'partial' && n.blockers.length));
  html += '<div class="sec">需要关注 · ' + issues.length + '</div>';
  if (!issues.length) html += '<div class="empty">没有失败或阻塞项</div>';
  issues.slice(0, 60).forEach(n => {
    let why = '';
    if (n.contradiction) why = '标记可用，但上游 ' + n.blockers.length + ' 项未通';
    else if (n.status === 'fail') why = n.state_note || '实测失败';
    else if (n.status === 'todo') why = n.blockers.length ? '未开始 · 卡在 ' + n.blockers.length + ' 项' : '未开始';
    else why = '部分可用 · 上游 ' + n.blockers.length + ' 项未通';
    html += '<div class="issue" data-i="' + n.i + '"><span class="ic" style="color:' + n.color + '">'
      + n.icon + '</span><div><div>' + esc(n.name) + '</div><div class="why">' + esc(why) + '</div></div></div>';
  });

  html += '<div class="sec">地址重叠 · ' + DATA.conflicts.length + '</div>';
  if (!DATA.conflicts.length) html += '<div class="empty">没有检测到重叠</div>';
  DATA.conflicts.slice(0, 40).forEach(cf => {
    const a = byId.get(cf.a), b = byId.get(cf.b);
    const tag = cf.kind === 'contains' ? '包含' : '部分重叠';
    html += '<div class="conf" data-i="' + a.i + '" data-flash="' + b.i + '"><span class="tag ' + cf.kind + '">'
      + tag + '</span><div><b>' + esc(a.name) + '</b> / ' + esc(b.name) + '<br><span class="dim">重叠 '
      + cf.bytes + ' 字节</span></div></div>';
  });

  html += '<div class="sec">状态口径</div>';
  DATA.status_order.forEach(st => {
    const m = DATA.statuses[st];
    html += '<div class="grow" style="cursor:default"><span class="ic">' + m.icon + '</span><span style="color:'
      + m.color + '">' + esc(m.label) + '</span><span class="n">' + c[st] + '</span></div>';
  });

  html += '<div class="foot">生成于 ' + esc(DATA.generated) + '<br>树: ' + esc(DATA.sources.tree)
    + '<br>状态: ' + esc(DATA.sources.state || '—') + '</div>';
  panel.innerHTML = html;
}

function renderDetail(i) {
  const n = node(i);
  let html = '<div class="ptitle"><span class="bar" style="background:' + n.color + '"></span><div>'
    + '<h2>' + esc(n.name) + '</h2><div class="dim mono">' + esc(n.id) + '</div></div></div>';
  html += '<div>' + statusPill(n.status) + ' <span class="dim" style="font-size:11.5px;margin-left:6px">'
    + esc(DATA.statuses[n.status].desc) + '</span></div>';

  if (n.contradiction) {
    html += '<div class="banner bad"><b>⛓ 状态矛盾：</b>标记为可用，但硬依赖里仍有 '
      + n.blockers.length + ' 项未通过。上游不通时该节点不可能真正工作。</div>';
  } else if (n.blocked && n.status !== 'fail') {
    html += '<div class="banner warn"><b>⛓ 上游未通：</b>' + n.blockers.length
      + ' 项依赖尚未打通，当前节点不可达。</div>';
  }
  if (n.blockers.length) {
    html += '<div class="chips">' + n.blockers.slice(0, 20).map(x => chipRef(x, '↑')).join('') + '</div>';
  }

  html += '<div class="sec">硬件</div><div class="kv">';
  html += '<span>基址</span><div class="mono">' + esc(n.base_str) + '</div>';
  html += '<span>大小</span><div class="mono">' + esc(n.size_str) + '</div>';
  if (n.range_str) html += '<span>区间</span><div class="mono dim">' + esc(n.range_str) + '</div>';
  if (n.irq.length) html += '<span>IRQ</span><div class="mono">' + esc(n.irq.join(', ')) + '</div>';
  if (n.driver) html += '<span>驱动</span><div>' + esc(n.driver) + '</div>';
  if (n.dt_path) html += '<span>DT 节点</span><div class="mono dim">' + esc(n.dt_path) + '</div>';
  if (n.compatible) html += '<span>compatible</span><div class="mono dim">' + esc(n.compatible) + '</div>';
  html += '<span>分组</span><div><span class="dot" style="display:inline-block;width:8px;height:8px;border-radius:50%;background:'
    + n.group_color + '"></span> ' + esc(n.group_label) + '</div>';
  if (n.tags.length) html += '<span>标签</span><div>' + n.tags.map(t => '<span class="chip">' + esc(t) + '</span>').join(' ') + '</div>';
  if (n.updated) html += '<span>更新</span><div class="dim">' + esc(n.updated) + '</div>';
  html += '</div>';

  const p = n.patch || {};
  if (p.kind && p.kind !== 'none') {
    html += '<div class="sec">补丁 / 版本</div>';
    html += '<div class="chips" style="margin-bottom:6px"><span class="pill" style="color:' + patchColor(p.kind) + '">'
      + esc(PATCH_LABEL[p.kind] || p.kind) + '</span>'
      + (p.enabled === false ? '<span class="pill" style="color:#f85149">未启用</span>' : '')
      + '</div><div class="kv">';
    if (p.ref) html += '<span>来源</span><div class="mono">' + esc(p.ref) + '</div>';
    if (p.delta) html += '<span>差异</span><div class="mono">' + esc(p.delta) + '</div>';
    if (p.note) html += '<span>说明</span><div>' + esc(p.note) + '</div>';
    html += '</div>';
  }

  html += '<div class="sec">依赖 · ' + n.deps.length + '</div>';
  html += n.deps.length ? '<div class="chips">' + n.deps.map(x => chipRef(x, '↑')).join('') + '</div>'
    : '<div class="empty">无硬依赖</div>';
  if (n.soft_deps.length) {
    html += '<div class="sec" style="margin-top:12px">软依赖 · ' + n.soft_deps.length
      + '<span class="dim" style="text-transform:none;letter-spacing:0">（缺失不阻塞，仅降级）</span></div>';
    html += '<div class="chips">' + n.soft_deps.map(x => chipRef(x, '⇢')).join('') + '</div>';
  }
  if (n.dependents.length) {
    html += '<div class="sec">被谁依赖 · ' + n.dependents.length + '</div>';
    html += '<div class="chips">' + n.dependents.map(x => chipRef(x, '↓')).join('') + '</div>';
  }

  if (n.evidence) html += '<div class="sec">证据</div><pre class="note">' + esc(n.evidence) + '</pre>';
  if (n.state_note) html += '<div class="sec">说明</div><pre class="note">' + esc(n.state_note) + '</pre>';
  if (n.notes) html += '<div class="sec">备注</div><pre class="note">' + esc(n.notes) + '</pre>';

  html += '<div class="sec">操作</div><div class="chips">'
    + '<span class="chip" data-focus="' + n.i + '">⤢ 聚焦</span>'
    + '<span class="chip" data-i="' + n.i + '">↺ 重选</span></div>';
  html += '<div class="foot">双击节点可聚焦；Esc 返回总览</div>';
  panel.innerHTML = html;
}

panel.addEventListener('click', e => {
  const f = e.target.closest('[data-focus]');
  if (f) { focusNode(+f.dataset.focus); return; }
  const g = e.target.closest('[data-group]');
  if (g) { fgroup.value = g.dataset.group; applyFilters(); return; }
  const t = e.target.closest('[data-i]');
  if (!t) return;
  const raw = t.dataset.i;
  if (raw === '') return;
  const i = +raw;
  if (t.dataset.flash) {
    select(i, [+t.dataset.flash]);
    setTimeout(() => flashNodes([i, +t.dataset.flash]), 60);
  } else {
    select(i);
  }
});

function applyFilters() {
  const term = q.value.trim().toLowerCase();
  const st = fstatus.value, gr = fgroup.value;
  let visible = 0;
  N.forEach(n => {
    let ok = true;
    if (st === 'issues') ok = n.status === 'fail' || n.status === 'todo' || n.blocked;
    else if (st === 'blocked') ok = n.blocked;
    else if (st === 'patch') ok = !!(n.patch && n.patch.kind && n.patch.kind !== 'none');
    else if (st !== 'all') ok = n.status === st;
    if (gr !== 'all') ok = ok && n.group === gr;
    if (term) {
      const hay = (n.id + ' ' + n.name + ' ' + n.driver + ' ' + n.base_str + ' '
        + n.compatible + ' ' + n.dt_path + ' ' + n.group_label).toLowerCase();
      if (!hay.includes(term)) ok = false;
    }
    const el = nodeEls.get(n.i);
    el.classList.toggle('hidden', !ok);
    el.classList.toggle('match', !!term && ok);
    if (ok) visible++;
  });
  const vis = new Set();
  N.forEach(n => { if (!nodeEls.get(n.i).classList.contains('hidden')) vis.add(n.i); });
  edgeEls.forEach(e => e.classList.toggle('hidden', !(vis.has(+e.dataset.f) && vis.has(+e.dataset.t))));
  document.getElementById('vcount').textContent = visible + ' / ' + N.length + ' 节点';
}

q.addEventListener('input', applyFilters);
fstatus.addEventListener('change', applyFilters);
fgroup.addEventListener('change', applyFilters);
document.getElementById('fit').addEventListener('click', fit);
document.getElementById('zin').addEventListener('click', () => zoomBy(1.25));
document.getElementById('zout').addEventListener('click', () => zoomBy(0.8));
document.getElementById('reset').addEventListener('click', () => { clearSel(); q.value = ''; fstatus.value = 'all'; fgroup.value = 'all'; applyFilters(); initialView(); });

document.addEventListener('keydown', e => {
  const typing = /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement.tagName);
  if (e.key === 'Escape') { clearSel(); document.activeElement.blur(); }
  else if (e.key === '/' && !typing) { e.preventDefault(); q.focus(); }
  else if (!typing && (e.key === '0')) fit();
  else if (!typing && (e.key === '+' || e.key === '=')) zoomBy(1.25);
  else if (!typing && e.key === '-') zoomBy(0.8);
  else if (e.key === 'Enter' && document.activeElement === q) {
    const first = N.find(n => nodeEls.get(n.i) && !nodeEls.get(n.i).classList.contains('hidden'));
    if (first) { select(first.i); focusNode(first.i); }
  }
});

DATA.groups.forEach(g => {
  const opt = document.createElement('option');
  opt.value = g.id; opt.textContent = g.label + ' (' + g.count + ')';
  fgroup.appendChild(opt);
});

const mini = document.getElementById('mini');
mini.setAttribute('viewBox', '0 0 ' + LW + ' ' + LH);
let miniReady = false;
function buildMini() {
  let s = '';
  N.forEach(n => {
    s += '<rect x="' + n.x + '" y="' + n.y + '" width="' + DATA.layout.node_w + '" height="'
      + DATA.layout.node_h + '" rx="10" fill="' + n.color + '" opacity=".85"/>';
  });
  s += '<rect id="minivp" class="vp"/>';
  mini.innerHTML = s;
  miniReady = true;
}
function updateMini() {
  if (!miniReady) return;
  const r = svg.getBoundingClientRect();
  const x0 = -tx / scale, y0 = -ty / scale;
  const x = Math.max(0, Math.min(LW, x0));
  const y = Math.max(0, Math.min(LH, y0));
  const x2 = Math.max(0, Math.min(LW, x0 + r.width / scale));
  const y2 = Math.max(0, Math.min(LH, y0 + r.height / scale));
  const vpEl = document.getElementById('minivp');
  vpEl.setAttribute('x', x); vpEl.setAttribute('y', y);
  vpEl.setAttribute('width', Math.max(0, x2 - x));
  vpEl.setAttribute('height', Math.max(0, y2 - y));
}
mini.addEventListener('click', e => {
  const r = mini.getBoundingClientRect();
  const cx = (e.clientX - r.left) / r.width * LW;
  const cy = (e.clientY - r.top) / r.height * LH;
  const sr = svg.getBoundingClientRect();
  tx = sr.width / 2 - cx * scale;
  ty = sr.height / 2 - cy * scale;
  apply();
});

buildMini();
renderOverview();
applyFilters();
initialView();
window.addEventListener('resize', () => { if (scale < 0.2) fit(); });
"""


HTML_SHELL = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<style>__CSS__</style>
</head>
<body>
<div id="app">
  <header>
    <h1>__TITLE__</h1>
    <div class="sub">__SUBTITLE__</div>
    <div class="spacer"></div>
    <div class="hstat">节点 <b>__TOTAL__</b></div>
    <div class="sep"></div>
    <div class="hstat">已打通 <b style="color:#3fb950">__DONE__</b>/__APPLICABLE__</div>
    <div class="sep"></div>
    <div class="hstat">失败 <b style="color:#f85149">__FAIL__</b></div>
    <div class="sep"></div>
    <div class="hstat">上游未通 <b style="color:#f85149">__BLOCKED__</b></div>
    <div class="sep"></div>
    <div class="hstat" id="vcount"></div>
  </header>
  <main>
    <div id="canvas">
      <div class="toolbar">
        <input id="q" type="search" placeholder="搜索 名称 / id / 基址 / 驱动…  ( / )" autocomplete="off">
        <select id="fstatus" title="状态过滤">
          <option value="all">全部状态</option>
          <option value="issues">只看问题</option>
          <option value="blocked">上游未通</option>
          <option value="ok">✅ 已打通</option>
          <option value="partial">⚠️ 部分可用</option>
          <option value="patched">🔧 需补丁</option>
          <option value="fail">⛔ 实测失败</option>
          <option value="todo">⏳ 未开始</option>
          <option value="unknown">❓ 无证据</option>
          <option value="patch">⚙ 有补丁记录</option>
          <option value="unused">🗑️ 不适用</option>
        </select>
        <select id="fgroup" title="分组过滤"><option value="all">全部分组</option></select>
        <button class="icon" id="fit" title="全览 (0)">⤢</button>
        <button class="icon" id="zin" title="放大 (+)">＋</button>
        <button class="icon" id="zout" title="缩小 (-)">－</button>
        <button class="icon" id="reset" title="重置">⟲</button>
      </div>
      __SVG__
      <div id="mini-wrap"><svg id="mini" preserveAspectRatio="none"></svg></div>
      <div class="hint">滚轮缩放 · 拖拽平移 · 双击聚焦</div>
    </div>
    <aside id="panel"></aside>
  </main>
</div>
<div class="tip" id="tip"></div>
<script>__JS__</script>
</body>
</html>
"""


def render_html(model: Model, out_path: Path) -> None:
    payload = _payload(model)
    data_json = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    data_json = (data_json.replace("</", "<\\/")
                 .replace("\u2028", "\\u2028").replace("\u2029", "\\u2029"))
    svg = _svg(model)
    stats = model.stats
    shell = (HTML_SHELL
             .replace("__TITLE__", h(model.title))
             .replace("__SUBTITLE__", h(model.subtitle or "设备树与打通状态"))
             .replace("__TOTAL__", str(stats["total"]))
             .replace("__DONE__", str(stats["done"]))
             .replace("__APPLICABLE__", str(stats["applicable"]))
             .replace("__FAIL__", str(stats["counts"]["fail"]))
             .replace("__BLOCKED__", str(stats["blocked"]))
             .replace("__SVG__", svg)
             .replace("__CSS__", HTML_CSS)
             .replace("__JS__", HTML_JS.replace("__DATA__", data_json)))
    out_path.write_text(shell, encoding="utf-8", newline="\n")


# --------------------------------------------------------------------------
# 渲染：其它格式
# --------------------------------------------------------------------------

def _dot_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def render_dot(model: Model) -> str:
    lines = [
        "digraph devtree {",
        '  graph [rankdir=LR, bgcolor="#ffffff", fontname="Microsoft YaHei", '
        'nodesep=0.35, ranksep=0.7, splines=spline, pad=0.3];',
        '  node [shape=box, style="rounded,filled", fontname="Microsoft YaHei", fontsize=9, '
        'color="#c9d1d9", fillcolor="#f6f8fa", fontcolor="#1f2328", margin="0.12,0.08"];',
        '  edge [color="#8b949e", arrowsize=0.7, penwidth=1.0];',
        "  legend [shape=plaintext, label=<",
        '<table border="0" cellborder="0" cellspacing="2" cellpadding="1">'
        '<tr><td colspan="2"><b>状态</b></td></tr>',
    ]
    for st in STATUS_ORDER:
        m = STATUS[st]
        lines.append(f'<tr><td bgcolor="{m["color"]}">    </td>'
                     f'<td align="left">{m["icon"]} {m["label"]}</td></tr>')
    lines.append("</table>>];")

    for g in model.groups:
        members = [n for n in model.nodes if n.group == g["id"]]
        if not members:
            continue
        lines.append(f'  subgraph cluster_{re.sub(r"[^A-Za-z0-9_]", "_", g["id"])} {{')
        lines.append(f'    label="{_dot_escape(g["label"])}"; fontsize=10; fontcolor="{g["color"]}"; '
                     f'color="{g["color"]}"; style="rounded,dashed"; penwidth=1.2;')
        for n in members:
            m = STATUS[n.status]
            sub = fmt_addr(n.base) if n.base is not None else "无基址"
            pk = str(n.patch.get("kind") or "")
            pl = f"\\nP:{pk}" if pk and pk != "none" else ""
            label = f'{n.name}\\n{sub} · {fmt_size(n.size) if n.size else "—"}\\n{m["icon"]} {m["label"]}{pl}'
            lines.append(f'    "{n.id}" [label="{_dot_escape(label)}", fillcolor="{m["color"]}22", '
                         f'color="{m["color"]}"];')
        lines.append("  }")

    for n in model.nodes:
        for dep in n.deps:
            style = ', color="#f85149", penwidth=1.4' if n.id in model.blockers and model.blockers[n.id] else ""
            lines.append(f'  "{dep}" -> "{n.id}" [{style.lstrip(", ")}];')
        for dep in n.soft_deps:
            lines.append(f'  "{dep}" -> "{n.id}" [style=dashed, arrowhead=open, color="#8b949e"];')

    lines.append("}")
    return "\n".join(lines)


def render_mermaid(model: Model) -> str:
    def mid(s: str) -> str:
        m = re.sub(r"[^A-Za-z0-9_]", "_", s)
        return "n_" + m if not m or m[0].isdigit() else m

    lines = ["flowchart LR"]
    for st in STATUS_ORDER:
        m = STATUS[st]
        lines.append(f'  classDef st_{st} fill:{m["color"]}22,stroke:{m["color"]},color:#1f2328;')
    for g in model.groups:
        members = [n for n in model.nodes if n.group == g["id"]]
        if not members:
            continue
        lines.append(f'  subgraph g_{mid(g["id"])}["{g["label"]}"]')
        for n in members:
            base = fmt_addr(n.base) if n.base is not None else "无基址"
            label = f'{n.name}<br/>{base} · {STATUS[n.status]["label"]}'
            lines.append(f'    {mid(n.id)}["{label}"]:::st_{n.status}')
        lines.append("  end")
    for n in model.nodes:
        for dep in n.deps:
            lines.append(f"  {mid(dep)} --> {mid(n.id)}")
        for dep in n.soft_deps:
            lines.append(f"  {mid(dep)} -.-> {mid(n.id)}")
    return "\n".join(lines)


def render_json(model: Model) -> str:
    payload = _payload(model)
    payload["issues"] = {
        "fail": [n.id for n in model.nodes if n.status == "fail"],
        "todo": [n.id for n in model.nodes if n.status == "todo"],
        "contradictions": model.stats["contradictions"],
        "blocked": model.stats["blocked_ids"],
    }
    return json.dumps(payload, ensure_ascii=False, indent=2)


# --------------------------------------------------------------------------
# DTS 导入
# --------------------------------------------------------------------------

@dataclass
class DtsNode:
    name: str
    label: Optional[str]
    parent: Optional["DtsNode"]
    props: Dict[str, Any] = field(default_factory=dict)
    children: List["DtsNode"] = field(default_factory=list)
    depth: int = 0
    path: str = "/"

    def cells(self, key: str) -> List[int]:
        val = self.props.get(key)
        return val.get("cells", []) if isinstance(val, dict) else []

    def strings(self, key: str) -> List[str]:
        val = self.props.get(key)
        return val.get("strings", []) if isinstance(val, dict) else []

    def has(self, key: str) -> bool:
        return key in self.props


REF_PAIRS = {"clocks", "assigned-clocks", "resets", "interconnects", "interrupts-extended",
             "qcom,wcd_usbss", "qcom,smem-states"}
DEFAULT_EXCLUDE = (r"tpdm|funnel|replicator|coresight|\bcti\b|cti@|tgu@|tmc@|csr@|"
                   r"\bete\d|\betm\d|_etm|traceNoc|gladiator|dummy_|stm@|smmu-test")


def _combine(cells: Sequence[int], count: int) -> int:
    value = 0
    for c in cells[:count]:
        value = (value << 32) | (c & 0xFFFFFFFF)
    return value


def _parse_cells(value: str) -> List[int]:
    cleaned = re.sub(r'"[^"]*"', " ", value)
    out = []
    for x in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", cleaned):
        out.append(int(x, 16) if x[:2].lower() == "0x" else int(x, 10))
    return out


def parse_dts(text: str) -> DtsNode:
    root = DtsNode(name="/", label=None, parent=None, depth=0, path="/")
    stack = [root]
    pending: Optional[str] = None
    root_open = False

    def store(node: DtsNode, raw: str) -> None:
        raw = raw.strip()
        if "=" in raw:
            key, val = raw.split("=", 1)
            key = key.strip()
            val = val.strip().rstrip(";").strip()
            node.props[key] = {
                "cells": _parse_cells(val),
                "strings": re.findall(r'"([^"]*)"', val),
                "raw": val,
            }
        else:
            node.props[raw.rstrip(";").strip()] = {"cells": [], "strings": [], "raw": ""}

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if pending is not None:
            pending += " " + line
            if line.endswith(";"):
                store(stack[-1], pending)
                pending = None
            continue
        if not line or line.startswith("//"):
            continue
        if line.startswith("/dts-v1/") or line.startswith("/memreserve/") or line.startswith("/plugin/"):
            continue
        if line in ("};", "}", "}; //"):
            if len(stack) > 1:
                stack.pop()
            elif root_open:
                root_open = False
            continue
        if line.startswith("/delete-node/") or line.startswith("/omit-if-no-ref/"):
            continue

        m = re.match(r"^(?:(?P<label>[A-Za-z_][A-Za-z0-9_]*)\s*:\s*)?(?P<name>[^\s{]+)\s*\{\s*$", line)
        if m and "=" not in line:
            name = m.group("name")
            parent = stack[-1]
            if name == "/" and parent is root and not root.children and not root_open:
                root_open = True
                continue
            node = DtsNode(
                name=name,
                label=m.group("label"),
                parent=parent,
                depth=parent.depth + 1,
                path=(parent.path.rstrip("/") + "/" + name),
            )
            parent.children.append(node)
            stack.append(node)
            continue

        if line.endswith(";"):
            if "=" in line:
                store(stack[-1], line)
            else:
                store(stack[-1], line)
            continue

        if "=" in line:
            pending = line
            if line.endswith(";"):
                store(stack[-1], pending)
                pending = None
            continue

    return root


def _walk(node: DtsNode) -> Iterable[DtsNode]:
    yield node
    for child in node.children:
        yield from _walk(child)


def _drop_back_edges(deps: Dict[str, List[str]]) -> int:
    color: Dict[str, int] = {}
    dropped = 0

    def dfs(u: str) -> None:
        nonlocal dropped
        color[u] = 1
        keep: List[str] = []
        for v in deps.get(u, []):
            c = color.get(v, 0)
            if c == 1:
                dropped += 1
                continue
            if c == 0:
                dfs(v)
            keep.append(v)
        deps[u] = keep
        color[u] = 2

    for nid in list(deps):
        if color.get(nid, 0) == 0:
            dfs(nid)
    return dropped


def import_dts(path: Path, out_path: Path, depth: int, include: Optional[str],
               exclude: Optional[str], want_deps: bool, under: str,
               warnings: List[str]) -> None:
    try:
        text = path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as exc:
        raise DataError(f"无法读取 {path}: {exc}") from None
    root = parse_dts(text)
    if not root.children:
        raise DataError(f"{path}: 没解析出任何节点（需要 fdtdump 或 dtc -O dts 的文本输出）")

    path_to_label: Dict[str, str] = {}
    for node in _walk(root):
        if node.name == "__symbols__":
            for key, val in node.props.items():
                if val["strings"]:
                    path_to_label.setdefault(val["strings"][0].rstrip("/"), key)

    phandle_to_node: Dict[int, DtsNode] = {}
    for node in _walk(root):
        cells = node.cells("phandle")
        if cells:
            phandle_to_node[cells[0]] = node

    inc_re = re.compile(include) if include else None
    exc_re = re.compile(exclude or DEFAULT_EXCLUDE)
    base_path = "/" if under.strip() in ("", "/") else "/" + under.strip().strip("/")

    def rel_depth(p: str) -> int:
        if base_path == "/":
            rel = p.lstrip("/")
        else:
            if p == base_path:
                return 0
            if not p.startswith(base_path + "/"):
                return -1
            rel = p[len(base_path) + 1:]
        return rel.count("/") + 1

    candidates = [n for n in _walk(root)
                  if n is not root and rel_depth(n.path) == depth]
    selected: List[DtsNode] = []
    for node in candidates:
        if node.path in path_to_label:
            pass
        if exc_re.search(node.name) or exc_re.search(node.path):
            continue
        if inc_re and not (inc_re.search(node.name) or inc_re.search(node.path)):
            continue
        if not node.has("reg") and not node.has("compatible"):
            continue
        selected.append(node)
    if not selected:
        raise DataError(f"深度 {depth} 下没有可导入的节点，试试 --depth 2/3 或放宽 --exclude")

    def addr_cells(node: DtsNode) -> Tuple[int, int]:
        parent = node.parent
        ac, sc = 2, 1
        if parent is not None:
            c = parent.cells("#address-cells")
            if c:
                ac = max(1, min(4, c[0]))
            c = parent.cells("#size-cells")
            if c:
                sc = max(1, min(4, c[0]))
        return ac, sc

    def group_for(node: DtsNode) -> str:
        compat = " ".join(node.strings("compatible")).lower()
        name = node.name.lower()
        rules = [
            ("usb", ("usb", "dwc3", "hsphy", "ssphy", "eud", "tcpc")),
            ("storage", ("ufs", "sdhci", "sdcc", "ice")),
            ("display", ("mdss", "dsi", "disp", "sde", "panel", "gpu", "kgsl")),
            ("input", ("touch", "synaptics", "goodix", "pinctrl", "gpi", "geni", "qup")),
            ("clock", ("clock", "gcc", "cc", "rpmh", "rsc", "interconnect", "noc")),
            ("power", ("pmic", "spmi", "charger", "battery", "power", "regulator", "qmp")),
            ("security", ("qsee", "scm", "crypto", "rng", "keymaster", "tz", "smmu", "iommu")),
            ("audio", ("audio", "lpass", "wcd", "adsp", "codec", "slim")),
            ("connectivity", ("pcie", "wlan", "ipa", "modem", "mss", "glink", "smem", "ipcc")),
        ]
        for gid, keys in rules:
            if any(k in compat or k in name for k in keys):
                return gid
        return "other"

    used = set()
    nodes_out: List[Dict[str, Any]] = []
    id_by_path: Dict[str, str] = {}
    dep_count = 0
    skipped_refs = 0

    def make_id(node: DtsNode, base: Optional[int]) -> str:
        label = path_to_label.get(node.path)
        raw = label or re.sub(r"@[0-9a-fA-F]+$", "", node.name)
        raw = raw.replace(",", "_")
        sid = re.sub(r"[^A-Za-z0-9_.\-:/@]", "_", raw).strip("_") or "node"
        if sid.startswith("_") or not sid:
            sid = "n" + sid
        if sid in used:
            sid = f"{sid}_{base:X}" if base is not None else f"{sid}_{len(nodes_out)}"
        while sid in used:
            sid += "_x"
        used.add(sid)
        return sid

    for node in selected:
        ac, sc = addr_cells(node)
        reg = node.cells("reg")
        own_ac = node.cells("#address-cells")
        own_sc = node.cells("#size-cells")
        if own_ac and own_sc and len(reg) == own_ac[0] + own_sc[0]:
            # 少数节点（如 mmio-sram）的 reg 按自身声明的 64 位 cells 书写
            ac, sc = own_ac[0], own_sc[0]
        base = size = None
        ranges = 0
        if reg and len(reg) >= ac + sc:
            base = _combine(reg[:ac], ac)
            size = _combine(reg[ac:ac + sc], sc) if sc else None
            ranges = len(reg) // (ac + sc)

        sid = make_id(node, base)
        id_by_path[node.path] = sid
        compat = node.strings("compatible")
        compat_s = ", ".join(compat)
        mem_like = bool(re.search(r"sharedmem|shared-mem|sram|shmem|smem|reserved|mem-region",
                                  compat_s + " " + node.name, re.I))
        dt_status = node.strings("status")[0] if node.has("status") else "okay"
        note_lines = []
        if dt_status != "okay":
            note_lines.append(f'DT status = "{dt_status}"（仅表示 Linux 节点开关，不代表 UEFI 打通）')
        if ranges > 1:
            note_lines.append(f"reg 共 {ranges} 段，此处只取第一段")
        if node.has("phandle"):
            note_lines.append(f"phandle = 0x{node.cells('phandle')[0]:X}")
        nodes_out.append({
            "id": sid,
            "name": node.name.split("@")[0] or node.name,
            "group": group_for(node),
            "base": base,
            "size": size,
            "compatible": compat_s,
            "dt_path": node.path,
            "irq": [],
            "driver": "",
            "deps": [],
            "no_overlap": mem_like,
            "notes": "；".join(note_lines),
        })

    if want_deps:
        by_id_out = {n["id"]: n for n in nodes_out}
        for node in selected:
            sid = id_by_path[node.path]
            deps: List[str] = []
            for prop, val in node.props.items():
                if not isinstance(val, dict):
                    continue
                cells = val.get("cells") or []
                if not cells:
                    continue
                if prop.endswith("-supply"):
                    candidates_cells = cells[:1]
                elif prop in REF_PAIRS:
                    candidates_cells = cells[0::2]
                elif prop in ("phys", "power-domains", "nvmem-cells", "mailbox", "extcon"):
                    candidates_cells = cells[:1]
                else:
                    continue
                for cell in candidates_cells:
                    target = phandle_to_node.get(cell)
                    if target is None or target is node:
                        continue
                    tid = id_by_path.get(target.path)
                    if tid and tid != sid and tid not in deps:
                        deps.append(tid)
                    elif tid is None:
                        skipped_refs += 1
            by_id_out[sid]["deps"] = deps
            dep_count += len(deps)

        graph = {n["id"]: list(n["deps"]) for n in nodes_out}
        dropped = _drop_back_edges(graph)
        if dropped:
            for n in nodes_out:
                n["deps"] = graph[n["id"]]
            warnings.append(f"phandle 引用存在 {dropped} 条回边（环），已丢弃；"
                            f"驱动初始化顺序仍需人工确认")

    groups: Dict[str, str] = {}
    for n in nodes_out:
        groups.setdefault(n["group"], n["group"])
    group_labels = {
        "usb": "USB", "storage": "存储", "display": "显示", "input": "输入/低速外设",
        "clock": "时钟/总线", "power": "电源", "security": "安全/IOMMU", "audio": "音频",
        "connectivity": "连接", "other": "其它",
    }
    doc = {
        "meta": {
            "title": f"Imported: {path.name}",
            "subtitle": f"从 DTS 自动导入 {len(nodes_out)} 个节点（深度 {depth}）",
            "updated": datetime.now().strftime("%Y-%m-%d"),
            "note": "自动导入骨架：status 不是 UEFI 打通状态，请用单独的状态文件标注。",
        },
        "groups": [{"id": g, "label": group_labels.get(g, g)} for g in groups],
        "nodes": nodes_out,
    }
    header = ("# devtree_viz.py --import-dts 生成\n"
              "# 1) 手工补充 driver / irq / 依赖（自动依赖只来自 phandle 引用）\n"
              "# 2) 用状态文件标注打通情况，切勿把 DT status=okay 当作已打通\n")
    body = yaml.safe_dump(doc, allow_unicode=True, sort_keys=False, width=1000)
    out_path.write_text(header + body, encoding="utf-8", newline="\n")

    warnings.append(f"导入 {len(nodes_out)} 节点 / 自动依赖 {dep_count} 条；"
                    f"引用到未导入节点而跳过的依赖 {skipped_refs} 处")


# --------------------------------------------------------------------------
# 控制台报告
# --------------------------------------------------------------------------

def print_report(model: Model) -> None:
    if not model.stats:
        for w in model.warnings:
            print(f"[warn]  {w}")
        for e in model.errors:
            print(f"[error] {e}", file=sys.stderr)
        return
    s = model.stats
    c = s["counts"]
    print(f"[tree]  {model.title}")
    print(f"        节点 {s['total']} · 硬依赖 {s['hard_edges']} · 软依赖 {s['soft_edges']} · 层数 {s['layers']}")
    parts = []
    for st in STATUS_ORDER:
        if c[st]:
            parts.append(f"{STATUS[st]['icon']} {STATUS[st]['label']} {c[st]}")
    print("[state] " + " · ".join(parts))
    print(f"[done]  {s['done']}/{s['applicable']} 可用（{s['progress']}%），不适用 {c['unused']}")

    if s["contradictions"]:
        names = "、".join(model.by_id[i].name for i in s["contradictions"][:8])
        more = "…" if len(s["contradictions"]) > 8 else ""
        print(f"[矛盾]  {len(s['contradictions'])} 个节点标记可用但上游未通: {names}{more}")
    if model.conflicts:
        print(f"[地址]  {len(model.conflicts)} 处重叠")
        for cf in model.conflicts[:5]:
            a, b = model.by_id[cf["a"]], model.by_id[cf["b"]]
            kind = "包含" if cf["kind"] == "contains" else "部分重叠"
            print(f"        {kind} {a.name} ({fmt_addr(a.base)}+{fmt_size(a.size)})"
                  f" × {b.name} ({fmt_addr(b.base)}+{fmt_size(b.size)})")
    for w in model.warnings:
        print(f"[warn]  {w}")
    for e in model.errors:
        print(f"[error] {e}", file=sys.stderr)


def output_summary(model: Model, out: Path) -> None:
    print(f"[out]   {out}  ({out.stat().st_size / 1024:.0f} KiB)")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="devtree_viz.py",
        description="设备树 + 打通状态可视化（自包含 HTML / DOT / SVG / PNG / Mermaid / JSON）",
        epilog="示例: devtree_viz.py examples/shennong.tree.yaml examples/shennong.state.yaml -o out.html",
    )
    p.add_argument("tree", nargs="?", help="设备树 YAML/JSON 文件（或 --import-dts 时的 DTS 输入）")
    p.add_argument("state", nargs="?", help="状态文件 YAML/JSON")
    p.add_argument("-o", "--out", help="输出文件（默认: devtree.html / devtree.dot …）")
    p.add_argument("-f", "--format", default="html",
                   choices=["html", "dot", "svg", "png", "mermaid", "json"],
                   help="输出格式（默认 html）")
    p.add_argument("--title", help="覆盖标题")
    p.add_argument("--check", action="store_true", help="只校验并打印摘要，不写文件")
    p.add_argument("--strict", action="store_true", help="把警告当作错误")
    p.add_argument("--block-on-partial", action="store_true",
                   help="把 partial（部分可用）也算作阻塞上游的状态")
    p.add_argument("--open", action="store_true", help="生成 HTML 后用浏览器打开")
    p.add_argument("--version", action="version", version=f"%(prog)s {VERSION}")

    imp = p.add_argument_group("DTS 导入")
    imp.add_argument("--import-dts", metavar="FILE", help="从 fdtdump/dtc 文本 DTS 生成树 YAML 骨架")
    imp.add_argument("--under", default="/soc", help="导入基准节点（默认 /soc，传 / 表示整棵树）")
    imp.add_argument("--depth", type=int, default=1, help="相对 --under 的深度（1 = 直接子节点，默认 1）")
    imp.add_argument("--include", help="只导入路径/名称匹配该正则的节点")
    imp.add_argument("--exclude", help=f"排除匹配该正则的节点（默认排除 coresight 等）")
    imp.add_argument("--no-deps", action="store_true", help="不自动从 phandle 推导依赖")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    args = build_parser().parse_args(argv)

    if args.import_dts:
        src = Path(args.import_dts)
        if not src.exists():
            print(f"错误: {src} 不存在", file=sys.stderr)
            return 2
        out = Path(args.out or (src.with_suffix(".tree.yaml").name if src.suffix else "tree.yaml"))
        warnings: List[str] = []
        try:
            import_dts(src, out, max(1, args.depth), args.include,
                       args.exclude, not args.no_deps, args.under, warnings)
        except DataError as exc:
            print(f"错误: {exc}", file=sys.stderr)
            return 2
        for w in warnings:
            print(f"[warn]  {w}")
        print(f"[out]   {out}")
        print("下一步: 补充 driver/irq/依赖，再写状态文件，然后:")
        print(f"  python {Path(__file__).name} {out.name} state.yaml -o devtree.html")
        return 0

    if not args.tree or not args.state:
        print("错误: 需要同时提供树文件和状态文件（或用 --import-dts 生成骨架）", file=sys.stderr)
        return 2
    tree_path, state_path = Path(args.tree), Path(args.state)
    for p in (tree_path, state_path):
        if not p.exists():
            print(f"错误: {p} 不存在", file=sys.stderr)
            return 2

    blocking = set(DEFAULT_BLOCKING)
    if args.block_on_partial:
        blocking.add("partial")

    try:
        model = build_model(tree_path, state_path, blocking)
    except DataError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 2
    if args.title:
        model.title = args.title

    print_report(model)

    if args.strict:
        model.errors.extend(model.warnings)
        model.warnings.clear()

    if model.errors:
        print(f"\n校验失败: {len(model.errors)} 个错误，未生成输出。", file=sys.stderr)
        return 1
    if args.check:
        print("[check] 通过")
        return 0

    fmt = args.format
    default_name = {
        "html": "devtree.html", "dot": "devtree.dot", "svg": "devtree.svg",
        "png": "devtree.png", "mermaid": "devtree.mmd", "json": "devtree.json",
    }[fmt]
    out = Path(args.out or default_name)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)

    try:
        if fmt == "html":
            render_html(model, out)
        elif fmt == "dot":
            out.write_text(render_dot(model), encoding="utf-8", newline="\n")
        elif fmt == "mermaid":
            out.write_text(render_mermaid(model), encoding="utf-8", newline="\n")
        elif fmt == "json":
            out.write_text(render_json(model), encoding="utf-8", newline="\n")
        elif fmt in ("svg", "png"):
            dot_bin = shutil.which("dot")
            if not dot_bin:
                raise DataError("未找到 Graphviz 的 dot，可改用 --format dot 后自行渲染")
            with tempfile.TemporaryDirectory() as td:
                src = Path(td) / "in.dot"
                src.write_text(render_dot(model), encoding="utf-8")
                proc = subprocess.run([dot_bin, f"-T{fmt}", str(src), "-o", str(out)],
                                      capture_output=True, text=True)
                if proc.returncode != 0:
                    raise DataError(f"dot 渲染失败: {proc.stderr.strip()}")
    except DataError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 2

    output_summary(model, out)
    if args.open and fmt == "html":
        webbrowser.open(out.resolve().as_uri())
    return 0


if __name__ == "__main__":
    sys.exit(main())
