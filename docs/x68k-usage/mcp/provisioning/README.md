# MCP対応のAIエージェント向け環境構築レシピ

ホストのシェルコマンドを実行できるMCP対応のAIエージェントが、利用者の指示に従ってX68000の
実行環境を構築するための固定手順。手順を推測で組み立てず、目的に対応する文書を読んで実行します。

| 目的 | 手順 |
|------|------|
| 別ディレクトリへエミュレータを新規導入する | [emulator_install.md](emulator_install.md) |
| 起動可能なSCSI HDDイメージを作成する | [hdd_install.md](hdd_install.md) |
| 既存HDDへADPCM／Z-MUSIC音源環境を導入する | [sound_driver_install.md](sound_driver_install.md) |

MCPクライアントとの接続設定は親ディレクトリの [MCP設定](../mcp_llm_setup.md) を参照します。
