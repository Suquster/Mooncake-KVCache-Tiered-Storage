"""vLLM 适配层包。

通过上游 Mooncake KV connector 接口把本项目接入 vLLM（需求 6.1）。
将 vLLM 的 KV 加载/存储回调翻译为 Scheduler 路由 + Data_Path 传输，
并把本项目的分层存储暴露为 connector 的后端 KV 池。

本任务（任务 1）仅建立包骨架；``ProjectKVConnector`` 等实现由任务 11 填充。
"""

__all__: list[str] = []
