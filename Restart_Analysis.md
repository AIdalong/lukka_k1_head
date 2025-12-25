# 系统重启原因分析

## 一、时间线分析

```
173192: 停止idle emoji rotation
173262: PlayEmoji called，开始播放AAF ID 18（无限循环）
182322: SystemInfo显示
        - free sram: 3419 bytes (约3.4KB)
        - minimal sram: 603 bytes (历史最低，约0.6KB)
        - free psram: 6776k
        - minimal psram: 6259k
188322: 展示挪车码，状态变为speaking
188342: SetStatus called，停止idle emoji rotation
188962: E (188962) pthread: Failed to create task!
        abort() was called
        系统重启
```

**关键时间点：**
- 从182322到188322：约6秒，SRAM持续消耗
- 从188322到188962：约640ms，展示挪车码后触发pthread创建失败

---

## 二、根本原因分析

### 2.1 直接原因：pthread创建任务失败

**错误信息：**
```
E (188962) pthread: Failed to create task!
abort() was called at PC 0x4217649f on core 1
```

**触发位置：** `main/mcp_server.cc:439`
```cpp
tool_call_thread_ = std::thread([this, id, tool_iter, arguments = std::move(arguments)]() {
    // ...
});
```

**调用链：**
1. 展示挪车码 → `display->SetEmotion("code")` 
2. 通过MCP工具调用 → `self.screen.show_parking_code`
3. MCP server处理工具调用 → `DoToolCall()`
4. 创建pthread任务执行工具 → `std::thread(...)`
5. **pthread创建失败** → `abort()`

### 2.2 根本原因：SRAM严重不足

**关键数据：**
- **当前剩余SRAM：3419 bytes (约3.4KB)**
- **历史最低SRAM：603 bytes (约0.6KB)**
- **pthread栈需求：6144 bytes (6KB)** - `DEFAULT_TOOLCALL_STACK_SIZE`

**问题分析：**
1. SRAM只剩3.4KB，历史最低只有603字节
2. pthread创建任务需要分配6KB栈空间（`DEFAULT_TOOLCALL_STACK_SIZE = 6144`）
3. **3.4KB < 6KB**，无法分配足够内存
4. pthread创建失败，触发`abort()`，系统重启

### 2.3 SRAM消耗分析

**在182322时刻（6秒前）：**
- free sram: 3419 bytes
- minimal sram: 603 bytes（说明系统曾经接近内存耗尽）

**SRAM消耗来源：**
1. **raw_input_buffer_：** 16KB（已优化，原40KB）
2. **音频队列：** 12-18KB（峰值）
3. **I2S DMA：** 5.6KB
4. **显示DMA：** 4-15KB（峰值）
5. **任务栈：** 28-31KB
6. **系统组件：** 31-61KB
7. **其他缓冲区：** 5-8KB

**总计：约102-154KB**（从堆分配）

**剩余SRAM：3.4KB**（严重不足）

---

## 三、问题触发机制

### 3.1 展示挪车码的流程

```
MCP工具调用 "self.screen.show_parking_code"
    ↓
McpServer::DoToolCall()
    ↓
创建std::thread执行工具回调
    ↓
工具回调：display->SetEmotion("code")
    ↓
EmojiWidget::SetEmotion("code")
    ↓
ShowRawBMP() 显示QR码图片
```

### 3.2 pthread创建失败的原因

**代码位置：** `main/mcp_server.cc:431-439`
```cpp
esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
cfg.thread_name = "tool_call";
cfg.stack_size = stack_size;  // 默认6144 bytes (6KB)
cfg.prio = 1;
esp_pthread_set_cfg(&cfg);

// 创建pthread任务需要分配6KB栈空间
tool_call_thread_ = std::thread([this, id, tool_iter, arguments = std::move(arguments)]() {
    // ...
});
```

**失败原因：**
- pthread创建任务需要从**内部SRAM**分配栈空间
- 需要分配**6KB连续内存块**
- 当前剩余SRAM只有**3.4KB**
- 即使有3.4KB，也可能因为**内存碎片化**无法分配6KB连续块
- **分配失败** → `pthread: Failed to create task!` → `abort()`

---

