# 音频队列使用PSRAM可行性分析

## 一、数据结构分析

### 1.1 AudioStreamPacket 结构

```cpp
struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;  // Opus编码数据，60-200 bytes/packet
};
```

### 1.2 队列定义

```cpp
std::list<AudioStreamPacket> audio_send_queue_;      // 最大40个packet
std::list<AudioStreamPacket> audio_decode_queue_;   // 最大40个packet
std::list<AudioStreamPacket> audio_testing_queue_;   // 最大40个packet
```

### 1.3 内存消耗

- 每个 `AudioStreamPacket` 约 150 bytes（payload + 元数据）
- 每个 `std::list` 节点额外开销：约 16 bytes（两个指针）
- 40 packets × (150 + 16) bytes ≈ 6.6KB/队列
- 3个队列峰值：约 18-20KB

---

## 二、数据流向分析

### 2.1 audio_send_queue_ 流程

```
Opus编码数据
    ↓
AudioStreamPacket (payload = Opus数据)
    ↓
audio_send_queue_.push_back()
    ↓
协议发送（通过WebSocket/MQTT）
    ↓
不参与DMA操作
```

### 2.2 audio_decode_queue_ 流程

```
Opus编码数据
    ↓
AudioStreamPacket (payload = Opus数据)
    ↓
audio_decode_queue_.push_back()
    ↓
解码：opus_decoder_->Decode(packet.payload, pcm)
    ↓
临时创建 std::vector<int16_t> pcm (SRAM)
    ↓
codec->OutputData(pcm)
    ↓
Write() 复制数据到 I2S DMA 缓冲区
    ↓
不直接使用队列数据
```

### 2.3 关键发现

1. **队列数据不参与DMA**
   - `AudioStreamPacket.payload` 存储的是 Opus 编码数据
   - 解码后的 PCM 数据是临时创建的 `std::vector<int16_t>`
   - `codec->OutputData()` 会复制数据到 I2S DMA 缓冲区

2. **数据访问模式**
   - 写入频率：每 60ms 一次（音频帧周期）
   - 读取频率：每 60ms 一次（解码播放）
   - 不是实时关键路径，可以容忍 PSRAM 延迟

---

## 三、PSRAM可行性分析

### 3.1 ✅ 可以使用PSRAM的原因

1. **不参与DMA操作**
   - 队列只存储 Opus 编码数据
   - 解码后的 PCM 数据是临时创建的（SRAM）
   - `codec->OutputData()` 会复制数据到 DMA 缓冲区

2. **访问频率适中**
   - 每 60ms 读写一次（音频帧周期）
   - 不是实时关键路径
   - PSRAM 延迟（50-100ns/字节）可接受

3. **数据大小合适**
   - 每个队列约 6-7KB
   - PSRAM 容量充足（6MB+ 可用）

### 3.2 ⚠️ 需要注意的问题

1. **std::list 节点分配**
   - `std::list` 的节点是动态分配的
   - 需要使用自定义分配器指定 PSRAM

2. **AudioStreamPacket.payload 分配**
   - `std::vector<uint8_t> payload` 默认使用 SRAM
   - 需要修改 `AudioStreamPacket` 结构，使 `payload` 使用 PSRAM 分配器

3. **性能影响**
   - PSRAM 访问速度比 SRAM 慢（约 1/4-1/2）
   - 每次读写增加约 0.1-0.3ms 延迟（可接受）

---

## 四、实现方案

### 4.1 方案1：修改 AudioStreamPacket 结构（推荐）

**优点：**
- 完全使用 PSRAM
- 代码改动相对集中

**实现：**

**步骤1：修改 protocol.h**
```cpp
// 定义 PSRAM 分配器（如果还没有）
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
    // ... 其他方法
};

// 修改 AudioStreamPacket
struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t, PSRAMAllocator<uint8_t>> payload;  // 使用PSRAM
};
```

**步骤2：修改 application.h**
```cpp
// 定义 list 的 PSRAM 分配器
template<typename T>
using PSRAMList = std::list<T, PSRAMAllocator<T>>;

// 修改队列声明
PSRAMList<AudioStreamPacket> audio_send_queue_;
PSRAMList<AudioStreamPacket> audio_decode_queue_;
PSRAMList<AudioStreamPacket> audio_testing_queue_;
```

### 4.2 方案2：仅队列节点使用 PSRAM（简化版）

**优点：**
- 代码改动小
- 部分节省 SRAM

**缺点：**
- `payload` 仍使用 SRAM
- 节省较少（约 50%）

**实现：**
```cpp
// 仅修改队列声明
std::list<AudioStreamPacket, PSRAMAllocator<AudioStreamPacket>> audio_send_queue_;
```

---

## 五、性能影响评估

### 5.1 读取性能

**解码时的数据读取：**
- 每次读取：1个 packet，约 150 bytes
- PSRAM 读取延迟：约 7.5-15μs（150 bytes × 50-100ns）
- **总延迟增加：约 0.01-0.02ms**（可忽略）

### 5.2 写入性能

**编码后的数据写入：**
- 每 60ms 写入一次，每次约 150 bytes
- PSRAM 写入延迟：约 7.5-15μs
- **总延迟增加：约 0.01-0.02ms**（可忽略）

### 5.3 总体影响

- **队列操作延迟：** 增加约 0.01-0.02ms（可忽略）
- **音频处理延迟：** 无影响（解码后的 PCM 仍在 SRAM）
- **SRAM 节省：** 约 18-20KB（峰值）

---

## 六、推荐方案

### 6.1 推荐：方案1（完全使用PSRAM）

**理由：**
1. 最大化 SRAM 节省（18-20KB）
2. 性能影响可忽略（<0.1ms）
3. 代码改动集中（主要在 protocol.h 和 application.h）

**实现步骤：**
1. 在 `protocol.h` 中添加 PSRAM 分配器（如果还没有）
2. 修改 `AudioStreamPacket` 的 `payload` 使用 PSRAM 分配器
3. 修改 `application.h` 中的队列声明使用 PSRAM 分配器
4. 测试音频编解码和播放功能

### 6.2 注意事项

1. **初始化检查**
   ```cpp
   // 检查 PSRAM 是否可用
   if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < required_size) {
       ESP_LOGE(TAG, "PSRAM不足，无法分配音频队列");
   }
   ```

2. **错误处理**
   - PSRAM 分配失败时的降级策略
   - 可以考虑回退到 SRAM（如果可用）

3. **性能监控**
   - 监控音频编解码性能
   - 如果性能影响过大，考虑混合方案

---

## 七、结论

### 7.1 ✅ 可以使用PSRAM

**原因：**
- 队列数据不参与 DMA 操作
- 访问频率适中（每 60ms 一次）
- 解码后的 PCM 数据是临时创建的（SRAM）
- 性能影响可忽略（<0.1ms）

### 7.2 预期效果

- **SRAM 节省：** 18-20KB（峰值）
- **性能影响：** <0.1ms 延迟（可忽略）
- **实现难度：** 中等（需要修改 AudioStreamPacket 结构）

### 7.3 实施建议

1. **立即实施：** 修改 `AudioStreamPacket` 和队列使用 PSRAM
2. **测试验证：** 验证音频编解码和播放功能
3. **监控优化：** 如果性能影响过大，考虑混合方案

**总计可节省：18-20KB SRAM**（加上之前的优化，共节省约 64-66KB）

