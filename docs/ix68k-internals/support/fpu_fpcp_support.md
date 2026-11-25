# X68030 FPCP / MC68882 対応状況

X68030 構成では、`fpu = on` および `fpu = 68882` が
68EC030 / 68030 にCPU所有の MC68882 を FPCP（cpID=1）で接続します。`fpu = off`（既定）
はFPUなしで起動します。X68030プロファイルは `fpu = on` を明記します。`68882` をそれ以外のCPUモデルに指定すると起動前にエラーになります。

FPCPはMMIOではなくCPU空間の接続です。したがってCPU空間フックは設定せず、
`FLOAT4.X` の FE コール（cpID=7）はHuman68kのF-line処理へ渡る。CIR（CZ-6BP1、
CIR（CZ-6BP1）は68000での `fpu = on|68881|68881-secondary` が `$E9E0xx` に
接続する別方式であり、030系のFPCP設定とは排他的です。

検証は `fpu_fpcp_x68030_test` と、IPLROM30・Human302の純正 `FLOAT4.X` を使う
`fpu_fpcp_float4_e2e` で行います。後者は `tools-host/dev/make_float4_xdf.sh` が
`disk/FLOAT4.XDF` を生成し、IPLROM30のFNOP検出、FLOAT4常駐、代表FE演算と
`fpu=none` の非常駐対照を確認します。
