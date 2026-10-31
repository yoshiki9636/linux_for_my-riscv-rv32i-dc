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

このリポジトリのトップに置いたまま使う。カーネルは必ずトップの下の `linux/` に clone して
ビルドする(手順は「ビルド手順」)。パスはスクリプトの場所を基準に決まるので、どこから
実行してもよい。読み込むファイルは次のとおり:

- `linux/vmlinux`、`linux/System.map`、`linux/arch/riscv/boot/Image`、
  `linux/arch/riscv/boot/dts/myfpga/my_fpga.dtb`
- `initramfs.cpio`、`start/start.elf`(トップに置く。どちらも「ビルド手順」で作る)

出力はトップの `Image.reloc` と `Image.hex`。入力が1つでも見つからないと、そのパスを
表示して止まる。

## 7. initramfs

```
initramfs/busybox.config     # busybox-1.36.1 の .config(78 アプレット)
initramfs/mtest.c            # /bin/mtest のソース
initramfs/init               # /init スクリプト
```

`initramfs.cpio` はリポジトリに入れていない。「ビルド手順」の 3 で、busybox の clone から
作ってトップに置く。

この手順で作った cpio(1,851,904 B、md5 `30a63e02cedcb7e2661c459c272b83dc`)は、
2026-11-01 に実機で動作を確認した。中身は newc アーカイブ1本で、次のものが入っている:

- `/init`、`bin/busybox`(1,267,600 B)と 78 アプレット分のシンボリックリンク
  (すべて `/bin` に置いている)
- `bin/mtest`(strip 済み、571,876 B)
- `dev/console`(c 5,1)、`dev/null`(c 1,3)、`dev/ttyMY0`(c 4,64)
- 空のディレクトリ `proc`、`sys`、`tmp`、`etc`、`sbin`、`usr/bin`

busybox に埋め込まれるビルド日時が変わるので、作り直すと md5 は一致しない。

参考: kernel#904 で使っていた元の cpio(1,849,660 B)は、newc アーカイブを2つ連結した
ものだった(1つ目が busybox のツリー、2つ目が `bin/mtest`(569,496 B)だけ)。
mtest は元のビルドフラグの記録がなく、同じバイナリは作れない。

## ビルド手順

順番は 1. カーネル → 2. ブートスタブ → 3. initramfs → 4. FPGA 用イメージ。
initramfs を作るときに使う `gen_init_cpio` はカーネルのビルドでできるので、
カーネルを先にビルドする。

```bash
R=~/linux/linux_for_my-riscv-rv32i-dc     # このリポジトリのトップ
K=$R/linux                                # カーネルツリー(必ずこの場所)
```

### 1. カーネル

```bash
# upstream のツリーを $R/linux に用意する(ベースのコミットだけを取ってくる浅い clone)
mkdir $K && cd $K
git init
git remote add origin https://github.com/torvalds/linux.git
git fetch --depth 1 origin cbfffcca2bf0622b601b7eaf477aa29035169184
git checkout FETCH_HEAD
#   全履歴が必要な場合は、上の代わりに次を使う(数 GB あり、時間がかかる):
#   cd $R && git clone https://github.com/torvalds/linux.git && cd linux
#   git checkout cbfffcca2bf0622b601b7eaf477aa29035169184

# 差分を当てる
git apply $R/fpga-kernel.patch
cp $R/fpga.config .config

# ビルド
make ARCH=riscv CROSS_COMPILE=riscv32-unknown-linux-gnu- olddefconfig
make ARCH=riscv CROSS_COMPILE=riscv32-unknown-linux-gnu- -j$(nproc)
make ARCH=riscv CROSS_COMPILE=riscv32-unknown-linux-gnu- dtbs   # my_fpga.dtb
#   -> vmlinux、System.map、arch/riscv/boot/Image、usr/gen_init_cpio ができる
```

### 2. ブートスタブ

```bash
cd $R/start
riscv32-unknown-linux-gnu-gcc -march=rv32ima -mabi=ilp32 -c start.S -o start.o
riscv32-unknown-linux-gnu-ld -T start.ld start.o -o start.elf
```

### 3. initramfs

