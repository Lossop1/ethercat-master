#!/bin/bash
# 鍙版灦澶氳疆娴嬭瘯椹卞姩鍣細鎶?璺戜竴杞?鐨勮剼鏈噸澶?N 杞紝骞舵妸姣忎竴杞殑璇佹嵁褰掓嫝鍒颁粨搴撻噷銆?#
#   sudo env DEPLOYMENT=<閮ㄧ讲ID> SERIES=<绯诲垪鍚? ROUNDS=6 GAP=60 MODE=reciprocate \
#        bash scripts/bench_series.sh
#
# 涓轰粈涔堣鏈夎繖涓€灞傘€傛帓鏌?杩欐潯鏁呴殰鏄繀鐜般€侀殢鏈恒€佽繕鏄病鏈?鍙兘闈犺繛璺戯紝鑰屽崟杞剼鏈?# 锛坆ench_idle.sh / bench_reciprocate_30deg.sh锛変竴娆″彧璺戜竴杞€佷骇鐗╄惤鍦?/tmp銆傝繛璺?# 鍗佸嚑杞箣鍚庯紝"绗嚑杞槸浠€涔堝舰鐘?灏辨暎鍦ㄤ竴鍫?/tmp 鏂囦欢閲岋紝鎶ュ憡鍙堟槸瑕嗙洊寮忕殑鈥斺€斾笂涓€杞?# 杩樻病鐪嬪氨琚笅涓€杞洊鎺夈€傝繖涓剼鏈妸閭ｄ欢浜嬫敹鎴愪竴浠讹細**姣忚疆涓€浠借嚜瓒崇殑璇佹嵁鐩綍**銆?#
# 瀹冧笉鑷繁璧蜂富绔欍€佷笉鑷繁鍙戠洰鏍囷紝閭ｄ簺浠嶇敱鍗曡疆鑴氭湰璐熻矗锛涜繖閲屽彧绠?閲嶅 + 鏀惰瘉鎹?銆?# 鎵€浠ュ弬鏁颁笉鐢ㄥ湪杩欓噷鍐嶅垪涓€閬嶁€斺€旂幆澧冨彉閲忓師鏍疯瀛愯剼鏈户鎵匡紙瑙佷笅闈?鍙傛暟"锛夈€?#
# 鍙傛暟锛堢幆澧冨彉閲忥級锛?#   DEPLOYMENT  蹇呭～銆傞儴缃?ID銆傛病鏈夐粯璁ゅ€硷細鍐欎竴涓粯璁ら儴缃茬瓑浜庢妸鏌愪竴涓彴鏋跺啓杩涘伐鍏烽噷銆?#   SERIES      蹇呭～銆傜郴鍒楀悕锛屽喅瀹氳瘉鎹洰褰?reports/_<SERIES>/銆?#   ROUNDS      杞暟锛岄粯璁?6銆?#   GAP         杞棿闂撮殧绉掓暟锛岄粯璁?60銆?*杩欎釜鍊兼湰韬槸鍙橀噺**锛氳儗闈犺儗閲嶅惎浼氫笉浼氳椹卞姩鍣?#               鏉ヤ笉鍙婂氨缁紝鏄緟鏌ョ殑闂涔嬩竴锛屾墍浠ラ棿闅旇鑳借皟銆佸苟涓旇杩?index.tsv銆?#   MODE        idle | reciprocate锛岄粯璁?reciprocate锛屽喅瀹氳皟鍝釜鍗曡疆鑴氭湰銆?#   鍏朵綑锛圖URATION锛廡RAVERSE锛廌EGREES锛廟ATE锛廜NLY_AXIS锛廔DLE锛廜BSERVATION 鈥︼級
#               鍘熸牱浼犵粰鍗曡疆鑴氭湰锛屽悇鑷湁鍚勮嚜鏂囨。锛岃繖閲屼笉閲嶅涔熶笉鏍￠獙銆?#
# 浜х墿锛歳eports/_<SERIES>/
#         index.tsv            姣忚疆涓€琛岋紙琛ㄥご鏉ヨ嚜 report_row.py --header锛?#         r<NN>_<TAG>/master.log      涓荤珯鏃ュ織
#         r<NN>_<TAG>/outcome.txt     鍗曡疆鑴氭湰鐨勫叏绋嬭緭鍑?#         r<NN>_<TAG>/client.log      瀹㈡埛绔棩蹇楋紙鏈夌殑璇濓級
#         r<NN>_<TAG>/summary.txt     report_summary.py 瀵硅杞姤鍛婄殑杈撳嚭
#         r<NN>_<TAG>/row.tsv         璇ヨ疆鍦?index.tsv 閲岄偅涓€琛?#         r<NN>_<TAG>/report-name.txt 璇ヨ疆鍦ㄥ綊妗ｇ洰褰曢噷鐨勫畬鏁存姤鍛婃枃浠跺悕
#         r<NN>_<TAG>/condition.txt     杩欎竴杞柦鍔犵殑鏉′欢锛堣酱鏁?鏃堕暱/鑺傛媿/闂撮殧/瑙傛祴鈥︼級
#         r<NN>_<TAG>/device-identity.txt 杩欎竴杞窇鐨勬槸浠€涔堜唬鐮併€佷粈涔堥厤缃€佹€荤嚎涓婃槸鍝嚑鍙?#         r<NN>_<TAG>/anomalies.tsv     璇ヨ疆涓荤珯鏃ュ織閲岀殑寮傚父锛屾寜鎺㈢储浠诲姟涔?3.4 鑺傜殑鍒?#         r<NN>_<TAG>/notes.md          鐜板満瑙傚療璁板綍锛?娌＄湅鎳傜殑"涔熷啓鍦ㄨ繖閲?#         r<NN>_<TAG>/nic-before.txt    缃戝崱鏀跺彂璁℃暟锛堣疆鍓嶏級
#         r<NN>_<TAG>/nic-after.txt     缃戝崱鏀跺彂璁℃暟锛堣疆鍚庯級
#         r<NN>_<TAG>/nic-diff.txt      涓よ€呯殑宸笌鍒ゅ畾锛氬抚鏈夋病鏈夊洖鍒扮綉鍗?#       **瀹屾暣鎶ュ憡涓嶅叆搴?*锛堝嚑鍗佸埌鍑犵櫨 MB锛夛紝瀹冪暀鍦ㄩ儴缃查厤缃?run_report_path 鎵€鎸?#       鐩綍鐨?archive/ 涓嬶紝涓婇潰閭ｄ釜鏂囦欢鍚嶅氨鏄寚鍚戝畠鐨勫潗鏍囥€?#
# 閫€鍑虹爜锛? = 鍏ㄩ儴杞閮借窇瀹屼簡锛?*涓嶄唬琛ㄦ瘡杞兘鎴愬姛**鈥斺€旀垚璐ュ湪 index.tsv 閲岀湅锛夛紱
#         1 = 鐜涓嶅锛堟病鏈?root銆佹病鏈?DEPLOYMENT銆佹姤鍛婅矾寰勮В鏋愪笉鍑烘潵锛夈€?
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

