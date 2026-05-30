# MinGC 工作流程

## 1. 对象分配流程 (`gc_malloc`)

```
用户调用 gc_malloc(size)
    │
    ▼
heap.allocate(user_size)
    │
    ├─ user_size > MAX_OBJECT_SIZE ? → return nullptr
    │
    ▼
eden.allocate(user_size)
    │
    ├─ 计算 total_size = align(sizeof(GCObject) + user_size, 4)
    ├─ total_size < sizeof(FreeBlock) ? → total_size = sizeof(FreeBlock)
    │
    ├─ top + total_size > end ?
    │   YES → collect_minor_gc()          ← 触发 Minor GC
    │   │      └→ eden.allocate(user_size) ← 重试
    │   │         ├─ 成功 → return user_ptr
    │   │         └─ 失败 → return nullptr
    │   NO  → bump top, set_size, return user_ptr
```

### allocate_raw 自由链表路径（GC 内部使用）

```
allocate_raw(total_size)
    │
    ▼
遍历 free_list（first-fit）
    │
    ├─ cur->size >= total_size ?
    │   │
    │   ├─ cur->size - total_size < sizeof(FreeBlock) ?
    │   │   YES → 整块返回，从链表摘除
    │   │   NO  → 分裂块：前端返回，残块留链
    │   │
    │   └─ 返回 cur（Header 起始地址）
    │
    └─ 未命中 → bump pointer
        ├─ top + total_size > end ? → return nullptr
        └─ bump top, return ptr
```

---

## 2. Minor GC 流程 (`collect_minor_gc`)

```
collect_minor_gc()
│
├─ 1. mark_phase(marked_list, &soft_refs)
│      ┌─────────────────────────────────────┐
│      │ 从 roots + soft_refs 出发 BFS 标记  │
│      │ 所有可达对象推入 grey_list          │
│      │ 弹出 → set_mark → 扫描 body →      │
│      │ 新发现未标记子对象 → 推入 grey_list │
│      │ 标记完成 → 存入 marked_list         │
│      └─────────────────────────────────────┘
│
├─ 2. Pass 1: 复制
│      for obj in marked_list:
│          ├─ which_ptr(obj) == Old ? → skip (Old Gen 不参与)
│          ├─ age >= PROMOTION_AGE(3) ? → heap.old.allocate_raw(total_size)
│          │                            → 晋升到 Old Gen
│          └─ 否则 → heap.to_space->allocate_raw(total_size)
│                    → 复制到 to_space
│          │
│          ├─ OOM ? → collect_full_gc(true)        ← is_oom=true 模式
│          │          ├─ mark_phase(nullptr)         (不包含 soft_refs)
│          │          ├─ sweep_old()
│          │          ├─ clear_dead_weak_refs(nullptr)
│          │          └─ clear_dead_soft_refs()     ← 软引用在此被清除
│          │          └→ 重试 allocate_raw
│          │
│          ├─ inc_age()
│          ├─ memcpy(new_obj, old_obj, total_size)
│          └─ forwarding_map[old] = new_obj
│
├─ 3. Root fix（强引用指针更新）
│      for each root slot in roots:
│          ├─ *slot 指向的对象在 forwarding_map 中？
│          │   YES → *slot = forwarding_map[old]->user_ptr()
│          │   NO  → 不变（Old Gen 对象未移动，或已死）
│
├─ 4. Soft ref fix（软引用指针更新）    ← 无此步则导致指针失效
│      for each soft slot in soft_refs:
│          └─ 同上逻辑（forwarding_map 更新）
│
├─ 5. Body fix（对象体内指针修复）
│      for each (old_obj, new_obj) in forwarding_map:
│          扫描 new_obj->body 中每个对齐 word
│          ├─ 值指向堆内？→ from_user_ptr → 查 forwarding_map
│          └─ 匹配 → 更新为 new_user_ptr
│
├─ 6. 弱引用处理
│      clear_dead_weak_refs(&forwarding_map)
│      ├─ 在 forwarding_map 中 → 更新为新地址（目标存活，被移动）
│      ├─ 不在 forwarding_map 且 !is_marked() → *slot = nullptr（目标已死）
│      └─ 不在 forwarding_map 但 is_marked() → 不变（Old Gen 对象，未移动）
│
├─ 7. 空间回收
│      eden.reset()           ← Eden top = start（全部回收）
│      swap_survivors()       ← 重置 from_space, 交换 from/to 角色
│
└─ 8. 标记清理
       for new_obj in forwarding_map → clear_mark()
       for obj in marked_list:
           └─ which_ptr(obj) == Old ? → clear_mark()
```

