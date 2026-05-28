# MinGC 设计文档

## 项目概述

实现一个 C++ 语言的最小垃圾收集器，用于了解 GC 的底层原理。

## 平台目标

- **当前实现目标**：32 位系统
- **未来支持**：64 位系统（设计时已考虑，但暂不实现）

## 核心数据结构

### GCObject Header（32 位）

```
位布局： [0:mark][1-4:age][5-31:size]

┌────────────────────────────────────────┐
│ 31                           5 4   1 0 │
│         size(27bit)            age   m │
└────────────────────────────────────────┘
```

- **mark (bit 0)** — 0=白(未标记)，1=黑(已标记)
- **age (bit 1~4, 4 bits)** — 存活年龄，0~15，超过阈值晋升 Old Gen
- **size (bit 5~31, 27 bits)** — 对象体大小（字节），最大 128MB

单对象最大尺寸限制：27 位无符号整数，最大值 `0x7FFFFFF = 134,217,727 字节 ≈ 128MB`

尺寸检测在 `heap.allocate()` 分配入口处完成，若 `size > MAX_OBJECT_SIZE` 直接返回 `nullptr`。

### Header 大小

- **32 位系统**：`sizeof(header_t) = 4 字节`，自然对齐
- **64 位系统**（预留）：`sizeof(header_t) = 8 字节`，需要条件编译区分

### 对象内存布局

```
┌─────────────────────┬─────────────────────────┐
│  GCObject Header    │     Object Body         │
│     4 bytes         │    user_size bytes      │
└─────────────────────┴─────────────────────────┘
      ↑
  alloc 返回的是 Header 之后的首地址（user_ptr）
```

Space::allocate 在分配时会对 total_size 做 4 字节对齐，确保所有对象起始地址对齐。

### 内存布局

```
┌──────────────────────────────────────────────────┐
│  Young Generation             │  Old Generation  │
│  ┌────────┬────────┬────────┐ │  ┌────────────┐  │
│  │  Eden  │ S0     │ S1     │ │  │  Old Gen   │  │
│  │ (8 MB) │ (1 MB) │ (1 MB) │ │  │  (10 MB)   │  │
│  └────────┴────────┴────────┘ │  └────────────┘  │
└──────────────────────────────────────────────────┘
```

- **Eden** — 新对象分配区，使用 bump-the-pointer
- **S0 / S1** — Survivor 区，Minor GC 时角色互换（复制算法）
- **Old Gen** — 长期存活对象，Full GC 时回收

## 分代设计

| 分代 | 触发条件 | 回收算法 |
|------|----------|----------|
| Young Gen (Eden + S0 + S1) | Eden 满了 | 复制算法 |
| Old Gen | 晋升失败 或 Old Gen 满了 | 标记-清除（自由链表） |

## 三色标记算法

标记阶段采用保守式扫描：将对象体中每个对齐 word 都视为潜在指针，通过 `which_ptr()` 判断是否指向堆内。无法精确区分指针和整数，可能产生少量浮动垃圾（false positive），但不会漏标存活对象。

标记流程：
1. 从 GC Roots 出发，将所有根指向的白色对象推入灰色队列
2. 循环处理灰色队列：弹出对象 → 标记为黑 → 扫描对象体中的所有潜在指针 → 将白色子对象推入灰色队列
3. 标记结束后，由 GC 入口函数负责清理标记位——Minor GC 在复制完成后清除新对象标记，Full GC 在清扫阶段清除 Old Gen 存活对象标记

## GC Roots 管理

根集合使用 `std::unordered_set<void**>` 存储，`void**` 而非 `void*` 的目的是为 Stage 3 复制算法中根指针的更新做准备。提供 `gc_add_root()` / `gc_remove_root()` API。

## Minor GC（复制算法）

### 两遍扫描（Two-Pass）

复制回收分为以下步骤：

**Pass 1：复制**
- 遍历三色标记得到的 `marked_list`
- 跳过 Old Gen 对象（不参与 Minor GC）
- 根据年龄选择目标：`age >= PROMOTION_AGE` → Old Gen，否则 → to_space
- 在目标空间用 `allocate_raw` 分配，`memcpy` 整体搬运 Header + Body
- 建立 `forwarding_map[old] = new`，用于后续指针修复
- 调用 `inc_age()` 递增存活次数

**Pass 2：修复指针**
- **根修复**：遍历 `roots` 集合，若根指向的对象在 `forwarding_map` 中，用新地址覆写 `*root`
- **Body 修复**：遍历所有新对象，保守扫描 body 中的每个 word；若指向已移动对象，用 `forwarding_map` 更新

### 空间管理

- 复制后 `eden.reset()` 清空 Eden
- `swap_survivors()`：重置 `from_space`，交换 `from_space` / `to_space` 角色

### 晋升

