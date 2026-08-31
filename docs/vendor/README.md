# 受控供应商资料

由于尚无再分发授权记录，供应商原始资料不提交到公开仓库。经过授权的副本应安装到
`SHA256SUMS` 列出的路径；预期 ESI 路径为 `docs/lz-joint/ECAT_CIA402.xml`。

批准工程发布前必须运行：

```sh
python3 tools/validate_project.py --root . --require-vendor-artifacts
```

不使用该参数时，受控资料缺失会被报告，但不会使公开仓库的干净检出构建失败。任何已安装资料
只要哈希不正确，校验始终失败。