### 关键时序说明

弱引用和软引用处理**必须在 reset/swap 之前**完成，因为此时旧对象的内存尚未被回收，`is_marked()` 仍可安全读取。`reset/swap` 之后旧空间的标记位被破坏，无法再准确判断对象生死。

---

## 3. Full GC 流程 (`collect_full_gc`)

```
collect_full_gc(is_oom = false)
│
├─ 1. 标记阶段
│      is_oom == false ?
│         mark_phase(marked_list, &soft_refs)   ← 正常模式，软引用当强根
│      :
│         mark_phase(marked_list, nullptr)      ← OOM 模式，软引用不参与
│
├─ 2. sweep_old() — 线形扫描 Old Gen
│      ┌─────────────────────────────────────────────┐
│      │ old_gen.free_list = nullptr  (全量重建)     │
│      │ cursor = old_gen.start                      │
│      │ while cursor < old_gen.top:                 │
│      │     obj = (GCObject*)cursor                 │
│      │     obj_size = obj->total_size()            │
│      │                                              │
│      │     if obj->is_marked():                    │
│      │         obj->clear_mark()   ← 存活：重置    │
│      │     else if obj_size >= sizeof(FreeBlock):  │
│      │         FreeBlock → 覆写在原址              │
│      │         头插法入链                          │
│      │     else:                                   │
│      │         ← 暗物质：不够大，跳过不回收        │
│      │                                              │
│      │     cursor += obj_size                      │
│      └─────────────────────────────────────────────┘
│
├─ 3. clear_dead_weak_refs(nullptr)
│      纯 is_marked() 判断：未标记 → *slot = nullptr
│
└─ 4. is_oom == true ?
        └→ clear_dead_soft_refs()
            纯 is_marked() 判断：未标记 → *slot = nullptr
```

### 为什么每次 Full GC 都全量重建 free_list？

因为标记阶段改变了 Old Gen 中对象的标记状态，原有 free_list 反映的是上一轮 GC 后的状态，已过时。全量重建确保 free_list 精确反映当前轮次中所有不可达对象的位置。

### 暗物质（Dark Matter）

`obj->total_size() < sizeof(FreeBlock)` 的死对象无法容纳 FreeBlock 头（`next_block` 指针会溢出到相邻对象），因此**跳过不回收**。它们永久占用 Old Gen 空间。

当前项目中产生暗物质的条件：对象 body_size < 12 字节（x64）且被晋升到 Old Gen 后死亡。测试套件中共 3 个此类对象，合计浪费 ~36 字节。

---

## 4. 引用类型决策树

```
                    ┌──────────────┐
                    │  gc_alloc    │
                    │  分配对象    │
                    └──────┬───────┘
                           │
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
     gc_add_root      gc_add_soft_ref   gc_add_weak_ref
     (强引用)          (软引用)          (弱引用)
          │                │                │
          ▼                ▼                ▼
    ┌──────────┐    ┌──────────────┐   ┌──────────┐
    │ 始终保活 │    │ 正常GC: 保活 │   │ 永不保活 │
    │ Minor GC │    │ OOM GC: 清除 │   │ GC 后    │
    │ 地址随迁 │    │ 地址随迁      │   │ 死→null  │
    └──────────┘    └──────────────┘   │ 活→随迁  │
                                       └──────────┘
```

### OOM 触发路径

```
collect_minor_gc()
  └→ copy 阶段
       └→ allocate_raw(total_size) in Old Gen → nullptr
            └→ collect_full_gc(true)        ← is_oom=true
                 ├→ mark_phase(nullptr)      软引用不作为根
                 ├→ sweep_old()              回收 Old Gen 死对象
                 ├→ clear_dead_weak_refs()
                 └→ clear_dead_soft_refs()   软引用被清除
            └→ allocate_raw(total_size) 重试
```

---

## 5. 数据流总结

```
              gc_malloc(size)
                   │
                   ▼
        ┌──────────────────────┐
        │   Heap::allocate()   │◄── Eden 分配 + Minor GC 触发
        └──────────┬───────────┘
                   │
         ┌─────────┴─────────┐
         │                   │
         ▼                   ▼
   Eden bump 成功      Eden 满了
         │                   │
         ▼                   ▼
   return user_ptr    collect_minor_gc()
                           │
              ┌────────────┼──────────────┐
              │            │              │
              ▼            ▼              ▼
         mark_phase    copy → OOM?   clear/reset
         (+soft_refs)    │    → Full GC    │
                         │         ↓       ▼
                    root/soft/body    free_list ← sweep_old
                    fix → weak fix
```
