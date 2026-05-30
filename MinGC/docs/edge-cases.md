# MinGC 边界情况与 Bug 修复记录

## 1. 自由链表头节点移除错误

**阶段**：Stage 4 — `allocate_raw` 自由链表实现

**现象**：自由链表分配后头节点未正确更新，导致下次分配命中已移除的节点。

**原因**：
```cpp
// 错误写法
if (prev == nullptr) {
    cur->next_block = cur->next_block;  // 什么都没改
}
```

头节点需要更新 `free_list` 本身，而非节点字段。

**修复**（`space.h:44`）：
```cpp
if (prev) {
    prev->next_block = cur->next_block;  // 中间节点：更新前驱
} else {
    free_list = cur->next_block;          // 头节点：更新 free_list 自身
}
```

**检测方式**：`test_stage4_free_list_reuse` 中 `allocate_raw` 返回的地址与 FreeBlock 地址不一致。

---

## 2. FreeBlock 越界覆写相邻对象（x64 关键 bug）

**阶段**：Stage 4 — `sweep_old` 实现

**现象**：Full GC 清扫后 `free_list->size` 显示错误值（4），且 `allocate_raw` 返回错误地址。测试 `test_stage4_free_list_reuse` 断言 `reused == header_addr` 失败。

**根因分析**：

```
x64 平台:
  sizeof(FreeBlock) = 16  (size_t=8 + pointer=8)
  sizeof(GCObject)  = 4   (uint32_t)

gc_malloc(8) → total_size = 4 + 8 = 12 (aligned)
               12 < 16 (sizeof FreeBlock)

sweep_old 在偏移 0 处创建 FreeBlock:
  reinterpret_cast<FreeBlock*>(header) at offset 0
  block->size       = 12  ← 写入 bytes 0~7
  block->next_block = ... ← 写入 bytes 8~15

bytes 8~11:  对象 1 的 body 内
bytes 12~15: 对象 2 的 header  ← 越界覆写！
```

对象 2 的 header 被 `next_block` 的零值覆盖，`total_size()` 返回 4（body_size=0）。sweep 的 `cur` 前进量从正确的 12 变为 4，导致后续所有对象的解析全部偏移错位。

**修复**（`collector.cpp:96`）：
```cpp
else if (obj_size >= sizeof(FreeBlock)) {
    // 仅当尺寸足够时才创建 FreeBlock
    FreeBlock* block = reinterpret_cast<FreeBlock*>(header);
    ...
}
// 尺寸不足 → 跳过不回收（变为 dark matter）
```

**检测方式**：debug 输出 `free_list->size == 4`（期望 ≥ 12），且 `free_list` 地址位于对象 body 中间而非 header。

---

## 3. allocate_raw 消费后残留 FreeBlock 元数据

**阶段**：Stage 4 测试

**现象**：`test_stage4_old_gen_dead_reclaim` 失败，因为 test 1 中 `allocate_raw` 消费了 FreeBlock 但没有写回有效的 GCObject header。下一轮 sweep 经过该地址时，将 FreeBlock 的 `size` 字段（8 字节整数）当作 GCObject header 解析，得到错误的 `total_size`，导致 `cur` 前进量错误，跳过后续的有效对象。

**流程**：
```
Test 1: allocate_raw(68) → 返回 FreeBlock 地址 → test 结束
        地址上的数据仍是 {size=68, next=nullptr}

Test 2: sweep_old 扫描到该地址
         将 size=68 的低 4 字节 (0x00000044) 当作 header_t
         解析出 body_size=2 → total_size=8 → cur 仅前进 8
         原来 68 字节的空间被错误地片断化扫描
```

**修复**：测试中 `allocate_raw` 返回后写入有效的 GCObject header：
```cpp
static_cast<GCObject*>(reused)->set_size(sz - sizeof(GCObject));
```

**防御性改进**：实际生产代码中 `allocate_raw` 的调用方（`collect_minor_gc` 的复制阶段）总是通过 `memcpy` 立即写入完整对象数据，不会出现此问题。仅直接调用 `allocate_raw` 的测试代码需要注意。

---

## 4. 软引用在 Minor GC 中未更新指针

**阶段**：Stage 5 — SoftRef 集成

**现象**：`test_stage5_softref_cleared_on_oom` 中 `which_ptr(soft_target)` 在预期为 `Old` 时返回 `Eden`。

**根因**：

```
Minor GC 执行流程:
  1. mark_phase  ← 软引用作为额外根，目标被标记
  2. copy        ← 目标从 Eden 复制到 to_space
  3. root fix    ← roots 的指针被 forwarding_map 更新
  4. [缺]         软引用指针未更新
  5. weak fix
  6. reset/swap  ← Eden 被回收

GC 结束后:
  soft_target 仍指向 Eden 旧地址（已回收空间）
  下一轮 GC 的 mark_phase 拿到旧地址 → from_user_ptr → 旧 header
  → is_marked() = false（上轮末尾 clear_mark 过了）
  → 对象未被推入 grey_list → 对象丢失
```

