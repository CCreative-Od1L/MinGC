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

尺寸检测在 `allocator` 分配入口处完成，若 `size > MAX_SIZE` 直接返回 `nullptr`。

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
  alloc 返回的是这个地址 - 1，即 header 位置
```

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

## 阶段规划

| 阶段 | 内容 | 文件 | 状态 |
|------|------|------|------|
| Stage 1 | GCObject Header + Space + Heap + Allocator（基本分配） | `src/gc/gcobject.h`<br>`src/memory/space.hpp`<br>`src/memory/heap.hpp`<br>`src/memory/allocator.hpp` | 进行中 |
| Stage 2 | GC Roots + 三色标记 | 未开始 |
| Stage 3 | 复制算法（Minor GC）+ 对象晋升 | 未开始 |
| Stage 4 | 标记-清除 / 标记-整理（Full GC） | 未开始 |
| Stage 5 | 引用类型（Soft/Weak/Phantom）+ Card Table | 未开始 |

## 模块依赖

```
MinGC/
├── src/
│   ├── gc/
│   │   └── gcobject.h
│   └── memory/
│       ├── space.hpp
│       ├── heap.hpp
│       └── allocator.hpp
└── main.cpp (待实现)
```

依赖关系：

```
main.cpp
   │
   ▼
allocator.hpp ──► heap.hpp ──► space.hpp
   │               │
   ▼               ▼
gcobject.h      gcobject.h
```

## 设计决策记录

| 决策 | 选择 | 原因 |
|------|------|------|
| 平台目标 | 32 位优先 | 简化实现，减少对齐复杂度 |
| Header 打包 | 单个 uint32_t + 位操作 | 避免位域跨编译器行为不一致 |
| 分配策略 | bump-the-pointer | 最简实现，无碎片 |
| Survivor 数量 | 2 个（S0/S1） | 满足复制算法需要 |

## 术语表

- **bump-the-pointer**：指针碰撞分配，Eden 区维护一个 top 指针，新分配时移动 top 即可
- **Minor GC**：针对 Young Gen 的垃圾回收
- **Full GC**：针对整个堆的垃圾回收
- **晋升**：对象从 Young Gen 移动到 Old Gen