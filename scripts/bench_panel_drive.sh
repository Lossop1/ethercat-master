#!/bin/bash
# 鍙版灦锛氫笉鐢ㄤ汉瀹堢潃閿洏锛屾妸涓€涓叉寜閿杺缁欓潰鏉胯窇涓€閬嶃€?#
# script(1) 缁欓潰鏉块€犱竴涓湡 pty锛堥潰鏉胯姹?stdin 鏄粓绔級锛屾垜浠粠绠￠亾閲屾寜鑺傚鍠傛寜閿紝
# 闈㈡澘鐨勫師濮嬭緭鍑猴紙鍚浆涔夊簭鍒楋級钀藉埌 /tmp/panel-out.txt銆傝浆褰曢噷鑳介獙鐨勬槸锛?#   1) 杩涗簡澶囩敤灞忓箷銆佹瘡甯х敤缁濆瀹氫綅銆侀潰鏉胯嚜宸辩殑杈撳嚭閲屾病鏈?\r\n锛堚噿 鐗╃悊涓婁笉浼氭粴灞忥級锛?#   2) 鎵撳瓧绐楀彛閲岄潰鏉跨殑鎺у埗鎷嶈繕鍦ㄦ帹杩涳紙甯ч噷鐨勬媿鍙峰湪娑級锛?#   3) 閫€鍑哄悗闈㈡澘鎶婄粓绔師鏍疯繕鍥炲幓銆佷富绔欎粛鍦ㄨ窇銆?# 杞綍鐢?analysis/panel_transcript.py 瑙ｆ瀽銆?#
# 涓€鍙ヨ瘽鍒寚鏈涜繖鏉¤剼鏈兘楠岋細涓荤珯鏃ュ織娌℃湁鏃堕棿鎴筹紝"鏌愭 [MODE] 鍒囨崲鏄笉鏄墦瀛楅€犳垚鐨?
# 鍒や笉鍑烘潵锛堣 layering-plan.md P10.4锛夈€傞潰鏉夸笉鍙戠洰鏍囨椂涓荤珯蹇呯劧瓒呮椂鍥?HOLD 鈥斺€?# 閭ｆ槸鐪嬮棬鐙楀湪骞叉椿锛屼笉鏄晠闅滐紝鏁板畠鏈夊嚑瀵硅鏄庝笉浜嗛棶棰樸€?#
# 鍓嶇疆锛氫富绔欏凡缁忓湪璺戯紙鏈剼鏈笉璐熻矗璧蜂富绔欙級銆傜敤 --range 3 鎶婅蒋鑼冨洿鍘嬪埌 卤3掳锛?# 浜庢槸寰€杩旈獙璇佺殑榛樿鎸箙鏄?min(30, 3脳0.6)=1.8掳锛屽叏绋嬮兘鏄皬鍔ㄤ綔銆?cd "$(dirname "$0")/.." || exit 1
RANGE=${RANGE:-3}
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
OUT=/tmp/panel-out.txt
LOG=/tmp/emaster-console-master.log
MARK=/tmp/panel-mark.txt

date +%s > "$MARK"          # 鎵撳瓧绐楀彛鐨勮捣鐐癸紝浜嬪悗鐢ㄥ畠鍒囦富绔欐棩蹇?: > "$OUT"

{
    sleep 8                                  # 绛夐潰鏉挎帴涓婁富绔欍€佺敾鍑虹涓€甯?    printf 'g';      sleep 1                 # 鎵撳紑杈撳叆琛?    printf '1:0.5 4:0.5'; sleep 1            # 澶氳酱鍐欐硶锛氳酱1銆佽酱4 鍚勮蛋 0.5掳
    printf '\r';     sleep 4                 # 鍥炶溅鎻愪氦
    printf 't';      sleep 20                # 寰€杩旈獙璇侊紙鎸箙 1.8掳锛?    printf 't';      sleep 3                 # 鍋滃線杩?    printf '0';      sleep 3                 # 鍥炲惎鍔ㄤ綅缃?    printf 'Q';      sleep 2                 # 閫€鍑洪潰鏉?} | script -q -c \
    "stty cols 80 rows 24; timeout 60 python3 tools/emaster_console.py --deployment $DEPLOYMENT --range $RANGE" \
    "$OUT"

echo "闈㈡澘杞綍锛?(wc -c < "$OUT") 瀛楄妭锛?(grep -c $'\x1b\[[0-9]*;1H' "$OUT" 2>/dev/null || echo 0) 琛岀粷瀵瑰畾浣?
