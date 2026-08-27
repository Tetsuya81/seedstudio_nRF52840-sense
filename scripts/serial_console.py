#!/usr/bin/env python3
"""XIAO nRF52840 のUSB CDCに接続する最小シリアルツール (標準ライブラリのみ)。

使い方:
  scripts/serial_console.py                      # 受信を流し続ける (Ctrl-C で終了)
  scripts/serial_console.py -s "p,1.0,d,1.0,p"   # コマンドと待ち秒数を順に実行
"""
import argparse, codecs, glob, os, select, sys, termios, time

BAUD = termios.B115200


def find_port():
    for p in sorted(glob.glob('/dev/cu.usbmodem*')):
        return p
    sys.exit('XIAO nRF52840 が見つかりません (USB接続を確認してください)')


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, BAUD, BAUD, cc])
    return fd


def pump(fd, seconds, out=sys.stdout):
    """seconds 秒のあいだ受信を表示する"""
    end = time.time() + seconds
    buf = b''
    # マルチバイト文字がチャンク境界で割れないよう逐次デコードする
    dec = pump.decoder
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if data:
                buf += data
                out.write(dec.decode(data))
                out.flush()
    return buf


pump.decoder = codecs.getincrementaldecoder('utf-8')('replace')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port')
    ap.add_argument('-s', '--script',
                    help='カンマ区切り。文字はコマンド送信、数値は待ち秒数。例: "p,1.5,d"')
    ap.add_argument('-t', '--timeout', type=float, default=0.0,
                    help='スクリプト無しのときの受信時間(秒)。0で無限')
    args = ap.parse_args()

    port = args.port or find_port()
    fd = open_port(port)
    print(f'--- {port} @115200 ---', file=sys.stderr)

    try:
        pump(fd, 1.0)  # 起動バナーを拾う
        if args.script:
            for tok in args.script.split(','):
                tok = tok.strip()
                if not tok:
                    continue
                try:
                    delay = float(tok)
                    pump(fd, delay)
                    continue
                except ValueError:
                    pass
                print(f'\n>>> send {tok!r}', file=sys.stderr)
                os.write(fd, tok.encode())
                pump(fd, 0.3)
            pump(fd, 1.0)
        else:
            pump(fd, args.timeout if args.timeout > 0 else 1e9)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