弱引用没有此问题，因为 `clear_dead_weak_refs(&forwarding_map)` 在每轮 GC 末尾执行转发了。

**修复**（`collector.cpp`）：在 root fix 之后、body fix 之前，添加软引用指针更新循环（与 root fix 逻辑完全一致）：
```cpp
for (auto r : soft_refs) {
    void* old_user_ptr = *r;
    if (!old_user_ptr) continue;
    GCObject* old_header = GCObject::from_user_ptr(old_user_ptr);
    auto it = forwarding_map.find(old_header);
    if (it != forwarding_map.end()) {
        *r = it->second->user_ptr();
    }
}
```

**检测方式**：对象经历了 4 次 Minor GC 后 `which_ptr()` 仍返回 `Eden` 而非 `Old`。

---

## 5. mark_phase extra_roots 默认 nullptr 导致空指针解引用

**阶段**：Stage 5 — mark_phase 重构

**现象**：Stage 2 测试（如 `test_stage2_mark_root`）调用 `mark_phase(marked)` 不传第二个参数时崩溃。

**根因**：
```cpp
mark_phase(marked_list, extra_roots = nullptr)
// ...
for (void** slot : *extra_roots) {  // extra_roots == nullptr → 解引用崩溃
```

**修复**（`mark.h:41`）：
```cpp
if (extra_roots) {
    for (void** slot : *extra_roots) {
        ...
    }
}
```

---

## 6. collect_full_gc 默认参数重复定义

**阶段**：Stage 5 — 测试集成

**现象**：MSVC 编译错误 `C2572: "Heap::collect_full_gc": 重定义默认参数 : 参数 1`。

**根因**：声明（`heap.h:55`）和定义（`collector.cpp:111`）两处都有 `= false`。

**修复**：仅在声明处保留默认值，定义处去掉：
```cpp
// heap.h
void collect_full_gc(bool is_oom = false);

// collector.cpp
void Heap::collect_full_gc(bool is_oom) { ... }
```

---

## 7. 暗物质（Dark Matter）— 设计性边界

**阶段**：Stage 4 — sweep_old 实现

**不是 bug，是已知设计取舍**。`total_size < sizeof(FreeBlock)` 的死对象无法容纳 FreeBlock 头，因此永久占用 Old Gen 空间。

**阈值**：
| 平台 | sizeof(FreeBlock) | 最小可回收 body_size |
|------|--------------------|------|
| x64 | 16 | 12 |
| x86 | 8 | 4 |

**当前影响**：测试套件中 3 个 `gc_malloc(sizeof(void*))` (=8 bytes) 对象晋升 Old Gen 后成为暗物质，合计占用 36 字节。

详见 `docs/dark-matter.md`。

---

## 8. 两层尺寸断裂

**阶段**：Stage 4 — 设计分析

`Space::allocate`（Eden 分配）会 padding `total_size` 到 `≥ sizeof(FreeBlock)`，但 `GCObject::total_size()` 始终从 header 的 body_size 重新计算，不感知 padding。

```
gc_malloc(8):
  Eden: allocate(8) → 物理 16 字节, set_size(8)
  晋升: allocate_raw(total_size()) → 12 字节在 Old Gen
        (total_size = 4 + 8 = 12)
```

Eden 中物理占用 16 字节，晋升后在 Old Gen 只占 12 字节。12 < sizeof(FreeBlock)=16 → 暗物质。

这不是 bug，因为：
1. Eden 对象不会被 sweep 扫描（Eden 用 reset 回收）
2. 晋升时 `allocate_raw(total_size())` 使用逻辑尺寸，对象在 Old Gen 的实际占用与 `total_size()` 一致
3. sweep 扫描的是 Old Gen，`total_size()` 与物理占用完全一致

**边界情况**：如果未来在 Eden 中做就地 sweep（而非 reset），则需要统一两层尺寸。

---

## 边界情况总结

| # | 类别 | 严重度 | 检测阶段 | 修复方式 |
|---|------|--------|---------|---------|
| 1 | 逻辑错误 | 高 | Stage 4 test | 修正 free_list 头节点更新分支 |
| 2 | 内存越界 | 高 | Stage 4 test | 尺寸守卫：`>= sizeof(FreeBlock)` 才创建 FreeBlock |
| 3 | 残留元数据 | 中 | Stage 4 test | 消费后写有效 header（生产代码由 memcpy 自动覆盖） |
| 4 | 指针未更新 | 高 | Stage 5 test | 添加 soft ref forwarding 循环 |
| 5 | 空指针解引用 | 高 | Stage 2 test | `if (extra_roots)` 守卫 |
| 6 | 编译错误 | 低 | 构建 | 移除定义处的重复默认参数 |
| 7 | 设计取舍 | 信息 | 设计审查 | 接受，记录在 dark-matter.md |
| 8 | 设计分析 | 信息 | 设计审查 | 无 bug，仅说明设计一致性 |
