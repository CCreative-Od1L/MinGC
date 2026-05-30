# MinGC 架构文档

## 1. 项目概述

MinGC 是一个用 C++20 实现的最小分代垃圾收集器，旨在帮助开发者理解 GC 的底层原理。纯标准库实现，无外部依赖，支持 x64/x86。

## 2. 文件结构与模块划分

```
MinGC/
└── src/
    ├── main.cpp                    # 测试入口（21 个测试用例）
    ├── gc/
    │   ├── gcobject.h              # GCObject Header（标记/年龄/大小）
    │   ├── root.h                  # GC Roots 管理（强引用）
    │   ├── mark.h                  # 三色标记算法（保守式扫描）
    │   ├── weakref.h               # WeakRef 弱引用
    │   ├── softref.h               # SoftRef 软引用
    │   └── collector.cpp           # Heap 单例 + Minor GC / Full GC 入口
    └── memory/
        ├── space.h                 # Space 分配器 + FreeBlock 定义
        └── heap.h                  # 堆布局 + 公开分配 API
```

### 2.1 模块职责

| 文件 | 职责 | 暴露接口 |
|------|------|---------|
| `gcobject.h` | GCObject 32-bit header 位操作 | `set_size()` / `get_size()` / `set_mark()` / `inc_age()` / `total_size()` |
| `space.h` | 物理内存管理（bump-the-pointer + 自由链表） | `Space::allocate()` / `Space::allocate_raw()` / `Space::reset()` |
| `heap.h` | 堆布局定义 + GC 触发 | `gc_malloc()` / `which_ptr()` / `collect_minor_gc()` / `collect_full_gc()` |
| `root.h` | 强引用根集合管理 | `gc_add_root()` / `gc_remove_root()` |
| `mark.h` | 三色标记（BFS 遍历） | `mark_phase(marked_list, extra_roots)` |
| `weakref.h` | 弱引用管理 + GC 后清理 | `gc_add_weak_ref()` / `clear_dead_weak_refs()` |
| `softref.h` | 软引用管理 + OOM 清除 | `gc_add_soft_ref()` / `clear_dead_soft_refs()` |
| `collector.cpp` | 全局状态定义 + GC 入口实现 | 全局 `Heap heap` / `roots` / `weak_refs` / `soft_refs` |

### 2.2 依赖关系图

```
main.cpp ──→ heap.h ──→ space.h ──→ gcobject.h
  │  │                      ↑
  │  ├──→ mark.h ───────────┘
  │  │     ├──→ root.h
  │  │     └──→ heap.h
  │  ├──→ weakref.h ───→ gcobject.h
  │  └──→ softref.h ───→ gcobject.h
  │
collector.cpp ──→ heap.h + mark.h + weakref.h + softref.h
```

无循环依赖。所有 `.h` 文件均可由 `main.cpp` 独立包含。

## 3. 内存布局

### 3.1 堆空间划分

```
┌─── 20 MB total ─────────────────────────────────────────────┐
│ Young Generation (10 MB)     │ Old Generation (10 MB)       │
│ ┌────────┬────────┬────────┐ │ ┌──────────────────────────┐ │
│ │  Eden  │   S0   │   S1   │ │ │        Old Gen           │ │
│ │ (8 MB) │ (1 MB) │ (1 MB) │ │ │       (10 MB)            │ │
│ └────────┴────────┴────────┘ │ └──────────────────────────┘ │
└──────────────────────────────┴──────────────────────────────┘
   bump-the-pointer  复制互换        bump + 自由链表
```

- **Eden (8 MB)**：新对象分配区，使用 bump-the-pointer 分配器
- **S0 / S1 (各 1 MB)**：Survivor 区，Minor GC 时复制算法在两者间来回搬运，角色互换
- **Old Gen (10 MB)**：长期存活对象。bump-the-pointer + 自由链表（Full GC 后重建）

### 3.2 对象内存布局

```
┌─────────────────────┬─────────────────────────┐
│  GCObject Header    │     Object Body         │
│     4 bytes         │    user_size bytes      │
└─────────────────────┴─────────────────────────┘
       ↑                           ↑
    header_t                alloc 返回此地址
  (GCObject*)              (user_ptr)
```

### 3.3 GCObject Header 位布局 (32-bit)

```
位布局： [0:mark][1-4:age][5-31:size]

┌────────────────────────────────────────┐
│ 31                           5 4   1 0 │
│         size(27bit)            age   m │
└────────────────────────────────────────┘
```

| 字段 | 位 | 宽度 | 含义 |
|------|----|------|------|
| mark | 0 | 1 bit | 三色标记位：0=白(未标记)，1=黑(已标记) |
| age | 1~4 | 4 bits | 存活年龄，0~15，≥ PROMOTION_AGE(3) 晋升 Old Gen |
| size | 5~31 | 27 bits | 对象体大小（字节），最大 128 MB |