fail() { echo "閿欒锛?1" >&2; exit 1; }

[ "$(id -u)" = "0" ] || fail "蹇呴』鍦?root 涓嬭繍琛岋紙鍗曡疆鑴氭湰瑕?mlockall + SCHED_FIFO锛?
DEPLOYMENT="${DEPLOYMENT:-${EMASTER_DEPLOYMENT:-}}"
[ -n "$DEPLOYMENT" ] || fail "娌℃湁缁?DEPLOYMENT锛堜篃涓嶈鐢ㄩ粯璁ゅ€尖€斺€旈粯璁ゅ€肩瓑浜庢妸鏌愪釜鍙版灦鍐欒繘宸ュ叿閲岋級"
SERIES="${SERIES:-}"
[ -n "$SERIES" ] || fail "娌℃湁缁?SERIES锛堝畠鍐冲畾璇佹嵁鐩綍 reports/_<SERIES>/锛?
ROUNDS="${ROUNDS:-6}"
GAP="${GAP:-60}"
MODE="${MODE:-reciprocate}"

case "$MODE" in
    idle)        ROUND_SCRIPT="$REPO/scripts/bench_idle.sh" ;;
    reciprocate) ROUND_SCRIPT="$REPO/scripts/bench_reciprocate_30deg.sh" ;;
    *)           fail "涓嶈璇?MODE=$MODE锛坕dle | reciprocate锛? ;;
