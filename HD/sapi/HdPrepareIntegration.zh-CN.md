# 交给 SumatraPDF 的接入说明

本次提供两条路径。先接标准 SAPI 缓冲设置，保留现有 A/B/C 队列；需要更早、独立于播放队列准备时，再启用公开预合成服务。当前 Sumatra 若未调用本头文件中的函数，不会自动启用第二条路径。

## 1. 标准队列：先扩大该 ISpVoice 的设备缓冲

```cpp
#include "HdPrepare.h"

// 创建并选定 ISpVoice/输出设备后，第一次 Speak 之前调用。
HRESULT buffered = HdConfigureSapiQueue(voice, 5000);
// 不支持或失败时保留普通 SAPI，不修改全局设备设置。
```

之后继续使用客户端已有的逻辑：A 用异步 purge + 非 XML，B 用异步非 XML、不 purge；A 的 END 到达后接管已经入队的 B，排入 C，不重复 Speak。

异步 Speak 是排队，不是并行合成。实测默认缓冲为 500 毫秒；A 的引擎调用被 `ISpTTSEngineSite::Write` 反压，B 到 A 接近播完时才被调用。引擎最后一次 Write 返回后约 1 毫秒内返回，没有另等设备把 A 播完。5 秒缓冲允许 A 提早写完，让串行的 B 合成在 A 剩余播放时间内完成。

缓冲增加了设备中提前排入的音频。停止／跳转／变速／换声必须 purge；不要等旧音频自然播完。第一段等待、计算量超过可提前播放时间、跨页提取/OCR 尚未完成的情况，不能靠这个设置保证无缝。

## 2. 可选预合成：公开注册 COM 类，可直接 CoCreateInstance

头文件为同目录的 `HdPrepare.h`。生产 CLSID 是 `{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8E}`，32 位、64 位均注册，`ThreadingModel=Both`。不是在私有引擎对象上新增接口，不需要获取 `ISpTTSEngine`，也没有命名管道端点发现问题。

```cpp
Microsoft::WRL::ComPtr<IHdSpeechPreparer> prepare;
HRESULT connected = HdConnectPreparer(prepare.GetAddressOf());
if (FAILED(connected)) {
    // REGDB_E_CLASSNOTREG、旧版本、接口不支持等：回退现有普通 SAPI。
}
HD_PREPARE_CAPS caps;
prepare->GetCapabilities(&caps); // 检查 version == 1 和 flags。
HD_PREPARE_LIMITS limits;
limits.maxCacheBytes = 64ull * 1024 * 1024;
limits.maxResidentBytes = 64ull * 1024 * 1024;
prepare->SetLimits(&limits); // 第一次 Prepare 之前，可选。
prepare->AdvanceGeneration(generation); // 初值从 1 开始，必须严格递增。

// 从 voice->GetVoice()->GetId() 取得完整 token ID。
// actualNextText 必须是发音词典改写后的实际文本，保留它供高亮映射。
ULONGLONG ticket = 0;
HRESULT submitted = prepare->Prepare(actualNextText.c_str(), tokenId, rate,
                                    generation, &ticket);
// S_OK：已排入／正在处理；S_FALSE：现成缓存命中。两者都不是错误。
```

一旦下一段文本可用就能 Prepare，不必等 A 开始发声。服务在独立 COM 线程复用自己的 ISpVoice、本地 HD 引擎及 synthesizer，输出到带词／句／口型元数据的 WAV 缓存。准备过程不输出到声卡、不向客户端的播放 voice 发送任何播放事件。

在拥有 SAPI 的线程定时查询，禁止在 UI 上轮询阻塞等待：

```cpp
HD_PREPARE_STATUS status;
HRESULT queried = prepare->GetStatus(ticket, &status);
if (SUCCEEDED(queried) && status.state == HD_READY &&
    status.generation == generation && !nextAlreadyQueued) {
    Microsoft::WRL::ComPtr<IHdPreparedAudio> audio;
    if (SUCCEEDED(prepare->Acquire(ticket, audio.GetAddressOf()))) {
        ULONG streamB = 0;
        HRESULT queued = HdQueuePrepared(voice, audio.Get(),
            actualNextText.c_str(), generation, SPF_ASYNC, &streamB);
        if (SUCCEEDED(queued)) {
            // 保存 ticket、streamB、generation、actualNextText、audio，直到 END 或 purge 确认。
            // 本段现在已经通过 SpeakStream 入队，不得再 Speak 文本一次。
        }
    }
}
```

