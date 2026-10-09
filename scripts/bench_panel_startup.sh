#!/bin/bash
# 鍙版灦锛氭妸闈㈡澘鎵旇繘"涓荤珯杩樻病杩?RUNNING"閭ｆ绐楀彛閲岀┖杞紝鐪嬪畠宕╀笉宕┿€?# 杩欐槸 P12.16锛堥潰鏉夸竴甯у氨姝汇€佷富绔欐病浜虹湅鐫€璺戝埌涓ゆ牴杞磋繘鏁呴殰锛夌殑瀹炴満鍥炲綊銆?#
# 涓轰粈涔堥潪寰椾笂鍙版灦锛氬穿鍦?*姣忓抚閮界敾**鐨勯〉鑴氫笂鈥斺€擿_headroom_text` 鎷块€変腑杞寸殑杞寖鍥翠綑閲忥紝
# 浣欓噺瑕?counts/搴︾殑鎹㈢畻绯绘暟銆傝€岀郴鏁版槸 `refresh_topology()` 璇诲洖鏉ョ殑锛宍attach()` 鍦?# `master_state != 4` 閭ｄ竴鍙ュ氨鎻愬墠 return 浜嗭紝绯绘暟杩樻槸绌虹殑锛涘悓涓€鏃跺埢 `poll_status()`
# 宸茬粡鎶?5 鏉¤酱濉炶繘浜?`engine.axes`锛屼簬鏄?`_headroom_text` 鐨?`if not engine.axes`
# 鎷︿笉浣忥紝瑁镐笅鏍?`self.factor[0]` 鎶?IndexError銆傝繖涓獥鍙ｅ彧鏈夌湡涓荤珯鎵嶆湁銆?#
# 鏈剼鏈嚜宸辨妸涓荤珯鎷夎捣鏉ワ紙璧扮殑灏辨槸鐢ㄦ埛韪╁埌鐨勯偅鏉¤矾锛氶潰鏉?`--start` + `--obs`锛夛紝
# 鍐嶈嚜宸辨妸瀹?SIGINT 鍋滄帀锛屼笉鐣欎富绔欏湪璺戙€?#
# 涓轰粈涔?杞綍閲屽嚭鐜拌繃銆屼富绔欒繕鍦ㄥ惎鍔ㄣ€?灏卞璇佹槑璧拌繘浜嗙獥鍙ｏ細閭ｅ彞璇濆啓鍦?attach() 閲?# `poll_status()` 鎴愬姛涔嬪悗鐨勪笅涓€鍙ュ垽鏂笂銆傜湅鍒板畠 鈬?poll_status 鎴愬姛杩?鈬?閭ｄ竴鍒?# `engine.axes` 宸茬粡琚?5 鏉¤酱濉笂浜嗐€傝€?`status` 鐨勮酱琛屾槸鎸?`plan->axis_count` 鍙戠殑
# 锛坰ession_control.c:707-715锛夛紝璺?state 鏃犲叧锛屼細璇濈瓟寰椾笂璇濆氨鏈夆€斺€旀墍浠ョ獥鍙ｉ噷杞村繀闈炵┖銆?# 2026-09-18 鍦ㄥ彴鏋朵笂鐩存帴闂繃涓荤珯锛屽洖鍖呮槸 `state=3 axes=5 杞磋=5`銆?#
# 鑳介獙鐨?/ 涓嶈兘楠岀殑锛岃娓呮锛?#   鑳介獙鈥斺€旂湡绐楀彛閲岄潰鏉夸竴璺噸鐢汇€佷笉鎶涘紓甯搞€佽酱琛ㄧ収鐢汇€傜┖杞湡闂翠笉缁欎换浣曟寜閿紝
#        鎵€浠ユ瘡甯ч兘鏄?鍒氭帴涓?杩樻病鎺ヤ笂涓荤珯"杩欎竴娈碉紝涓嶆槸琚寜閿甫杩囧幓鐨勩€?#   涓嶈兘楠屸€斺€?淇墠閭ｄ唤浠ｇ爜鍦ㄨ繖閲屽繀鐒跺穿"銆傞偅鐢辩绾垮鐓ц礋璐ｏ紝璺戯細
#        python3 checks/console_engine_check.py --mutate-startup-window
#        涓よ€呭悎璧锋潵鎵嶆槸闂幆锛氱绾胯瘉鏄庢鏌ユ湁鍒ゅ埆鍔涳紝杩欓噷璇佹槑鐪熺獥鍙ｈ繘寰楀幓銆?#   涓嶈兘楠屸€斺€旂獥鍙ｆ湁澶氶暱銆傚畠闅忎富绔欏垵濮嬪寲鑰楁椂鍙橈紝鏈剼鏈彧鏄┖杞涔呭幓瑕嗙洊瀹冿紱
#        鍒ゆ嵁浜屼細鍛婅瘔浣犺繖涓€杞埌搴曡娌¤鐩栧埌銆?#
# 鍏ㄧ▼**涓嶅彂浠讳綍鐩爣**锛氬彧鏈変富绔欎娇鑳藉苟淇濇寔浣嶇疆锛岃酱涓嶅姩銆?# 蹇呴』鍦?root 涓嬭繍琛岋紙涓荤珯瑕?mlockall/SCHED_FIFO锛屽鎺ュ瓧鐢?root 鍒涘缓锛夛細
#   sudo bash /home/orangepi/ethercat-master/scripts/bench_panel_startup.sh
#
# 鍙傛暟锛堢幆澧冨彉閲忥級锛?#   DEPLOYMENT  閮ㄧ讲 ID锛岄粯璁?orangepi-bench-quint-30deg锛堝彴鏋剁幇鏈?5 涓粠绔欙級
#   WAIT        绌鸿浆绉掓暟锛岄粯璁?75銆傛兂澶氳鐩栦竴浼氬効灏辫皟澶с€?
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEPLOYMENT=${DEPLOYMENT:-orangepi-bench-quint-30deg}
WAIT=${WAIT:-75}