晋升年龄阈值 `PROMOTION_AGE = 3`，即对象经历 4 次 Minor GC（首次在 Eden 创建，3 次在 Survivor 间翻转）后晋升至 Old Gen。

## Old Gen 自由链表

### FreeBlock 结构

```cpp
struct FreeBlock {
    size_t     size;       // 本块总大小（含 FreeBlock 头）
    FreeBlock* next_block; // 下一块
};
```

FreeBlock **覆写在死对象原占用的内存上**，不额外分配元数据空间。分配时返回的 `FreeBlock*` 即是可复用空间的起始地址。

### 最小尺寸约束

为确保障所有对象（包括 0 字节 body 的最小对象）在回收后有足够的空间存放 FreeBlock 头，`Space::allocate` 强制 `total_size >= sizeof(FreeBlock)`。

### 分配路径

`allocate_raw` 采用首次适配策略：

1. 遍历自由链表，找 `size >= requested` 的块
2. 若剩余空间 >= `sizeof(FreeBlock)`，则分裂块（前端返回，残块留链）
3. 否则整块返回，从链表中摘除
4. 自由链表无满足块时，回退 bump-the-pointer

### 清扫（Sweep）

`collect_full_gc()` 的清扫阶段线形扫描 Old Gen：

```
cursor = heap.old.start
while cursor < heap.old.top:
    obj = (GCObject*)cursor
    if obj->is_marked():
        obj->clear_mark()              // 存活：重置标记
    else if obj->total_size() >= sizeof(FreeBlock):
        fb = (FreeBlock*)obj           // 死亡：原地创建 FreeBlock
        fb->size = obj->total_size()
        fb->next_block = free_list
        free_list = fb                 // 头插法入链
    cursor += obj->total_size()
```

- 只扫描到 `top`（已分配边界）
- 头插法避免遍历查找尾节点
- Old Gen 对象不移动，仅标记为可复用
- `total_size < sizeof(FreeBlock)` 的对象无法容纳 FreeBlock 头，跳过不回收（变成 **dark matter**）
- `allocate_raw` 消费 FreeBlock 后，调用方负责写入有效的 GCObject header（`set_size`），确保后续 sweep 能正确解析该区域

## 引用类型（WeakRef / SoftRef）

### 概述

除强引用（`roots`）外，引入两种弱化引用级别，允许在不阻止 GC 的前提下感知对象生命周期。

| 类型 | 阻止回收 | 置 null 时机 |
|------|---------|-------------|
| **WeakRef** | 否 | 每次 GC 后，若目标已死 |
| **SoftRef** | 正常 GC: 是；OOM GC: 否 | OOM 时目标已死 |

两者均使用 `void**` 表示槽位——GC 可直接覆写用户持有的指针，与 Roots 机制一致。

### WeakRef 处理流程

`clear_dead_weak_refs(forwarding_map*)` 在 GC 后遍历所有弱引用槽位：

- **Minor GC**（`forwarding_map` 非空）：若目标在 forwarding_map 中 → 更新为新地址；否则若未标记 → 置 null
- **Full GC**（`forwarding_map == nullptr`）：纯 `is_marked()` 判断 → 未标记则置 null

调用时机：Minor GC 在 body fix 之后、reset/swap 之前；Full GC 在 sweep 之前。

### SoftRef 处理流程

- **正常 GC**：`mark_phase` 将软引用目标作为额外根（`extra_roots` 参数），保证 GC 不回收它们
- **OOM Full GC**：`collect_full_gc(true)` 不将软引用作为根；标记 + 清扫后调用 `clear_dead_soft_refs()` 置空死亡目标的槽位

### 存储

与 Roots 一致，使用全局 `std::unordered_set<void**>` 注册表：

```cpp
extern std::unordered_set<void**> weak_refs;   // collector.cpp 中定义
extern std::unordered_set<void**> soft_refs;   // collector.cpp 中定义
```

API：`gc_add_weak_ref(slot)` / `gc_remove_weak_ref(slot)`，软引用同理。

## 阶段规划

| 阶段 | 内容 | 文件 | 状态 |
|------|------|------|------|
| Stage 1 | GCObject Header + Space + Heap（基本分配） | `src/gc/gcobject.h`<br>`src/memory/space.h`<br>`src/memory/heap.h` | 已完成 |
| Stage 2 | GC Roots + 三色标记 | `src/gc/root.h`<br>`src/gc/mark.h`<br>`src/gc/collector.cpp` | 已完成 |
| Stage 3 | 复制算法（Minor GC）+ 对象晋升 | `src/gc/collector.cpp` | 已完成 |
| Stage 4 | 标记-清除（Full GC）+ 自由链表 | `src/gc/collector.cpp`<br>`src/memory/space.h` | 已完成 |
| Stage 5 | 弱引用（WeakRef）+ 软引用（SoftRef） | `src/gc/weakref.h`<br>`src/gc/softref.h`<br>`src/gc/collector.cpp` | 进行中 |