### 3.4 FreeBlock 结构（Old Gen 自由链表节点）

```cpp
struct FreeBlock {
    size_t     size;        // 本块总大小（含 FreeBlock 自身）
    FreeBlock* next_block;  // 下一块
};
```

- **覆写复用**：FreeBlock 直接覆写在死对象原占用的内存上，不额外分配
- **最小尺寸**：`sizeof(FreeBlock)` = 16 bytes (x64) / 8 bytes (x86)
- **尺寸约束**：`Space::allocate()` 和 `GCObject::total_size()` 均保证 ≥ `sizeof(FreeBlock)`

## 4. 核心抽象

### 4.1 Space — 内存空间

```cpp
struct Space {
    uint8_t*   start;      // 空间起始
    uint8_t*   top;        // 已分配边界（bump 指针）
    uint8_t*   end;        // 空间结束
    FreeBlock* free_list;  // 自由链表头（仅 Old Gen 使用）
};
```

提供两个分配接口：
- **`allocate(user_size)`**：用户 API 入口，自动计算总尺寸 + 对齐 + FreeBlock 最小尺寸检查，用于 Eden
- **`allocate_raw(total_size)`**：GC 内部使用，先查自由链表（first-fit + 分裂），未命中则 bump。用于 to_space 复制和 Old Gen 晋升

### 4.2 Heap — 堆管理器

```cpp
struct Heap {
    Space  eden, s0, s1, old;
    Space* from_space = &s0;
    Space* to_space   = &s1;
    
    void* allocate(size_t user_size);      // Eden 分配，失败触发 Minor GC
    void collect_minor_gc();               // 标记 → 复制 → forwarding → 清扫
    void collect_full_gc(bool is_oom);     // 标记 → 清扫 Old Gen
};
```

### 4.3 引用体系（三层递弱）

```
强引用 (roots)     → 始终阻止回收，标记起始点
软引用 (soft_refs) → 正常 GC 阻止回收；OOM Full GC 退化为弱引用
弱引用 (weak_refs) → 不阻止回收，GC 后目标死亡则自动置 nullptr
```

三者均使用 `std::unordered_set<void**>` 存储槽位地址，GC 可直接覆写 `*slot`。

## 5. 根集合（Roots）

```cpp
extern std::unordered_set<void**> roots;        // 强引用
extern std::unordered_set<void**> weak_refs;    // 弱引用
extern std::unordered_set<void**> soft_refs;    // 软引用
```

`void**` 而非 `void*` 的设计：
- 用户注册 `gc_add_root(&my_ptr)`，GC 保存 `&my_ptr` 的地址
- Minor GC 中对象被复制后，GC 通过 `*root = new_user_ptr` 直接覆写用户的指针变量
- 用户在 GC 后无需重新获取引用——变量自动指向新位置

## 6. 关键算法位置索引

| 算法 | 文件 | 函数 | 行号 |
|------|------|------|------|
| bump-the-pointer 分配 | `space.h` | `Space::allocate` | 19 |
| 自由链表分配 (first-fit) | `space.h` | `Space::allocate_raw` | 35 |
| 三色标记 (BFS) | `mark.h` | `mark_phase` | 24 |
| 保守式指针扫描 | `mark.h` | `scan_object` | 7 |
| Minor GC 入口 | `collector.cpp` | `Heap::collect_minor_gc` | 16 |
| Full GC 入口 | `collector.cpp` | `Heap::collect_full_gc` | 111 |
| Old Gen 清扫 | `collector.cpp` | `sweep_old` | 91 |
| 弱引用清理 | `weakref.h` | `clear_dead_weak_refs` | 16 |
| 软引用推入灰队列 | `softref.h` | `soft_ref_grey_list` | 17 |
| 软引用置空 | `softref.h` | `clear_dead_soft_refs` | 27 |

## 7. 测试覆盖

21 个测试用例覆盖所有阶段：

| 阶段 | 测试数 | 覆盖内容 |
|------|--------|---------|
| Stage 1 | 3 | 基本分配、Oversize 拒绝、Max Object 限制 |
| Stage 2 | 6 | 根标记、对象链标记、不可达不标记、根去重、根移除、空根 |
| Stage 3 | 6 | 对象复制+根更新、对象图修复、Eden 回收、Forwarding 去重、晋升、Old Gen 不移动 |
| Stage 4 | 3 | 自由链表复用、Old Gen 死对象回收、OOM 晋升重试 |
| Stage 5 | 5 | 弱引用死亡置空、弱+强共存、弱引用 Minor GC 更新、软引用保活、软引用 OOM 清除 |