OUT=/tmp/panel_startup.txt
MASTER_LOG=/tmp/emaster-console-master.log

if [ "$(id -u)" != "0" ]; then
    echo "閿欒锛氶渶瑕?root锛堜富绔欒 mlockall/SCHED_FIFO锛屽鎺ュ瓧鐢?root 鍒涘缓锛? >&2
    exit 1
fi

cd "$REPO" || exit 1

# 娓呯悊涓婃閬楃暀锛氬厛鍋滀富绔欙紝鍐嶅垹濂楁帴瀛楋紝閬垮厤鏂颁富绔欑粦鍒版棫 inode 涓?pkill -9 -f emaster-master 2>/dev/null
sleep 1
rm -f /tmp/emaster-*.sock
rm -f "$OUT" "$MASTER_LOG"

echo "=== 闈㈡澘鍚姩绐楀彛 路 绌鸿浆 ${WAIT}s 路 涓嶅彂浠讳綍鐩爣 ==="
echo "杞綍=$OUT 涓荤珯鏃ュ織=$MASTER_LOG"

# 涓嶇粰鎸夐敭锛岃瀹冭嚜宸遍噸鐢伙紱鍒扮偣浜嗙粰涓€涓?Q 閫€鍑恒€?# script(1) 鏄负浜嗙粰闈㈡澘涓€涓湡 tty锛氭病鏈?tty 瀹冩嬁涓嶅埌缁堢灏哄锛岃蛋鐨勪笉鏄悓涓€鏉¤矾銆?{ sleep "$WAIT"; printf 'Q'; sleep 2; } | script -q -c \
    "stty cols 100 rows 30; ulimit -r unlimited; timeout $((WAIT + 40)) bash scripts/console.sh --start --obs" \
    "$OUT"
echo "闈㈡澘閫€鍑虹爜=$?"

# 闈㈡澘璧颁簡锛屼富绔欒繕寮€鐫€锛堟病缁?--allow-stop-master锛岄潰鏉夸笉浼氬叧瀹冿級銆傛寜鍙版灦绾緥绔嬪埢鍋滐紝
# 璧?SIGINT 璁╂姤鍛婂啓鍑烘潵銆?MPID=$(pgrep -f emaster-master | head -1)
if [ -n "$MPID" ]; then
    echo "涓荤珯 pid=$MPID锛屽彂 SIGINT"
    kill -INT "$MPID" 2>/dev/null
    for _ in $(seq 1 60); do
        kill -0 "$MPID" 2>/dev/null || break
        sleep 0.5
    done
fi
if pgrep -f emaster-master >/dev/null; then
    echo "** 涓荤珯娌￠€€鍑猴紝寮烘潃"
    pkill -9 -f emaster-master
