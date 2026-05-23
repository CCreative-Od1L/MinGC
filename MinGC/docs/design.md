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
| Old Gen | 晋升失败 或 Old Gen 满了 | 标记-整理 / 标记-清除 |

## 三色标记算法

标记阶段采用保守式扫描：将对象体中每个对齐 word 都视为潜在指针，通过 `which_ptr()` 判断是否指向堆内。无法精确区分指针和整数，可能产生少量浮动垃圾（false positive），但不会漏标存活对象。

标记流程：
1. 从 GC Roots 出发，将所有根指向的白色对象推入灰色队列
2. 循环处理灰色队列：弹出对象 → 标记为黑 → 扫描对象体中的所有潜在指针 → 将白色子对象推入灰色队列
3. 标记结束后，统一清除所有标记位（Stage 2 暂不做回收）

## GC Roots 管理

根集合使用 `std::unordered_set<void**>` 存储，`void**` 而非 `void*` 的目的是为 Stage 3 复制算法中根指针的更新做准备。提供 `gc_add_root()` / `gc_remove_root()` API。

## 阶段规划

| 阶段 | 内容 | 文件 | 状态 |
|------|------|------|------|
| Stage 1 | GCObject Header + Space + Heap（基本分配） | `src/gc/gcobject.h`<br>`src/memory/space.h`<br>`src/memory/heap.h` | 已完成 |
| Stage 2 | GC Roots + 三色标记 | `src/gc/root.h`<br>`src/gc/mark.h`<br>`src/gc/collector.cpp` | 已完成 |
| Stage 3 | 复制算法（Minor GC）+ 对象晋升 | | 未开始 |
| Stage 4 | 标记-清除 / 标记-整理（Full GC） | | 未开始 |
| Stage 5 | 引用类型（Soft/Weak/Phantom）+ Card Table | | 未开始 |

## 模块依赖

```
MinGC/
├── src/
│   ├── main.cpp
│   ├── gc/
│   │   ├── gcobject.h
│   │   ├── root.h
│   │   ├── mark.h
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
  │  │
  │
collector.cpp ──→ heap.h + mark.h
```

- `collector.cpp` 定义 `Heap heap` 全局单例和 `Heap::collect_minor_gc()`
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

## 术语表

- **bump-the-pointer**：指针碰撞分配，Eden 区维护一个 top 指针，新分配时移动 top 即可
- **Minor GC**：针对 Young Gen 的垃圾回收
- **Full GC**：针对整个堆的垃圾回收
- **晋升**：对象从 Young Gen 移动到 Old Gen
- **三色标记**：将对象分为白（未扫描）、灰（待扫描）、黑（已扫描）三类，从根出发迭代标记
- **保守式扫描**：不依赖类型信息，将对象体中每个对齐 word 都视为潜在指针
- **GC Roots**：垃圾回收的起始点，包括栈变量、全局变量等。本项目通过显式 API 注册
