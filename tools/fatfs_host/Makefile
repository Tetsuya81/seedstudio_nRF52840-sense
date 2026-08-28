# 実機に触れずに f_mkfs の生成ジオメトリを測る。
#   make        -> n_fats=1 (上流既定) と n_fats=2 の両方をビルド
OUT     ?= .
#   make run    -> 両方を実行
CC      ?= cc
CFLAGS  ?= -std=c99 -O1 -Wall -Wno-unused-but-set-variable -I.
SRC      = main.c diskio_file.c fatfs/ff.c

all: mkfs_f1 mkfs_f2

mkfs_f1: $(SRC)
	$(CC) $(CFLAGS) -DMKFS_N_FATS=1 -o $@ $(SRC)

mkfs_f2: $(SRC)
	$(CC) $(CFLAGS) -DMKFS_N_FATS=2 -o $@ $(SRC)

run: all
	@echo "################ n_fats = 1 (上流既定) ################"; ./mkfs_f1 $(OUT)
	@echo; echo "################ n_fats = 2 (提案) ####################"; ./mkfs_f2 $(OUT)

clean:
	rm -f mkfs_f1 mkfs_f2 *.img
.PHONY: all run clean
