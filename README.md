# MinGC

## 项目简介

该项目旨在实现一个C++语言的最小垃圾收集器（Garbage Collector），用于帮助开发者了解GC的底层原理。

## 当前进度

| 阶段 | 内容 | 状态 |
|------|------|------|
| Stage 1 | GCObject Header + Space + Heap（基本分配） | 已完成 |
| Stage 2 | GC Roots + 三色标记 | 已完成 |
| Stage 3 | 复制算法（Minor GC）+ 对象晋升 | 已完成 |
| Stage 4 | 标记-清除（Full GC）+ 自由链表 | 已完成 |
| Stage 5 | 弱引用（WeakRef）+ 软引用（SoftRef） | 已完成 |

## 项目结构

```
MinGC/
├── src/
│   ├── main.cpp
│   ├── gc/
│   │   ├── gcobject.h    # GCObject Header（标记/年龄/大小）
│   │   ├── root.h        # GC Roots 管理
│   │   ├── mark.h        # 三色标记算法
│   │   ├── weakref.h     # WeakRef 弱引用
│   │   ├── softref.h     # SoftRef 软引用
│   │   └── collector.cpp # Heap 单例 + Minor GC / Full GC 入口
│   └── memory/
│       ├── space.h       # bump-the-pointer 分配器
│       └── heap.h        # 堆布局 + 公开分配 API
└── docs/
    ├── architecture.md   # 架构文档（模块划分、数据结构、依赖关系）
    ├── workflow.md       # 工作流程（分配、Minor GC、Full GC 图解）
    ├── edge-cases.md     # 边界情况与 Bug 修复记录
    ├── dark-matter.md    # 暗物质专题
    ├── future.md         # 已知局限与改进方向
    └── design.md         # 设计文档（阶段规划、决策记录、术语表）
```

## 技术栈

- C++20
- 无外部依赖，纯标准库实现

## 适合人群

对垃圾回收机制感兴趣的C++开发者
