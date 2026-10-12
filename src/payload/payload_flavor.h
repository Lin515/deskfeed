#pragma once

// 负载的**构建风味**开关（唯一出处）。
//
// 负载有两个产物、一份源：
//   * `deskfeed_payload`      —— 生产：注入 explorer.exe 的就是它。它里面只有一个东西：
//                                注入式任务栏读数的生产逻辑。
//   * `deskfeed_payload_test` —— 自测：与生产**逐字节同源**，只多 `DESKFEED_DLL_TEST_API`。
//                                探针 ABI、完成事件变异、安装延迟、探针布防、observer 只存在于它。
//
// 原生离线用例目标（`deskfeed-native-tests`）把 `hook_post.cpp` / `action_worker.cpp` 等**再编译
// 一次**，它也要这些开关（用例靠它们制造确定性对抗窗口），所以用 `DESKFEED_NATIVE_TESTS` 一起算。
//
// 生产 DLL 两个定义都没有 ⇒ `DESKFEED_PAYLOAD_PROBE_BUILD` 不成立 ⇒ 探针入口与全部对抗开关
// **根本不编译**。注意这不是"启动时把它们复位成正常"：生产版本里不存在任何"活的故意错误路径"，
// 所以也不存在"caller 布防就能改变生产行为"这条路径。
#if defined(DESKFEED_DLL_TEST_API) || defined(DESKFEED_NATIVE_TESTS)
#define DESKFEED_PAYLOAD_PROBE_BUILD 1
#endif