`HdQueuePrepared` 会验证实际文本、当前 voice token、当前 rate、generation，任何一项不符便拒绝缓存；它还把播放音量同步给缓存流。音频流播放不调用 TTS 引擎的 GetVolume，不能假定只调用 `ISpVoice::SetVolume` 就能调节预合成 PCM。音量改变时同时对当前及已排队的 `IHdPreparedAudio` 调用 `SetPlaybackVolume`。语速已经烘焙进音频；变速或换声需要新代次、purge、重新准备，不能继续播放旧缓存。

准备未完成、失败、资源不足或服务不存在时，可以只调用一次普通 Speak 回退。必须先把该段标记为已经回退／已入队并取消对应 ticket，防止稍后 READY 回调又把它入队一次。成功入队后必须同时持有 ticket 和 audio lease，直到本段 END 或 purge 确认完成，不能立即 Cancel：`Cancel(ticket)` 也会使已经 Acquire 的流 Read 失败；`AdvanceGeneration` 取消该服务旧代次的全部准备，不替客户端 purge 声卡队列。

每次 Acquire 得到独立的读流，缓存命中后重新 Acquire，不复用已经读到结尾的流。返回的 ISpStream 自身持有文件／内存配额租约，SAPI 排队期间即使局部指针释放也不会提前删文件。建议仍把 audio 保存在段落状态中，便于同步音量。服务对象保持整个朗读会话；关闭前先停止播放。

## 3. 代次、purge 与流号重用

必须在一个串行 SAPI 工作线程上调用 Speak、Prepare 调度、purge 和 GetEvents；UI 只投递命令。UI 消息携带工作线程收到事件时的 generation，不能到 UI 执行时才读取当前 generation。

实测 purge 后流号会重新从 1 开始，且旧流的 WORD/END 收尾通知可能在 purge 时集中送出。仅用 `ulStreamNum` 不能区分前后两代。

停止／跳转／重启／变速／换声建议：

1. UI 立即切换 generation、停止接受旧代次的高亮消息。
2. 在 SAPI 工作线程调用 `HdPurgeAndDrain(voice)`。它同步等待中止确认并清掉旧通知，不是等待旧音频自然播放完毕；不要在 UI 线程调用。
3. `prepare->AdvanceGeneration(newGeneration)`，释放旧段落状态，设置新 voice/rate。
4. 提交新 A，保存返回的流号。在新代次的 `SPEI_START_INPUT_STREAM` 到达前不接受 WORD/END 更新。
5. 如果中止确认超时或失败，不复用无法确认已清空的 voice；隔离旧实例后回退／重建。

普通后端取消时会发送停止信号并排空旧请求到完成帧，再复用健康的模型进程。超时或损坏连接会销毁重建，不能强行复用。后台准备任务不直接写播放 site，因此旧任务即使正在收尾也不能把 PCM 或事件写进新流。

## 4. 限额和隔离

服务实例之间、voice、rate、原文和 generation 均隔离。默认最多 8 个任务，单段最多 32,768 个 UTF-16 字符，总计 65,536 字符。缓存文件总预算 64 MiB、单项 16 MiB；驻留缓存／读流租约另有 64 MiB 的保守预留预算。

PCM 使用临时文件，任务元数据、文本和打开的读流均计入限额；已取得的流即使被缓存索引淘汰，仍会占用配额到最后一个引用释放。超限返回 `HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)`，客户端可以普通 SAPI 回退。系统 HD 模型及 SAPI／驱动本身的内存不属于缓存限额，不承诺限制整个进程工作集。文件写出期间也存在暂存文件，不能把逻辑缓存预算当作整个磁盘的硬配额。

## 5. 事件与当前客户端注意事项