## 模块依赖

```
MinGC/
├── src/
│   ├── main.cpp
│   ├── gc/
│   │   ├── gcobject.h
│   │   ├── root.h
│   │   ├── mark.h
│   │   ├── weakref.h
│   │   ├── softref.h
│   │   └── collector.cpp
│   └── memory/
│       ├── space.h
│       └── heap.h
```

依赖关系：

```
main.cpp ──→ heap.h ──→ space.h ──→ gcobject.h
  │  │                      ↑
  │  ├──→ mark.h ───────────┘
  │  │     └──→ root.h
  │  ├──→ weakref.h ───→ gcobject.h
  │  └──→ softref.h ───→ gcobject.h
  │
collector.cpp ──→ heap.h + mark.h + weakref.h + softref.h
```

- `collector.cpp` 定义 `Heap heap` 全局单例、`roots`/`weak_refs`/`soft_refs` 集合，实现 `collect_minor_gc()` 和 `collect_full_gc()`
- `heap.h` 声明 `extern Heap heap`、`gc_malloc()`、`which_ptr()`
- 无循环依赖

## 设计决策记录

| 决策 | 选择 | 原因 |
|------|------|------|
| 平台目标 | 32 位优先 | 简化实现，减少对齐复杂度 |
| Header 打包 | 单个 uint32_t + 位操作 | 避免位域跨编译器行为不一致 |
| 分配策略 | bump-the-pointer | 最简实现，无碎片 |
| Survivor 数量 | 2 个（S0/S1） | 满足复制算法需要 |
| 对象扫描 | 保守式（conservative） | 无需对象类型信息，最小实现 |
| 模块划分 | 多 TU（.cpp + .h） | 避免循环依赖，职责分离 |
| GC 入口 | collector.cpp | 分离 GC 逻辑与测试/入口代码 |
| 晋升年龄阈值 | `PROMOTION_AGE = 3` | 4 次 GC 存活后晋升，避免过早/过晚 |
| 最小分配单元 | `sizeof(FreeBlock)` | 保证死对象空间可存放 FreeBlock 头部 |
| Old Gen 自由链表 | 单链表 + 头插法 | Old Gen 死对象就地转为 FreeBlock，无需额外内存 |
| Old Gen 回收算法 | 标记-清除 | 比标记-整理更简单，引入自由链表和碎片化概念，教学价值更高 |
| 小对象处理 | 暗物质（跳过不回收） | 尺寸不足 FreeBlock 的死对象不可回收，简化实现，教学上展示碎片问题 |
| 引用类型存储 | `unordered_set<void**>` | 与 Roots 一致，GC 可直接覆写槽位，无需对象格式信息 |
| SoftRef 存活策略 | 正常 GC 当强引用，OOM 当弱引用 | 教学上最简：一个标记入口参数区分两种行为 |

## 术语表

- **bump-the-pointer**：指针碰撞分配，Eden 区维护一个 top 指针，新分配时移动 top 即可
- **Minor GC**：针对 Young Gen 的垃圾回收
- **Full GC**：针对整个堆的垃圾回收
- **晋升**：对象从 Young Gen 移动到 Old Gen
- **三色标记**：将对象分为白（未扫描）、灰（待扫描）、黑（已扫描）三类，从根出发迭代标记
- **保守式扫描**：不依赖类型信息，将对象体中每个对齐 word 都视为潜在指针
- **GC Roots**：垃圾回收的起始点，包括栈变量、全局变量等。本项目通过显式 API 注册
- **清扫（Sweep）**：线形扫描 Old Gen，将未标记对象回收为 FreeBlock 的过程
- **自由链表（Free List）**：管理 Old Gen 中已回收碎片的单链表，供后续分配时优先复用
- **Forwarding Map**：复制 GC 中的 `old→new` 地址映射，用于 Pass 2 指针修复
- **两遍扫描（Two-Pass）**：复制回收算法，Pass 1 拷贝存活对象建立映射，Pass 2 修正所有指针
- **FreeBlock**：覆写在死对象原址上的元数据结构，仅含 size 和 next 字段，不额外分配内存
- **Dark Matter（暗物质）**：`total_size < sizeof(FreeBlock)` 的死对象，因空间不足以容纳 FreeBlock 头而无法回收，永久占用 Old Gen 空间
- **WeakRef（弱引用）**：不阻止对象回收的引用。GC 后发现目标已死则自动置 `nullptr`
- **SoftRef（软引用）**：正常 GC 下阻止回收，OOM 时退化为弱引用，允许 GC 释放内存
- **OOM（Out of Memory）**：晋升 Old Gen 失败时触发的 Full GC，此模式下软引用不保活
