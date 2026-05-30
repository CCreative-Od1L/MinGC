# MinGC 已知局限与改进方向

## 已知局限

### 1. 暗物质不可回收

`total_size < sizeof(FreeBlock)` 的对象晋升 Old Gen 后死亡 → 永久泄漏。当前 x64 阈值为 body_size ≥ 12 字节才可回收。

### 2. 无对象合并

相邻 FreeBlock 不会合并。反复分配/回收小对象可能导致外部碎片：即使总空闲空间充足，单个 FreeBlock 不够大时分配失败。

### 3. 自由链表仅限 Old Gen

Survivor 区使用 bump-the-pointer + reset 回收，不涉及 FreeBlock。Eden 同样只做 reset。只有 Old Gen 包含自由链表逻辑。

### 4. 无 Card Table（跨代写屏障）

Minor GC 通过全局 `mark_phase` 重新扫描 Old Gen 中的所有对象体来查找跨代引用。在大型 Old Gen 场景下成本很高。真实 GC 使用 Card Table 记录脏页避免全量扫描。

### 5. 保守式扫描产生浮动垃圾

`scan_object` 将对象体中所有对齐 word 视为潜在指针。整数与指针无法区分 → 可能错误地将非指针标记为指针，延长一些本应死亡的对象的生命周期。

### 6. 无并发/增量 GC

所有 GC 操作均为 Stop-The-World，无并发标记或增量回收。

### 7. x64 默认构建

代码支持 32-bit `header_t`，但当前以 x64 Debug 构建和测试。`sizeof(void*)` 和 `sizeof(FreeBlock)` 的变化（暗物质阈值变化）在 x86 下未验证。

### 8. OOM 处理不完整

`collect_minor_gc` 的 OOM 分支中，`collect_full_gc(true)` 仅清理 Old Gen。若 Old Gen 自由链表重建后仍然无法满足晋升需求，`continue` 跳过该对象（对象丢失）。

### 9. 缺少 Finalization / PhantomRef

没有析构回调机制。对象回收时无法执行 `finalize()`。Java 的 `PhantomReference` 和 `ReferenceQueue` 可用于实现此类需求。

---

## 改进方向

### 优先级 1：学习价值高，实现难度低

#### 1.1 相邻 FreeBlock 合并（Coalescing）

在 `sweep_old` 完成头插法建链后，遍历一次 free_list 将物理相邻的 FreeBlock 合并。

```
改进前: free_list → [4KB] → [4KB] → [8KB]   (三个不相邻块)
改进后: free_list → [16KB]                     (合并相邻块)
```

实现要点：
- sweep 按地址排序（当前头插法是逆序），可改为尾插法或添加排序步骤
- 遍历时检测 `cur->地址 + cur->size == next->地址`

#### 1.2 晋升时 Old Gen 分配 Padding

当前晋升到 Old Gen 使用 `allocate_raw(total_size())`，不保证 `≥ sizeof(FreeBlock)`。可改为：

```cpp
size_t padded = max(obj->total_size(), sizeof(FreeBlock));
new_obj = heap.old.allocate_raw(padded);
```

消除暗物质，确保所有 Old Gen 对象可回收。

**需同步**：`total_size()` 也要返回 padded 值，或 sweep 的 `cur` 前进量使用独立记录。

#### 1.3 改进 sweep_old 迭代方式

当前 sweep 依赖 `total_size()` 计算前进量。可改为在 `collect_minor_gc` 晋升时存储实际的物理分配尺寸（在对象间保持 padding 间隙），避免两层尺寸断裂。

### 优先级 2：中等价值，需要更多设计

#### 2.1 Card Table

Old Gen 划分为 512 字节卡片。写屏障标记脏卡片。Minor GC 时只扫描脏卡片对应的对象体。

```cpp
constexpr size_t CARD_SIZE = 512;
uint8_t card_table[OLD_SIZE / CARD_SIZE];

void write_barrier(void** slot, void* new_value) {
    *slot = new_value;
    size_t card = ((uint8_t*)slot - old.start) / CARD_SIZE;
    card_table[card] = 1;  // dirty
}
```

教学价值高，但需要用户手动调用 `gc_write_barrier`（C++ 无法隐式拦截普通赋值）。

#### 2.2 改进 OOM 处理

`collect_minor_gc` OOM 分支在 `continue` 之前记录失败日志，或提供 `gc_last_oom()` 查询接口。极端情况下可多次触发 Full GC 直到成功或确认失败。

#### 2.3 暴露 GC 统计

```cpp
struct GCStats {
    size_t minor_gc_count;
    size_t full_gc_count;
    size_t promoted_bytes;
    size_t freed_bytes;
};
```

便于教学演示和性能分析。

### 优先级 3：高级特性，当前阶段非必需

#### 3.1 精确式扫描（Precise Scanning / OOP Map）

取代保守式扫描，为每个类型的对象记录指针偏移表。彻底消除浮动垃圾。

#### 3.2 并发标记（Concurrent Mark）

标记阶段与 mutator 并发运行，减少 Stop-The-World 暂停时间。需要 SATB（Snapshot-At-The-Beginning）或增量更新写屏障。

#### 3.3 增量复制（Incremental Compaction）

Full GC 不只是 Mark-Sweep，而是 Mark-Compact（整理）——将存活对象向一端移动，消除碎片。需引入 Brooks-style forwarding pointer 或偏移表。

#### 3.4 Finalization / PhantomRef

```
struct Finalizer {
    void** slot;      // 指向对象的弱引用
    void (*callback)(void*);
};
```

对象被 sweep 前若未被标记 → 触发 `callback`。复杂度较高，需要独立的 finalizer 线程。

#### 3.5 线程安全

添加全局 `std::mutex`，使 `gc_malloc` / `gc_add_root` 等 API 线程安全。GC 期间所有 mutator 线程需在安全点暂停（Safepoint）。

#### 3.6 大型对象区（Large Object Space）

超过阈值的对象直接分配到独立空间（避免复制成本），使用 Mark-Sweep 而非 Copy。

---

## 建议路线图

```
现在
├─ 暗物质消除：晋升 padding + free_list 合并
├─ Card Table：写屏障 + 脏卡片扫描
├─ GC 统计：计数 minor/full GC 次数和释放字节
└─ 精确式扫描：OOP Map 替代保守扫描
    ↓
未来
├─ 并发标记：SATB 屏障 + 并发线程
├─ 增量整理：Mark-Compact 替代 Mark-Sweep
└─ Finalization：弱引用 + 回调队列
```