else
    echo "涓荤珯宸叉湁搴忛€€鍑?
fi
pgrep -f emaster-master >/dev/null && echo "** 杩樻湁涓荤珯鍦ㄨ窇" || echo "锛堝凡纭娌℃湁涓荤珯鍦ㄨ窇锛?

echo ""
echo "=== 鍒ゅ畾 ==="

if [ ! -f "$OUT" ]; then
    echo "[澶辫触] 娌℃湁杞綍鏂囦欢鈥斺€旈潰鏉垮帇鏍规病璧锋潵"
    exit 1
fi

# 鍒ゆ嵁涓€锛氶潰鏉挎湁娌℃湁鎶涘紓甯搞€傜储寮曡秺鐣岄偅鏉″穿婧冧細鐣欎笅 Traceback/IndexError銆?if grep -qE "Traceback|IndexError" "$OUT"; then
    echo "[澶辫触] 杞綍閲屾湁寮傚父锛?
    grep -nE "Traceback|IndexError" "$OUT" | head -10
    exit 1
fi
echo "[閫氳繃] 杞綍閲屾病鏈?Traceback / IndexError"

# 鍒ゆ嵁浜岋細闈㈡澘纭疄澶勫湪杩囬偅涓獥鍙ｂ€斺€斿畠鎵撹繃"涓荤珯杩樺湪鍚姩锛坰tate=鈥︼級"杩欏彞璇濄€?# 杩欏彞璇濆彧鏈?attach() 璧板埌 master_state!=4 閭ｄ竴鍙ユ墠鍐欏緱鍑烘潵锛岃€岄偅鏄琛屾槸鎺掑湪
# poll_status() 鎴愬姛涔嬪悗鐨勶細鐪嬪埌瀹冿紝灏辩瓑浜庤瘉鏄?engine.axes 褰撴椂闈炵┖銆?# 灏戜簡杩欎竴鏉★紝"闈㈡澘娌″穿"鍙兘鍙槸鍥犱负浠庡ご鍒板熬娌¤繘绐楀彛銆?if grep -q "涓荤珯杩樺湪鍚姩" "$OUT"; then
    echo "[閫氳繃] 杞綍閲屽嚭鐜拌繃鍚姩绐楀彛锛堜富绔欒繕鍦ㄥ惎鍔?state鈮?锛?
else
    echo "[璀﹀憡] 杞綍閲屾病鐪嬪埌鍚姩绐楀彛鈥斺€斾富绔欒繘 RUNNING 澶揩锛岃繖涓€杞病瑕嗙洊鍒帮紝"
    echo "       鎶?WAIT 璋冨ぇ閲嶈窇锛屾垨纭涓荤珯鏄笉鏄凡缁忓湪璺戯紙閭ｆ牱闈㈡澘浼氱洿鎺ユ帴涓婏級"
fi

# 鍒ゆ嵁涓夛細闈㈡澘鐢诲嚭浜嗚酱琛紝涓嶆槸绌鸿窇涓€鍦恒€?if grep -q "杞?" "$OUT"; then
    echo "[閫氳繃] 闈㈡澘鐢诲嚭浜嗚酱琛?
else
    echo "[澶辫触] 杞綍閲屾病鏈夎酱琛ㄢ€斺€旈潰鏉胯捣鏉ヤ簡浣嗘病鐢诲嚭鏉?
    tail -30 "$OUT"
    exit 1
fi

# 鍒ゆ嵁鍥涳紙淇℃伅锛夛細涓荤珯閭ｆ鎷夎捣鏉ユ垚娌℃垚銆傛敞鎰忛潰鏉垮穿浜嗕富绔欎笉浼氳窡鐫€閫€鍑猴紝
# 鎵€浠?涓荤珯璧锋潵浜?鏈韩涓嶈兘褰撴垚闈㈡澘娌″穿銆?echo ""
if [ -f "$MASTER_LOG" ]; then
    echo "涓荤珯鏃ュ織灏鹃儴锛堜緵瀵圭収锛夛細"
    grep -E "\[SHUTDOWN\]|鎬荤嚎鐘舵€? "$MASTER_LOG" | tail -4
else
    echo "锛堟病鏈変富绔欐棩蹇?$MASTER_LOG锛?
fi

echo ""
echo "缁撹锛氶潰鏉垮湪鍚姩绐楀彛閲屾椿涓嬫潵浜?
