# AudioPlayer 全面工程审计与加固计划

## 项目概述

- **项目类型**: Qt6 C++ 桌面音频播放器
- **代码规模**: 78个源码文件，24,513行代码
- **后端支持**: WASAPI (Windows), ASIO (Windows), ALSA (Linux), FFmpeg (跨平台)
- **测试框架**: Qt Test (已有7个测试文件)
- **文档结构**: `docs/bug/` (状态跟踪), `docs/dev/` (开发指南)

## 依赖关系

```
Phase 1 (代码库全景分析)
    ↓
    ├─→ Phase 2 (测试全覆盖)
    ├─→ Phase 3 (代码质量加固)
    ├─→ Phase 4 (文档生成)
    ├─→ Phase 5 (性能与架构优化)
    └─→ Phase 6 (跨平台兼容性分析)
```

**关键依赖**:
- Phase 1 是所有后续工作的基础，必须首先完成
- Phase 2-6 可以并行执行，但都需要引用 Phase 1 的分析结果
- Phase 3 (代码质量) 和 Phase 5 (性能分析) 有部分重叠，可以合并执行

## 并行执行策略

### 第一阶段：Phase 1 (串行执行)
**执行者**: 单一窗口
**输出**: `docs/audit/phase1-*.md`

### 第二阶段：Phase 2-6 (并行执行)
**窗口分配**:

| 窗口 | 任务 | 输出目录 |
|------|------|----------|
| A | Phase 2: 测试全覆盖 | `tests/` |
| B | Phase 3+5: 代码质量 + 性能分析 | `docs/audit/phase3-*.md`, `docs/audit/phase5-*.md` |
| C | Phase 4: 文档生成 | `docs/audit/phase4-*.md` |
| D | Phase 6: 跨平台分析 | `docs/audit/phase6-*.md` |

## Phase 1: 代码库全景分析

### 任务清单

1. **类职责分析** (`phase1-class-responsibilities.md`)
   - 读取所有 .h/.cpp 文件
   - 列出每个类的职责、依赖关系、线程模型
   - 识别核心类和辅助类

2. **类依赖图** (`phase1-class-dependencies.md`)
   - 绘制完整的类依赖关系图（文字版）
   - 标注依赖方向和类型

3. **播放流程调用链** (`phase1-playback-callchain.md`)
   - 从用户点击播放到音频输出的完整调用链
   - 包含跨线程调用和信号槽连接

4. **WASAPI 状态机** (`phase1-wasapi-statemachine.md`)
   - WASAPI 切换/错误恢复的完整状态机
   - 状态转换条件和处理逻辑

5. **跨线程信号槽分析** (`phase1-cross-thread-signals.md`)
   - 识别所有跨线程信号槽调用
   - 标注是否安全（Qt::ConnectionType）

6. **内存管理分析** (`phase1-memory-management.md`)
   - 列出所有 raw pointer 使用
   - 评估内存泄漏风险
   - 识别智能指针使用情况

## Phase 2: 测试全覆盖

### 任务清单

1. **AudioDecoder 测试** (`test_audiodecoder.cpp`)
   - 所有格式的解码测试
   - 边界条件测试（空文件、损坏文件、超大文件）
   - 错误处理测试

2. **AudioBuffer 测试** (`test_audiobuffer.cpp`)
   - 边界条件测试
   - 并发读写测试
   - 内存溢出测试

3. **WindowsWasapiAudioPlayer 测试** (`test_wasapiaudioplayer.cpp`)
   - 状态转换测试
   - 错误恢复测试
   - 设备切换测试

4. **PlaylistManager 测试** (`test_playlistmanager.cpp`)
   - 增删改查测试
   - 边界条件测试（空列表、单曲目、超大列表）
   - **状态**: ❌ 功能未实现，跳过测试

5. **音量控制测试** (`test_volumecontrol.cpp`)
   - 音量调节测试
   - 静音测试
   - 边界值测试
   - **状态**: ✅ 已实现，可编写测试