- 每段 WORD 的位置、长度对应其实际改写文本的 UTF-16 索引，从本段 0 开始，不跨流累计。
- WAV 缓存不嵌入 START/END_INPUT_STREAM；播放时由 SAPI 分配新流号并在设备时序中产生一次 START/END，不能按准备完成时刻触发。
- 当前段 END 后只接管已经入队的下一段，不重新 Speak，不 purge。
- 未提取／未 OCR 的文本不能预合成；智能双语切换继续沿用客户端当前不提前排队的策略。

## 6. 复测与证据

`tools/QueueProbe.cpp` 实际调用同一个 ISpVoice 的 A/B/C 队列，只捕获本测试进程的 WASAPI 回环，不录麦克风或其他应用。`tools/analyze_queue.py` 把实际回环与引擎提交的 PCM 做归一化波形相关对齐，包含原 PCM 的首尾静音；不是用 END 和 START 通知时间相减冒充采样间隔。

| 路径 | A→B 最后／最初 PCM 采样间隔 | B→C |
| --- | ---: | ---: |
| 普通队列，500ms 缓冲 | 770ms | 1180ms |
| 普通队列，5000ms 缓冲 | 0 个采样 | 0 个采样 |
| 显式 Prepare + SpeakStream，500ms 缓冲 | 0 个采样 | 0 个采样 |

最终 SHD3 版本按 A 的 START 通知立即提交 B 复测，普通 500ms 缓冲为
690／1230ms；x64 预合成、x86 普通 5000ms 缓冲仍为 0／0 个采样。
x86 的 rate=4 预合成测试为 0／约 -0.083ms，后一数值属于参考波形重采样的
2 个采样对齐误差，不表示音频实际重叠。两端相关性均通过校验。

控制场景记录的调用／中止确认耗时：停止约 78ms、跳转约 335ms、变速约
101ms、换声约 166ms，均不是等待旧段自然播完；日志中均出现旧 native 请求
排空后的可复用标记。早于第一段 START 的跳转也通过测试。

这些是本机该样例测量，不保证所有文本、负载和设备均无缝。变速后可能有输出重采样造成的 1～2 个参考采样的对齐误差。设备自然停顿保留，额外合成空档与自然停顿分开计算。

原始文件：`research/queue-baseline/`、`research/queue-buffer5000/`、`research/queue-prepared/`、`research/queue-epoch-*/`。CSV 记录 QPC、进程、线程、引擎调用、请求编号、合成开始／首 PCM／完成、每次 Write 调用及返回、客户端流事件。`tools/analyze_queue_trace.py` 生成时间汇总。进程回环在设备闲置时可能暂停位置时钟，因此不把首包时间戳简单外推成物理停止延迟；正常连续段的波形对齐有两端相关性及漂移校验。

最终样本位于 `research/queue-release-*/`，`sample-gaps.json` 为采样间隔，
`timing-summary.json` 为引擎及通知时间。缓存路径同时保存实际准备的 WAV
作为波形参考。捕获发生丢帧／时钟漂移时分析器会拒绝精确结论；不采用这种
失败捕获的数字。扩大捕获缓冲后已复测通过。

完整复测可运行 `tools/Test-HdQueue.ps1 -Architecture x64` 或 `-Architecture x86`。
该脚本会实际播放合成的测试语音，自动建立并清理独立的测试 COM 注册。
可用 `-Modes baseline,buffered,prepare` 限定场景；Python 分析需要 NumPy，
可通过 `-Python` 指定已安装 NumPy 的解释器。标准队列场景在 A 的 START 通知时
排入 B，A 的 END 通知时排入 C；预合成场景在准备完成后只排入一次音频流。

标准 SAPI 队列依据：[Microsoft SpVoice 文档](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms720149(v=vs.85))。回环捕获采用 [Microsoft 进程回环 API](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)。

公开生产 CLSID 入口也已在 x64/Win32 临时用户注册下分别通过客户端测试，
日志为 `research/prepare-public-entry-x64.txt` 和 `research/prepare-public-entry-Win32.txt`。
验证包含缓存命中、不播放、逐流 WORD/END 事件及空间回收；临时注册已清除。
日志中的“installed public COM service”表示探针选择生产 CLSID，不代表机器安装已完成。
机器更新是否完成应以 `research/install-hd-sapi-queue-service-status.json` 的 Success 为准；
没有成功状态文件时，不应声称新版本已经安装。
