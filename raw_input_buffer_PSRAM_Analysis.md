# raw_input_buffer_ 使用PSRAM可行性分析

## 一、raw_input_buffer_ 使用场景分析

### 1.1 数据流向

```
ReadAudio() 音频输入
    ↓
raw_input_buffer_.insert() - 存储原始音频数据
    ↓
PerformDoaOnceAfterWakeWord() - DOA处理时
    ↓
复制到 interleaved (临时vector，SRAM)
    ↓
分离到 chL, chR (static vector，SRAM)
    ↓
esp_doa_process(chL.data(), chR.data()) - DOA处理
```

### 1.2 关键代码位置

**存储数据：** `main/application.cc:1007`
```cpp
raw_input_buffer_.insert(raw_input_buffer_.end(), data.begin(), data.end());
```

**使用数据：** `main/application.cc:1607`
```cpp
interleaved.insert(interleaved.end(), raw_input_buffer_.end() - frame * 2 - i * 2, raw_input_buffer_.end() - i * 2);
```

**DOA处理：** `main/application.cc:1617`
```cpp
float angle = esp_doa_process(doa_handle_, chL.data(), chR.data());
```

---

## 二、PSRAM可行性分析

### 2.1 ✅ 可以使用PSRAM的原因

1. **不参与DMA操作**
   - `raw_input_buffer_` 只是数据存储缓冲区
   - 不直接用于I2S DMA或其他硬件DMA
   - 数据会被复制到SRAM中的临时缓冲区（`interleaved`, `chL`, `chR`）

2. **访问频率较低**
   - 只在DOA处理时读取（唤醒词后执行一次）
   - 不是实时处理路径，可以容忍PSRAM的延迟
   - 写入频率：每60ms一次（音频帧周期）

3. **数据大小适中**
   - 当前：8000 samples × 2 bytes = 16KB
   - PSRAM容量充足（日志显示有6MB+可用）

4. **std::vector支持自定义分配器**
   - 可以使用自定义分配器指定PSRAM分配

### 2.2 ⚠️ 需要注意的问题

1. **性能影响**
   - PSRAM访问速度比SRAM慢（约1/4-1/2）
   - 每次DOA处理需要从PSRAM读取数据
   - 但DOA处理不是实时路径，影响可接受

2. **std::vector默认使用SRAM**
   - `std::vector<int16_t>` 默认从SRAM分配
   - 需要使用自定义分配器或手动管理PSRAM内存

3. **数据复制开销**
   - 从PSRAM读取数据到SRAM临时缓冲区
   - 需要额外的内存复制操作
   - 但这是必要的（DOA处理需要SRAM数据）

---

## 三、实现方案

### 3.1 方案1：使用自定义分配器（推荐）

**优点：**
- 保持std::vector接口
- 代码改动小
- 自动管理内存

**实现：**
```cpp
// 定义PSRAM分配器
template<typename T>
class PSRAMAllocator {
public:
    using value_type = T;
    
    T* allocate(size_t n) {
        return (T*)heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    }
    
    void deallocate(T* p, size_t n) {
        heap_caps_free(p);
    }
    
    template<typename U>
    bool operator==(const PSRAMAllocator<U>&) const { return true; }
    
    template<typename U>
    bool operator!=(const PSRAMAllocator<U>&) const { return false; }
};

// 在application.h中
std::vector<int16_t, PSRAMAllocator<int16_t>> raw_input_buffer_;
```

### 3.2 方案2：手动管理PSRAM内存

**优点：**
- 完全控制内存分配
- 可以精确控制大小

**实现：**
```cpp
// 在application.h中
int16_t* raw_input_buffer_ = nullptr;  // 使用原始指针
size_t raw_input_buffer_size_ = 0;
size_t raw_input_buffer_capacity_ = 0;

// 在初始化时分配
raw_input_buffer_ = (int16_t*)heap_caps_malloc(
    raw_input_buffer_size_ * sizeof(int16_t), 
    MALLOC_CAP_SPIRAM
);
```

### 3.3 方案3：混合方案（部分PSRAM）

**优点：**
- 平衡性能和内存
- 保留部分SRAM用于快速访问

**实现：**
- 使用固定大小数组（SRAM）作为快速缓冲区
- 使用PSRAM作为长期存储
- 定期将SRAM数据刷新到PSRAM

---

## 四、性能影响评估

### 4.1 读取性能

**DOA处理时的数据读取：**
- 每次DOA处理读取：2048 samples × 2 channels × 2 bytes = 8KB
- PSRAM读取延迟：约50-100ns/字节（vs SRAM 10-20ns）
- **总延迟增加：约0.4-0.8ms**（可接受）

### 4.2 写入性能

**音频数据写入：**
- 每60ms写入一次，每次约960-1920 samples = 1.9-3.8KB
- PSRAM写入延迟：约50-100ns/字节
- **总延迟增加：约0.1-0.4ms**（可接受）

### 4.3 总体影响

- **DOA处理延迟：** 增加约0.4-0.8ms（可忽略）
- **音频输入延迟：** 增加约0.1-0.4ms（可忽略）
- **SRAM节省：** 16KB（当前大小）

---

## 五、推荐方案

### 5.1 推荐：使用自定义分配器（方案1）

**理由：**
1. 代码改动最小
2. 保持std::vector的便利性
3. 自动管理内存生命周期
4. 性能影响可接受

**实现步骤：**
1. 定义PSRAMAllocator
2. 修改raw_input_buffer_类型
3. 测试DOA处理性能

### 5.2 注意事项

1. **初始化检查**
   ```cpp
   if (raw_input_buffer_.empty() && raw_input_buffer_.capacity() == 0) {
       // 检查PSRAM是否可用
       if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < raw_input_buffer_size_ * sizeof(int16_t)) {
           ESP_LOGE(TAG, "PSRAM不足，无法分配raw_input_buffer_");
       }
   }
   ```

2. **错误处理**
   - PSRAM分配失败时的降级策略
   - 可以考虑回退到SRAM（如果可用）

3. **性能监控**
   - 监控DOA处理时间
   - 如果性能影响过大，考虑混合方案

---

## 六、结论

### 6.1 ✅ 可以使用PSRAM

**原因：**
- `raw_input_buffer_` 不参与DMA操作
- 访问频率较低（只在DOA处理时读取）
- 数据会被复制到SRAM临时缓冲区
- 性能影响可接受（<1ms延迟）

### 6.2 预期效果

- **SRAM节省：** 16KB（当前大小）
- **性能影响：** <1ms延迟（可忽略）
- **实现难度：** 低（使用自定义分配器）

### 6.3 实施建议

1. **立即实施：** 使用自定义分配器将`raw_input_buffer_`移到PSRAM
2. **测试验证：** 验证DOA处理性能和正确性
3. **监控优化：** 如果性能影响过大，考虑混合方案

**总计可节省：16KB SRAM**（加上之前优化的24KB，共节省40KB）

