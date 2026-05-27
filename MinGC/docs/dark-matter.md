# Dark Matter（暗物质）

## 定义

`total_size < sizeof(FreeBlock)` 的死对象，因空间不足以容纳 FreeBlock 头部而无法被 Full GC 回收。清扫阶段跳过这些对象，它们永久占用 Old Gen 空间。

## 产生条件

```
gc_malloc(N) → Minor GC × 4 → 晋升 Old Gen → 失去根引用 → Full GC → sweep 跳过
```

核心守卫代码 (`src/gc/collector.cpp:96`)：

```cpp
else if (obj_size >= sizeof(FreeBlock)) {
    FreeBlock* block = reinterpret_cast<FreeBlock*>(header);
    block->size = obj_size;
    block->next_block = old_gen.free_list;
    old_gen.free_list = block;
}
// 不满足条件 → 跳过，不创建 FreeBlock
cur += obj_size;  // 仍按 total_size() 前进，迭代不受影响
```

## 尺寸阈值

| 平台 | `sizeof(FreeBlock)` | 最小可回收 body_size | `total_size()` |
|------|--------------------|-----------------------|----------------|
| x64 | 16 | 12 | 16 |
| x86 | 8 | 4 | 8 |

计算公式：

```
total_size() = ALIGN_UP(sizeof(GCObject) + body_size, HEADER_ALIGN)
           ≥ sizeof(FreeBlock)
→ body_size ≥ sizeof(FreeBlock) - sizeof(GCObject)
```

## 两层尺寸断裂

Eden 分配和 Old Gen 晋升使用不同的尺寸逻辑，导致同一对象的物理占用在两面不一致：

| 阶段 | 函数 | 物理分配 | 存储到 Header | `total_size()` 返回值 |
|------|------|---------|--------------|---------------------|
| Eden 分配 | `Space::allocate(8)` | 16 字节（padding 到 `≥ sizeof(FreeBlock)`） | `set_size(8)` → body=8 | 12 |
| 晋升到 Old Gen | `allocate_raw(total_size())` | 12 字节（`total_size()=12`） | Header 由 `memcpy` 原样复制 | 12 |

断裂点：`gc_malloc(8)` 在 Eden 物理占 16 字节，晋升后在 Old Gen 只占 12 字节。`total_size()` 始终返回 12，`12 < 16` → dark matter。

相关代码：

- `Space::allocate`, padding 逻辑: `src/memory/space.h:23-24`
- `GCObject::total_size()`: `src/gc/gcobject.h:59-62`
- `FreeBlock` 结构: `src/memory/space.h:5-8`

## 当前存在的暗物质

测试套件运行完毕后，以下对象留在 Old Gen 中成为暗物质（x64 平台，`sizeof(void*) = 8`）：

| 测试 | 对象 | body_size | total_size | Old Gen 占用 |
|------|------|-----------|------------|-------------|
| `test_stage3_promotion` | `p` | 8 | 12 | 12 bytes |
| `test_stage3_old_gen_not_moved` | `p` | 8 | 12 | 12 bytes |
| `test_stage4_oom_promotion_retry` | `c` | 8 | 12 | 12 bytes |
| **合计** | — | — | — | **36 bytes** |

其他测试使用 `gc_malloc(64)` 或更大尺寸，不产生暗物质。

## 设计取舍

### 优点

- **简单**：只需一个 `>= sizeof(FreeBlock)` 条件，无需额外元数据
- **安全**：避免 `FreeBlock.next_block`（8 字节指针）越界写入相邻对象 header
- **迭代正确**：`cur += total_size()` 前进量始终等于对象的实际物理尺寸（晋升后），不会错位

### 代价

- **内存泄漏**：小对象晋升到 Old Gen 后无法回收，永久占用空间
- **碎片累积**：在大量小对象晋升的场景下，暗物质占比不可忽略

### 潜在改进（当前阶段未实现）

1. **晋升时 padding**：`collect_minor_gc` 中对 Old Gen 分配做 `max(total_size, sizeof(FreeBlock))` padding，使所有 Old Gen 对象均可回收。需同步修正 `total_size()` 或引入独立字段记录真实分配尺寸。
2. **相邻暗物质合并**：sweep 时检测连续多个暗物质对象，合并为一块 FreeBlock（总尺寸 ≥ sizeof(FreeBlock) 时）。
3. **改变 FreeBlock 结构**：使用更紧凑的表示（如 32 位偏移量替代 64 位指针），缩小 `sizeof(FreeBlock)`。

当前 toy GC 中暗物质总量仅 36 字节（10 MB Old Gen 中可忽略），暂不实现上述改进。