esac
# 鏌ュ瓨鍦ㄤ笌鍙锛?*涓嶆煡鍙墽琛屼綅**锛氭湰浠撳簱 scripts/ 涓嬬殑鑴氭湰涓€寰嬩互 `bash <鑴氭湰>` 璋冪敤锛?# 绱㈠紩閲屼篃閮芥槸 100644锛堝彧鏈夋瀬灏戞暟琚埆鐨勮剼鏈敤 [ -x ] 鏌ヨ繃鐨勬墠甯﹀彲鎵ц浣嶏級銆?# 杩欓噷鑻ユ煡 -x锛屽氨闇€瑕佹瘡涓崟杞剼鏈澶栧甫涓€涓潈闄愪綅锛屼笌浠撳簱绾﹀畾鐩稿弽銆?[ -r "$ROUND_SCRIPT" ] || fail "鍗曡疆鑴氭湰涓嶅瓨鍦ㄦ垨涓嶅彲璇伙細$ROUND_SCRIPT"

# 鎶ュ憡璺緞鎸?deployment_id 浠庨厤缃噷鎵撅紝涓嶆寜鏂囦欢鍚嶇寽锛堟枃浠跺悕鐢ㄤ笅鍒掔嚎銆両D 鐢ㄨ繛瀛楃锛夛紝
# 涔熶笉鍐欐鈥斺€旀崲閮ㄧ讲鏃跺繕浜嗗悓姝ヤ細璇诲畬涓€浠藉埆澶勭殑鏃ф姤鍛婂嵈鐪嬩笉鍑哄紓甯搞€?# 杩欎竴娈典笌 tools/verify_p25_motion.sh 鍚屼竴鏉¤鍒欍€?REPORT_REL=$(DEPLOYMENT="$DEPLOYMENT" python3 -c "
import glob, json, os, sys
target = os.environ['DEPLOYMENT']
for path in sorted(glob.glob(os.path.join('$REPO', 'config', 'deployments', '*.json'))):
    with open(path, encoding='utf-8') as handle:
        if json.load(handle).get('deployment_id') == target:
            print(json.load(open(path, encoding='utf-8'))['run_report_path'])
            sys.exit(0)
sys.exit(1)
" 2>/dev/null)
[ -n "$REPORT_REL" ] || fail "璇讳笉鍑洪儴缃?$DEPLOYMENT 鐨?run_report_path"
REPORT="$REPO/$REPORT_REL"
REPORT_DIR="$(dirname "$REPORT")"
ARCHIVE_DIR="$REPORT_DIR/archive"

EVIDENCE="$REPO/reports/_$SERIES"
mkdir -p "$EVIDENCE"
INDEX="$EVIDENCE/index.tsv"
if [ ! -f "$INDEX" ]; then
    # 琛ㄥご = 鏈剼鏈嚜宸遍偅鍑犲垪 + report_row.py 鐨勫垪锛堜袱澶勫悇鍐欏悇鐨勪細婕傦紝鎵€浠ュ悎鎴愪竴娆★級銆?    printf 'series\tdeployment\tmode\tround_at\tgap_s\texit_code\t%s\n' \
        "$(python3 "$REPO/analysis/report_row.py" --header)" > "$INDEX"
fi

echo "绯诲垪=$SERIES 閮ㄧ讲=$DEPLOYMENT 妯″紡=$MODE 杞暟=$ROUNDS 闂撮殧=${GAP}s"
echo "鍗曡疆鑴氭湰=$ROUND_SCRIPT"
echo "璇佹嵁鐩綍=$EVIDENCE"
echo "鎶ュ憡=$REPORT"
echo

