# VersionUpdate
A Unreal  plugin for downloading and installing files like pak exe....
暂时用于与hotpatcher 配套使用，部分联动功能以及本身功能暂未完善


###  TODO List
感谢hxd
~~1. 整理逻辑和命名（安装器和CVO）
2. 安装后的客户端版本号处理
3. 大版本更新顺便更新基于该大版本的补丁
4. HTTP优化，断点续传，分片....
5. 安装前 md5 size校验
6. unmount 会报错，不重启的热更会崩溃
7. 文件下载完成后通知加标记
8. 本地json保存补丁包，方便清理无用包（以及清理验证功能）
9. subsystem 改造~~

1.分片下载暂时没有测试，如果不成功会回退成普通下载
2.编辑器功能需要完善，现在好j8难用，最好能够做diff、自动化等
3.丢弃包还没完全测好
