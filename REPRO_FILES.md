# 現在の Linux イメージ(kernel#904、2026-10-30 08:01)を再現するためのファイル

このリポジトリには、upstream Linux からの差分と、FPGA 用イメージ(`Image.reloc`/`Image.hex`)
を作るのに必要なファイルだけを置いている。

カーネルのベース: upstream のコミット `cbfffcca2bf0622b601b7eaf477aa29035169184`
(Merge tag 'trace-v7.0-rc5'、7.0.0-rc5)。

ツールチェーン: `riscv32-unknown-linux-gnu-*`(gcc 15.2.0、binutils 2.46。
元の環境では `/opt/riscv32im.linux/bin` にある)。

## 1. カーネルソース: upstream から変更した既存ファイル(7)

```
arch/riscv/boot/dts/Makefile
arch/riscv/kernel/entry.S
arch/riscv/kernel/head.S
arch/riscv/kernel/traps.c
drivers/clocksource/timer-clint.c
drivers/tty/serial/Kconfig
drivers/tty/serial/Makefile
```

## 2. カーネルソース: 新規ファイル(3)

```
drivers/tty/serial/myuart.c
arch/riscv/boot/dts/myfpga/Makefile
arch/riscv/boot/dts/myfpga/my_fpga.dts
```

## 3. パッチ

```
fpga-kernel.patch
```

1章と2章の10ファイル分を、`cbfffcca2` からのパッチ1本にまとめたもの。
`cbfffcca2` に当てると、1章・2章のファイルと同じ内容になる(確認済み)。
パッチを当てるか、1章・2章のファイルを上書きコピーするかのどちらかでよい。

## 4. カーネル設定

```
fpga.config        # 実際にビルドに使った .config
```

## 5. ブートスタブ

```
start/start.S
start/start.ld
```

## 6. イメージ後処理

```
make_image_reloc.py
```

カーネルツリーのトップで実行する。読み込むファイルは次のとおり:
`arch/riscv/boot/Image`、`System.map`、`arch/riscv/boot/dts/myfpga/my_fpga.dtb`、
`initramfs.cpio`、`start/start.elf`。
出力は `Image.reloc` と `Image.hex`。

## 7. initramfs

```
initramfs.cpio               # そのまま使う(1,849,660 B)
initramfs/busybox.config     # busybox-1.36.1 の .config(78 アプレット)
initramfs/mtest.c            # /bin/mtest のソース
initramfs/init               # /init スクリプト(initramfs.cpio から取り出したもの)
```

`initramfs.cpio` にはデバイスノード(dev/console、dev/null、dev/ttyMY0)が入っているので、
root 権限なしでは作り直せない。ビルドにはこのバイナリをそのまま使う。
`initramfs/` の下は、中身を作り直すときの参考として置いている。

cpio は newc アーカイブを2つ連結したもの(1つ目が busybox のツリー、2つ目が
`bin/mtest`)。`cpio -it` では1つ目しか表示されない。

busybox を作り直す場合(busybox-1.36.1 は upstream のリリース版):
```
cp initramfs/busybox.config busybox-1.36.1/.config
cd busybox-1.36.1
make CFLAGS_busybox="-static-pie -Wl,-z,norelro"
```
`.config` で CROSS_COMPILER_PREFIX=riscv32-unknown-linux-gnu-、EXTRA_CFLAGS=-fPIE を設定済み。
NOMMU では mprotect が使えないので、static-pie のバイナリは必ず `-z norelro` でリンクする。

## ビルド手順

```
# upstream のツリーを用意する
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux
git checkout cbfffcca2bf0622b601b7eaf477aa29035169184

# 差分を当てる(R はこのリポジトリのパス)
R=~/linux/linux_for_my-riscv-rv32i-dc
git apply $R/fpga-kernel.patch
cp $R/fpga.config .config
cp -r $R/start .
cp $R/make_image_reloc.py $R/initramfs.cpio .

# カーネル
make ARCH=riscv CROSS_COMPILE=riscv32-unknown-linux-gnu- olddefconfig
make ARCH=riscv CROSS_COMPILE=riscv32-unknown-linux-gnu- -j$(nproc)
make ARCH=riscv CROSS_COMPILE=riscv32-unknown-linux-gnu- dtbs   # my_fpga.dtb

# ブートスタブ
(cd start && \
 riscv32-unknown-linux-gnu-gcc -march=rv32ima -mabi=ilp32 -c start.S -o start.o && \
 riscv32-unknown-linux-gnu-ld -T start.ld start.o -o start.elf)

# FPGA 用イメージ
python3 make_image_reloc.py     # Image.reloc と Image.hex ができる
```

## 参考: 元のビルドの出力

kernel#904 の元の出力(このリポジトリには入れていない):
```
Image.hex      # md5 4add1721247851ebee48b448d8bfd87d
Image.reloc    # md5 3c07249b4db3dd47369d55527deb86b1
```
再ビルドすると、カーネルのバナーに入るビルド番号と日時が変わるので、md5 は一致しない。
元の .good-899 と #904 の違いもこのバナー(36 バイト)だけだった。