for round_index in $(seq 1 "$ROUNDS"); do
    TAG="$(printf '%sr%02d' "$SERIES" "$round_index")"
    ROUND_AT="$(date '+%Y-%m-%d %H:%M:%S')"
    ROUND_DIR="$EVIDENCE/$(printf 'r%02d' "$round_index")_$TAG"
    mkdir -p "$ROUND_DIR"

    # 鏉′欢璁板綍銆備簨鍚庢瘮杈冧袱杞椂锛岀涓€姝ュ氨鏄‘瀹氬畠浠殑宸埆鍦ㄥ摢锛涜€岃疆娆℃槸鑴氭湰寰幆鍑烘潵鐨勶紝
    # 鍛戒护琛屽巻鍙查噷鍙湁涓€鏉″懡浠わ紝鐪嬩笉鍑烘瘡涓€杞悇鑷殑鏉′欢銆?    {
        echo "series=$SERIES"
        echo "round=$round_index"
        echo "tag=$TAG"
        echo "started_at=$ROUND_AT"
        echo "deployment=$DEPLOYMENT"
        echo "mode=$MODE"
        echo "gap_s=$GAP"
        echo "rounds=$ROUNDS"
        echo "round_script=$ROUND_SCRIPT"
        # 鍏朵綑鏉′欢鐢卞崟杞剼鏈户鎵匡紝杩欓噷鐓ф牱璁颁竴浠斤細瀹冧滑鏄?杩愬姩褰㈡€?鐨勫叏閮ㄥ弬鏁般€?        for name in DURATION IDLE TRAVERSE DEGREES RATE WAVE ONLY_AXIS \
                    REPORT_INTERVAL OBSERVATION SETTLE_TICKS; do
            value="${!name:-}"
            [ -n "$value" ] && echo "$(printf '%s' "$name" | tr 'A-Z' 'a-z')=$value"
        done
    } > "$ROUND_DIR/condition.txt"

    # 瑁呯疆韬唤锛氳繖涓€杞窇鐨勬槸浠€涔堜唬鐮併€佷粈涔堥厤缃€佹€荤嚎涓婃彃鐨勬槸鍝嚑鍙般€?    # 娌℃湁瀹冿紝璺ㄨ疆娆′笌璺ㄥぉ鐨勬瘮杈冧笉鎴愮珛銆傞噰闆嗗け璐ュ彧鍐欎竴琛岃鏄庯紝涓嶈鏁磋疆鍋滀笅 鈥斺€?    # 涓€涓淳鐢熸枃浠朵笉璇ユ嫋鍨竴杞帰鏌ャ€?    python3 "$REPO/scripts/capture_identity.py" "$ROUND_DIR/device-identity.txt" \
        --deployment "$DEPLOYMENT" >/dev/null 2>&1 ||
        echo "capture_error=閲囬泦澶辫触锛堟鏃?device-identity.txt 涓嶅彲鐢紝璇ヨ疆鐨勮法杞瘮杈冧笉鎴愮珛锛? \
            > "$ROUND_DIR/device-identity.txt"

    # 缃戝崱鏀跺彂璁℃暟锛氳疆鍓嶅揩鐓с€?*杩欐槸涓荤珯杞欢涔嬪鐨勫敮涓€瑙傛祴鐐?*锛岀敤鏉ュ垎杈?    # "甯ф病鍥炴潵"锛堥摼璺?浠庣珯锛変笌"甯у洖鏉ヤ簡涓荤珯娌¤"锛堜富鏈猴級銆傛帴鍙ｅ悕浠庝笂闈㈤偅浠?    # device-identity.txt 閲屽彇锛屼笉鍦ㄨ繖閲岄噸澶嶈В鏋愰厤缃€斺€斾袱澶勫悇瑙ｆ瀽涓€娆★紝灏嗘潵鏀归厤缃?    # 灏变細鏈変竴澶勫繕鏀广€?    NIC_IFACE="$(sed -n 's/^interface=//p' "$ROUND_DIR/device-identity.txt" 2>/dev/null)"
    if [ -n "$NIC_IFACE" ] && [ "$NIC_IFACE" != "unknown" ]; then
        python3 "$REPO/scripts/nic_counters.py" "$ROUND_DIR/nic-before.txt" \
            --interface "$NIC_IFACE" >/dev/null 2>&1 || true
    fi

    # 鐜板満璁板綍妯℃澘銆傛帰鏌ョ殑缁撹缁忓父鏉ヨ嚜"娌＄湅鎳傜殑"閭ｅ嚑鏉★紝缁欏畠涓€涓浐瀹氱殑鍦版柟锛?    # 鍏嶅緱瀹冧滑鍙暀鍦ㄧ粓绔粴鍔ㄩ噷銆?    [ -f "$ROUND_DIR/notes.md" ] || cat > "$ROUND_DIR/notes.md" <<'NOTES'
