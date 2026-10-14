# 贡献

## 构建

```bash
python3 tools/gen/validate_project.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DEMASTER_BUILD_HARDWARE_TOOLS=ON -DEMASTER_BUILD_TESTS=ON
cmake --build build -j4
```

## 测试

```bash
ctest --test-dir build --output-on-failure    # 单元测试
bash tests/checks/ci_replay.sh                # 离线检查，按 CI 顺序跑完整套
```

`ci_replay.sh` 里的每条检查可单独运行：`python3 tests/checks/<检查>.py`。带 `--mutate` 的检查会先验证自己抓得住错，再跑正式判定。

## 提交前

- 构建零告警。
- `ctest` 全绿。
- `ci_replay.sh` 全绿。
- 改到用户可见的提示文案，同时更新 `config/messages/zh_CN.json` 与引用它的代码。
- 改到 `config/` 下任何字段，同时更新校验器和 `config/README.md`。

## 代码约定

- 格式由 `.clang-format` 和 `.editorconfig` 定，提交前按它格式化。
- 注释与文档用中文，按 ASD-STE100：一句一个事实，主动语态，现在时。
- 业务代码只引用消息 ID，不嵌入操作者可见的提示文本。
- 逻辑代码不记录具体电机型号。

## 提交

- 一个提交一件事，提交信息写清为什么改，不写流水账。
- 不要提交 `build/`、`runtime/` 下的产物。