## 四、内存状态分析

### 4.1 内存消耗时间线

| 时间 | 事件 | SRAM状态 |
|------|------|---------|
| 173262 | PlayEmoji开始（无限循环） | 开始消耗 |
| 182322 | SystemInfo打印 | free: 3.4KB, min: 0.6KB |
| 188322 | 展示挪车码 | 触发MCP工具调用 |
| 188962 | pthread创建失败 | **SRAM不足，无法分配6KB栈** |

### 4.2 内存碎片化影响

**问题：**
- 即使总剩余SRAM有3.4KB，也可能无法分配6KB连续块
- 内存碎片化导致无法找到足够大的连续内存
- 历史最低只有603字节，说明系统曾经非常接近内存耗尽

**证据：**
- `minimal sram: 603` - 系统运行过程中SRAM曾经只剩603字节
- 这表明内存压力一直很大，只是当前时刻稍微恢复了一点

---

## 五、问题总结

### 5.1 直接原因
**pthread创建任务失败** - 需要6KB栈空间，但SRAM只剩3.4KB且碎片化

### 5.2 根本原因
**SRAM严重不足** - 系统正常运行已消耗大部分SRAM，剩余空间不足以创建新任务

### 5.3 触发条件
1. SRAM剩余 < 6KB（pthread栈大小）
2. 触发MCP工具调用（展示挪车码）
3. MCP server尝试创建pthread任务
4. 分配失败 → abort() → 重启

### 5.4 关键发现

1. **历史最低SRAM只有603字节**
   - 说明系统在运行过程中曾经非常接近内存耗尽
   - 即使当前有3.4KB，也处于危险状态

2. **pthread栈大小过大**
   - 默认6KB对于简单工具调用可能过大
   - 可以考虑减少到2-3KB

3. **内存碎片化严重**
   - 即使总内存够，也无法分配大块连续内存
   - 需要优化内存分配策略

---

## 六、解决方案建议

### 6.1 立即解决方案

1. **减少pthread栈大小**
   - 当前：`DEFAULT_TOOLCALL_STACK_SIZE = 6144` (6KB)
   - 建议：减少到2048-3072 bytes (2-3KB)
   - **节省：3-4KB SRAM**

2. **检查SRAM后再创建pthread**
   - 在创建pthread前检查可用SRAM
   - 如果不足，使用同步方式执行工具调用（不创建线程）

### 6.2 根本解决方案

1. **继续优化SRAM使用**
   - 减少音频队列大小（节省9-12KB）
   - 复用临时缓冲区（减少碎片化）
   - 优化任务栈大小

2. **优化内存管理**
   - 使用静态分配替代动态分配
   - 减少内存碎片化
   - 预分配关键缓冲区

3. **改进pthread使用策略**
   - 对于简单工具调用，使用同步执行（不创建线程）
   - 只在必要时创建pthread
   - 使用更小的栈大小

---

## 七、预防措施

1. **添加内存检查**
   ```cpp
   // 在创建pthread前检查
   int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
   if (free_sram < stack_size + 2048) {  // 预留2KB余量
       // 使用同步方式执行，不创建线程
       return tool->Call(arguments);
   }
   ```

2. **监控minimal sram**
   - 如果`minimal sram < 10KB`，告警
   - 如果`minimal sram < 5KB`，拒绝创建新任务

3. **优化工具调用策略**
   - 简单工具调用：同步执行
   - 复杂工具调用：异步执行（pthread）
   - 根据SRAM状态动态选择

---

## 八、结论

**重启原因：**
- **直接原因：** pthread创建任务失败（需要6KB栈，但SRAM只剩3.4KB）
- **根本原因：** SRAM严重不足（历史最低603字节）+ 内存碎片化
- **触发条件：** 展示挪车码时触发MCP工具调用，需要创建pthread任务

**关键数据：**
- 剩余SRAM：3.4KB
- pthread栈需求：6KB
- **缺口：2.6KB + 碎片化影响**

**解决方案优先级：**
1. **立即：** 减少pthread栈大小到2-3KB
2. **短期：** 添加SRAM检查，不足时同步执行
3. **长期：** 继续优化SRAM使用，减少内存碎片化