# 瑙傚療璁板綍

## 鐜板満

## 娌＄湅鎳傜殑

NOTES

    echo "===== 绗?$round_index/$ROUNDS 杞?$TAG 寮€濮?$ROUND_AT ====="

    # 鍒犳帀鏈疆鐨勬棫浜х墿锛岄伩鍏?璇诲埌浜嗕笂涓€杞殑 done"杩欑鍋囬€氳繃銆?    rm -f "/tmp/${TAG}_done" "/tmp/${TAG}_master.log" \
          "/tmp/${TAG}_outcome.txt" "/tmp/${TAG}_client.log"

    TAG="$TAG" bash "$ROUND_SCRIPT" >/dev/null 2>&1
    EXIT_CODE=$?
    #
    # 浠ュ崟杞剼鏈嚜宸卞啓涓嬬殑 _done 涓哄噯锛?? 鍙綔鍏滃簳銆?    # 鐞嗙敱锛氬崟杞剼鏈殑 finish() 涓€搴﹀彧 echo 涓?exit锛屼簬鏄?璺戞弧锛忔湁绌烘。锛忚涓荤珯涓柇锛?    # shutdown 娌″仠浣?鍥涚缁撳眬鍦?$? 閲屽叏鏄?0锛岃€?index.tsv 鐨?exit_code 鍒楀彇鐨勫氨鏄?$?
    # 鈥斺€旀敹灏炬帀杞撮偅绉嶈疆娆′細鏄剧ず鎴愬共骞插噣鍑€鐨勪竴杞紙2026-10-08 瀹炴祴鍒颁竴娆★級銆?    # finish() 宸茬粡琛ヤ笂 exit锛涜繖閲屽啀浠?_done 涓哄噯锛屾槸鍥犱负閭ｄ釜鏂囦欢鏄?*鍒ゅ畾鍙戠敓鐨勫綋鍦?*
    # 鍐欎笅鐨勶紝姣旇皟鐢ㄦ柟璇诲埌鐨?$? 鏇存帴杩戜簨瀹烇紝涔熻兘鎸′綇灏嗘潵鍐嶆湁浜烘紡鍐?exit銆?    # 鏈疆寮€濮嬫椂 _done 宸茶鍒犳帀锛屾墍浠ユ枃浠朵笉瀛樺湪灏辫鏄庤繖涓€杞病璧板埌鍒ゅ畾锛屾鏃剁敤 $?銆?    if [ -f "/tmp/${TAG}_done" ]; then
        EXIT_CODE="$(cat "/tmp/${TAG}_done")"
    fi
    echo "  鍗曡疆鑴氭湰閫€鍑虹爜=$EXIT_CODE锛堣鑴氭湰澶撮儴鐨勫惈涔夎〃锛?

    for artifact in master.log outcome.txt client.log; do
        [ -f "/tmp/${TAG}_$artifact" ] && cp -f "/tmp/${TAG}_$artifact" "$ROUND_DIR/"
    done
    [ -f "/tmp/${TAG}_done" ] && cp -f "/tmp/${TAG}_done" "$ROUND_DIR/exit-code.txt"

    # 缃戝崱鏀跺彂璁℃暟锛氳疆鍚庡揩鐓?+ 宸垎銆傚樊鍒嗘妸"杩欎竴杞湁娌℃湁甯ф病鍥炲埌缃戝崱"鍐欐垚涓€琛岋紝
    # 鑰屽畠姝ｆ槸鍒嗚鲸涓绘満渚т笌閾捐矾渚х殑閭ｄ竴鍒€銆?    if [ -n "$NIC_IFACE" ] && [ "$NIC_IFACE" != "unknown" ]; then
        python3 "$REPO/scripts/nic_counters.py" "$ROUND_DIR/nic-after.txt" \
            --interface "$NIC_IFACE" >/dev/null 2>&1 || true
        python3 "$REPO/scripts/nic_counters.py" --diff "$ROUND_DIR/nic-before.txt" \
            "$ROUND_DIR/nic-after.txt" > "$ROUND_DIR/nic-diff.txt" 2>&1 || true
    fi

    # 寮傚父琛細鎶婁富绔欐棩蹇楅噷宸茬粡鏈夌殑鍧愭爣鍥哄畾鎴愪竴寮犲彲璺ㄨ疆褰掑苟鐨勮〃銆傚彧璇绘棩蹇楋紝鍥犳蹇笖
    # 蹇呯劧鍙敤锛堟姤鍛婂彲鑳藉洜纾佺洏鎴栧穿婧冭€岀己澶憋級銆傛棩蹇椾笉鍦ㄥ氨璺宠繃锛屼笉璁╂淳鐢熸枃浠舵嫋鍨竴杞€?    if [ -f "/tmp/${TAG}_master.log" ]; then
        python3 "$REPO/analysis/anomalies.py" "/tmp/${TAG}_master.log" \
            -o "$ROUND_DIR/anomalies.tsv" --quiet 2>>"$ROUND_DIR/notes.md" || true
    fi

    # 瀹屾暣鎶ュ憡鏄鐩栧紡鐨勶細鍏堟妸杩欎竴杞搴旂殑褰掓。鏂囦欢鍚嶈涓嬫潵锛堥偅鏄畬鏁磋瘉鎹殑鍧愭爣锛夛紝
    # 鍐嶅**褰撳墠**閭ｄ唤鎶ュ憡鍑烘憳瑕佲€斺€斾袱鑰呴兘灏忥紝鑳藉叆搴撱€?    ARCHIVE_NAME="-"
    if [ -d "$ARCHIVE_DIR" ]; then
        ARCHIVE_NAME="$(ls -t "$ARCHIVE_DIR" 2>/dev/null | head -1)"
    fi
    printf '%s\n' "$ARCHIVE_NAME" > "$ROUND_DIR/report-name.txt"

    ROW="-"
    if [ -f "$REPORT" ]; then
        python3 "$REPO/analysis/report_summary.py" "$REPORT" \
            > "$ROUND_DIR/summary.txt" 2>&1
        ROW="$(python3 "$REPO/analysis/report_row.py" "$REPORT" "$TAG" 2>/dev/null)"
        printf '%s\n' "$ROW" > "$ROUND_DIR/row.tsv"
    else
        echo "  杩欎竴杞病鏈夋姤鍛婏紙涓荤珯娌¤捣鏉ユ垨娌″啓锛? | tee "$ROUND_DIR/summary.txt"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$SERIES" "$DEPLOYMENT" "$MODE" "$ROUND_AT" "$GAP" "$EXIT_CODE" "$ROW" >> "$INDEX"
    echo "  璇佹嵁=$ROUND_DIR"

    if [ "$round_index" -lt "$ROUNDS" ]; then
        echo "  杞棿绛夊緟 ${GAP}s锛?(date '+%H:%M:%S')锛?
        sleep "$GAP"
    fi
    echo
done

echo "鍏ㄩ儴 $ROUNDS 杞粨鏉熴€傛眹鎬伙細$INDEX"
echo "**娉ㄦ剰**锛氶€€鍑虹爜 0 鍙〃绀鸿疆娆¤窇瀹屼簡锛屼笉浠ｈ〃姣忚疆閮芥垚鍔熲€斺€旀垚璐ョ湅 index.tsv銆?
exit 0