6. **均衡器测试** (`test_equalizer.cpp`)
   - 频段调节测试
   - 预设切换测试
   - **状态**: ❌ 功能未实现，跳过测试

7. **无缝播放测试** (`test_gaplessplayback.cpp`)
   - 曲目切换测试
   - 缓冲区管理测试
   - **状态**: ❌ 功能未实现，跳过测试

8. **集成测试** (`test_integration.cpp`)
   - 完整播放流程测试
   - 多后端切换测试

9. **WASAPI 错误恢复模拟测试** (`test_wasapi_recovery.cpp`)
   - 设备断开模拟
   - 缓冲区溢出模拟

## Phase 3: 代码质量加固

### 任务清单

1. **内存泄漏审查**
   - 检查所有 new/delete 配对
   - 检查 Qt 对象父子关系
   - 检查循环引用

2. **线程竞争审查**
   - 检查共享数据访问
   - 检查锁的使用
   - 检查死锁风险

3. **错误路径审查**
   - 检查未处理的错误
   - 检查异常安全
   - 检查资源清理

4. **魔法数字审查**
   - 识别所有硬编码数字
   - 提取为常量

5. **重复代码审查**
   - 识别重复逻辑
   - 提取为公共函数

6. **类型转换规范化**
   - 将 C 风格转换改为 C++ 风格
   - 使用 static_cast, dynamic_cast, reinterpret_cast

7. **错误处理统一**
   - 统一错误处理策略
   - 添加错误日志

## Phase 4: 文档生成

### 任务清单

1. **Doxygen 文档注释**
   - 为每个类生成详细注释
   - 为每个公共方法生成注释
   - 包含参数说明、返回值、异常

2. **架构设计文档** (`phase4-architecture.md`)
   - 整体架构概述
   - 模块划分和职责
   - 数据流图

3. **API 参考手册** (`phase4-api-reference.md`)
   - 核心类 API 文档
   - 使用示例

4. **开发者入门指南** (`phase4-developer-guide.md`)
   - 环境搭建
   - 构建步骤
   - 调试技巧

5. **故障排查手册** (`phase4-troubleshooting.md`)
   - WASAPI 问题排查
   - ASIO 问题排查
   - ALSA 问题排查
   - 常见错误和解决方案

## Phase 5: 性能与架构优化

### 任务清单

1. **性能瓶颈分析** (`phase5-performance-analysis.md`)
   - 播放管线性能分析
   - CPU 使用率分析
   - 内存使用分析

2. **优化方案** (`phase5-optimization-proposals.md`)
   - 方案 A: 缓冲区优化
   - 方案 B: 线程模型优化
   - 方案 C: 解码器优化
   - 每种方案的利弊分析

3. **Buffer 管理分析** (`phase5-buffer-management.md`)
   - 当前策略分析
   - 优化建议

4. **内存占用分析** (`phase5-memory-usage.md`)
   - 内存分配模式
   - 内存碎片分析
   - 优化建议

5. **启动时间优化** (`phase5-startup-optimization.md`)
   - 启动流程分析
   - 优化空间评估

## Phase 6: 跨平台兼容性分析

### 任务清单

1. **Windows 特有代码分析** (`phase6-windows-specific.md`)
   - 列出所有 Windows 特有代码
   - 评估移植难度

2. **ALSA 后端完成度评估** (`phase6-alsa-completeness.md`)
   - 功能完整性检查
   - 已知问题和限制

3. **平台抽象层设计** (`phase6-platform-abstraction.md`)
   - 为每个平台差异点设计抽象层
   - 接口定义
   - 实现策略

## 输出文件结构