```bash
W=~/linux/initramfs-work                  # 作業ディレクトリ(どこでもよい)
mkdir -p $W && cd $W

# 3-1. busybox を clone して 1.36.1 にする
git clone https://git.busybox.net/busybox
#   公式サーバーが遅いときや落ちているときは GitHub のミラーも使える(タグ名は同じ):
#   git clone https://github.com/mirror/busybox.git
#   tarball の場合: wget https://busybox.net/downloads/busybox-1.36.1.tar.bz2 && tar xf busybox-1.36.1.tar.bz2
cd busybox
git checkout 1_36_1

# 3-2. .config をコピーする。defconfig などで作り直さないこと
#    (NOMMU、hush、CROSS_COMPILER_PREFIX、EXTRA_CFLAGS=-fPIE はこのファイルにしかない)
cp $R/initramfs/busybox.config .config
make oldconfig            # 同じ 1.36.1 なら何も聞かれないはず

# 3-3. busybox をビルドする(riscv32-unknown-linux-gnu-* に PATH が通っていること)
make CFLAGS_busybox="-static-pie -Wl,-z,norelro" -j$(nproc)
#   -> busybox(約 1.27 MB)ができる
make busybox.links
#   -> アプレット一覧(78 行)。make だけでは作られないので、別に作る
cd ..

# 3-4. mtest をビルドする。strip を忘れると cpio が 2 MB の窓を超えて
#      make_image_reloc.py がエラーになる
riscv32-unknown-linux-gnu-gcc -O2 -fPIE -static-pie -Wl,-z,norelro \
    -o mtest $R/initramfs/mtest.c
riscv32-unknown-linux-gnu-strip mtest

# 3-5. gen_init_cpio 用のリストを書く
#    リストの中では変数を使わず、$W から見た相対パスで書く。$W を書くと、エディタに
#    貼り付けたときなどに展開されずに残り、gen_init_cpio が失敗する
#    (途中で切れた 10 KB 程度の cpio ができる)。
cp $R/initramfs/init init
cat > initramfs.list <<'EOF'
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null    0666 0 0 c 1 3
nod /dev/ttyMY0  0600 0 0 c 4 64
dir /proc 0755 0 0
dir /sys  0755 0 0
dir /tmp  0755 0 0
dir /etc  0755 0 0
dir /sbin 0755 0 0
dir /bin  0755 0 0
dir /usr  0755 0 0
dir /usr/bin 0755 0 0
file /bin/busybox busybox/busybox 0755 0 0
file /bin/mtest   mtest 0755 0 0
file /init init 0755 0 0
EOF
for a in $(sed 's|.*/||' busybox/busybox.links); do
  echo "slink /bin/$a busybox 0777 0 0"; done >> initramfs.list

# 3-6. cpio を作る。gen_init_cpio は手順 1 のカーネルのビルドで $K/usr/ にできている
#    (デバイスノードもこれで作れるので root 権限は不要)
#    相対パスは今いるディレクトリから解決されるので、必ず $W で実行する。
cd $W
$K/usr/gen_init_cpio initramfs.list > initramfs.cpio \
  || { echo "ERROR: gen_init_cpio が失敗した"; rm -f initramfs.cpio; }
ls -l initramfs.cpio    # 約 1.85 MB になるはず。10 KB 程度ならファイルが読めていない

# 3-7. 中身を確認して、トップに置く
cpio -itv < initramfs.cpio | less     # dev のメジャー/マイナー番号とアプレット一覧を確認
cp initramfs.cpio $R/initramfs.cpio
```

注意:

- NOMMU では mprotect が ENOSYS を返す。そのため static-pie のバイナリは必ず
  `-z norelro` でリンクする。付けないと glibc の RELRO 処理が `_exit(127)` で終了する。
- `make_image_reloc.py` は cpio を `0x900000` に置く。cpio が DTS の
  `linux,initrd-end`(`0xB00000`、上限 2 MB)を超える場合や、ELF が PIE(ET_DYN)で
  ない場合はビルドを失敗させる。

### 4. FPGA 用イメージ

```bash
python3 $R/make_image_reloc.py     # $R/Image.reloc と $R/Image.hex ができる
```

## 参考: 元のビルドの出力

kernel#904 の元の出力(このリポジトリには入れていない):
```
Image.hex      # md5 4add1721247851ebee48b448d8bfd87d
Image.reloc    # md5 3c07249b4db3dd47369d55527deb86b1
```
再ビルドすると、カーネルのバナーに入るビルド番号と日時が変わるので、md5 は一致しない。
元の .good-899 と #904 の違いもこのバナー(36 バイト)だけだった。