```
docs/audit/
├── audit-plan.md                    # 本文档
├── phase1-class-responsibilities.md # 类职责分析
├── phase1-class-dependencies.md     # 类依赖图
├── phase1-playback-callchain.md     # 播放流程调用链
├── phase1-wasapi-statemachine.md    # WASAPI 状态机
├── phase1-cross-thread-signals.md   # 跨线程信号槽分析
├── phase1-memory-management.md      # 内存管理分析
├── phase3-code-quality.md           # 代码质量审查报告
├── phase4-architecture.md           # 架构设计文档
├── phase4-api-reference.md          # API 参考手册
├── phase4-developer-guide.md        # 开发者入门指南
├── phase4-troubleshooting.md        # 故障排查手册
├── phase5-performance-analysis.md   # 性能分析报告
├── phase5-optimization-proposals.md # 优化方案
├── phase5-buffer-management.md      # Buffer 管理分析
├── phase5-memory-usage.md           # 内存占用分析
├── phase5-startup-optimization.md   # 启动时间优化
├── phase6-windows-specific.md       # Windows 特有代码分析
├── phase6-alsa-completeness.md      # ALSA 后端完成度
└── phase6-platform-abstraction.md   # 平台抽象层设计

tests/
├── test_audiodecoder.cpp            # AudioDecoder 测试
├── test_audiobuffer.cpp             # AudioBuffer 测试
├── test_wasapiaudioplayer.cpp       # WASAPI 测试
├── test_playlistmanager.cpp         # PlaylistManager 测试
├── test_volumecontrol.cpp           # 音量控制测试
├── test_equalizer.cpp               # 均衡器测试
├── test_gaplessplayback.cpp         # 无缝播放测试
├── test_integration.cpp             # 集成测试
└── test_wasapi_recovery.cpp         # WASAPI 错误恢复测试
```

## 验证标准

### Phase 1 完成标准
- [ ] 所有类的职责、依赖、线程模型已记录
- [ ] 类依赖图完整且准确
- [ ] 播放流程调用链覆盖所有路径
- [ ] WASAPI 状态机包含所有状态和转换
- [ ] 跨线程信号槽调用已标注安全性
- [ ] 内存管理风险已识别

### Phase 2 完成标准
- [ ] 每个已实现的核心类都有对应测试文件
- [ ] 测试覆盖所有已实现的公共方法
- [ ] 边界条件测试完整
- [ ] 集成测试覆盖主要播放流程
- [ ] 所有测试可以通过
- [ ] 未实现的功能（PlaylistManager、均衡器、无缝播放）跳过测试，在文档中标注

### Phase 3 完成标准
- [ ] 所有内存泄漏已修复
- [ ] 所有线程竞争已解决
- [ ] 所有错误路径已处理
- [ ] 魔法数字已提取为常量
- [ ] 重复代码已重构
- [ ] 类型转换已规范化

### Phase 4 完成标准
- [ ] 所有类有 Doxygen 注释
- [ ] 架构设计文档完整
- [ ] API 参考手册可用
- [ ] 开发者入门指南可操作
- [ ] 故障排查手册覆盖已知问题

### Phase 5 完成标准
- [ ] 性能瓶颈已识别
- [ ] 优化方案已提出并分析利弊
- [ ] Buffer 管理策略已分析
- [ ] 内存占用模式已分析
- [ ] 启动时间优化空间已评估

### Phase 6 完成标准
- [ ] Windows 特有代码已列出
- [ ] ALSA 后端完成度已评估
- [ ] 平台抽象层设计已完成

## 执行注意事项

1. **遵循项目工作流程**
   - 每次修改前先说明计划
   - 保持最小化改动
   - 遵循现有代码风格

2. **文档规范**
   - 使用中文编写分析文档
   - 包含具体的代码引用（文件名:行号）
   - 提供具体的改进建议

3. **测试规范**
   - 使用 Qt Test 框架
   - 遵循现有测试风格
   - 测试命名清晰

4. **验证要求**
   - 每个 Phase 完成后运行相关测试
   - 确保不破坏现有功能
   - 记录验证结果
