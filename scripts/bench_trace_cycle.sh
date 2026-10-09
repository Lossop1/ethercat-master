#!/bin/bash
# 鍛ㄦ湡绾跨▼鐨?璋佹嫋鎱簡杩欎竴鎷?鍙栬瘉閰嶆柟锛氳窇涓€杞彴鏋惰繍琛岋紝鍚屾椂鐢?ftrace 鎶?# 鐢ㄦ埛鎬佹帰閽?+ 璋冨害/涓柇 + 绯荤粺璋冪敤璁颁笅鏉ャ€?#
# 瑕佸洖绛旂殑闂鏄袱绉嶅畬鍏ㄤ笉鍚岀殑鍙兘锛?#   1) 涓荤珯鑷繁鎱?鈥斺€?杩欐娲绘湰鏉ュ氨瑕侀偅涔堜箙锛?#   2) 绾跨▼琚嬁璧?鈥斺€?娲讳笉澶氾紝浣?CPU 涓嶅湪瀹冩墜閲屻€?# 涓よ€呯殑鏃堕棿绾块暱寰椾笉涓€鏍凤紝鎵€浠ヨ褰曢噷鍚屾椂瑕佹湁"瀹冨湪璺?鍜?瀹冩病鍦ㄨ窇"涓ら潰鐨勮瘉鎹€?#
# 璁颁笁鏍蜂笢瑗匡細
#   a) uprobe锛氬洓涓敤鎴锋€佸嚱鏁板叆鍙ｅ悇涓€涓偣锛屾妸鍛ㄦ湡鎷嗘垚鍑犳銆?#        浜ゆ崲鍏ュ彛 鈫?璁℃椂缁熻鍏ュ彛锛? 鍙?鏀?閭/璁℃暟鍣ㄥ洓娈碉級
#        璁℃椂缁熻 鈫?PDO 瀹¤杈撳嚭锛? 鐜板満鍏ョ幆閭ｄ竴娈碉級
#        瀹¤杈撳嚭 鈫?涓嬩竴涓氦鎹㈠叆鍙ｏ紙= 鎺㈤拡 + WKC 绛栫暐 + 鎴鏈熷垽瀹?+ 鏀跺熬锛?#      鍝竴娈靛嚭闂锛岀湅璁板綍閲屼袱鐐圭殑闂撮殧灏辩煡閬擄紝涓嶇敤鏀逛唬鐮佸姞璁℃椂鐐广€?#      娉ㄥ唽鐢ㄧ殑鏂囦欢鍋忕Щ鐢?analysis/uprobe_offsets.py 浠?ELF 鎹㈢畻
#      锛坲probe 瑕佺殑鏄枃浠跺亸绉伙紝`nm` 缁欑殑鏄櫄鎷熷湴鍧€锛屾瀵归綈涓嶉綈鏃朵袱鑰呬笉绛夛級銆?#   b) sched_switch / sched_waking 鈥斺€?绾跨▼浠€涔堟椂鍊欏湪 CPU 涓娿€佷粈涔堟椂鍊欒鎹笅鍘汇€?#   c) irq_handler_* / softirq_* / raw_syscalls 鈥斺€?鎶㈠崰瀹冪殑鏄‖涓柇銆佽蒋涓柇锛?#      杩樻槸瀹冭嚜宸卞彂璧风殑绯荤粺璋冪敤銆俢lock_gettime 鍦?arm64 涓婅蛋 vDSO锛屾湰鏉ヤ笉璇?#      鍑虹幇绯荤粺璋冪敤锛涘畠瑕佹槸鍑虹幇浜嗭紝鏈韩灏辨槸缁撹銆?#
# 璺戝畬鐢?analysis/trace_busy_windows.py 璇荤粨鏋滐細鎶?涓€娆¤繛缁崰鐢?CPU
# 瓒呰繃 N ms"鐨勭獥鍙ｆ憡寮€锛屼簨浠跺甫浜鸿瘽鏍囩銆?#
# 鍙傛暟锛堢幆澧冨彉閲忥級锛?#   TAG       鏈疆鏍囩锛屽喅瀹?/tmp/<TAG>_trace.txt 绛夋枃浠跺悕锛岄粯璁?trace
#   DURATION  杩愯绉掓暟锛岄粯璁?180
#   DEGREES   寰€澶嶅箙搴︼紝榛樿 0锛堜笉鍔ㄨ酱锛夈€?#             涓嶅姩杞存槸鏁呮剰鐨勶細璇佹嵁鎸囧悜"鍋滄憜缁撴潫杩愯"锛屼笉鏄?杩愯缁撴潫瑙﹀彂鍋滄憜"锛?#             鎵€浠ヨ酱杞笉杞笌鍋滄憜鏃犲叧锛涗笉杞渷鐢点€佷笉鍙戠儹锛? 鍙蜂粠绔欎篃涓嶇敤鍙楃儹銆?#   CPU       鍙姄鍝竴鏍革紝榛樿 11銆?*蹇呴』涓庝富绔欓儴缃查噷鍛ㄦ湡绾跨▼鐨?RT 缁戝畾涓€鑷?*锛?#             鍚﹀垯璁板綍閲屾病鏈夊懆鏈熺嚎绋嬨€?#   BUFFER_MB 缁欒繖涓€鏍稿澶х殑鐜紝榛樿 2048銆?*鐜殑澶у皬鎸?瑕佽兘鍥炵湅澶氶暱鐨勭獥鍙?瀹氾紝
#             涓嶆寜鏈哄櫒瀹?*锛氭湰鍙版灦瀹炴祴浜嬩欢瀵嗗害绾?4.7 MB/s锛?048 MB 绾﹁ 7 鍒嗛挓锛?#             澶熻鐩?180 s 鏁磋疆杩樻湁浣欓噺銆傜幆鏄鐩栧啓锛屽啓婊′簡涓?*鏈€鑰佺殑**鈥斺€旀墍浠ョ粰
#             澶皬涓㈢殑鏄?寮€澶?锛屽仠鏈洪偅涓€鎷嶏紙鎬诲湪鏈€鍚庯級姘歌繙鐣欏緱浣忋€傛満鍣ㄥ唴瀛樹笉澶熸椂
#             鍐呮牳浼氭嫆缁濆啓鍏ュ苟淇濈暀鍘熷€硷紝鑴氭湰鍥犳蹇呴』鎶?瀹為檯鎷垮埌澶氬ぇ銆佽褰曞疄闄呰鐩?#             浜嗗闀?閮芥墦鍑烘潵锛涘彧鐪嬫湁娌℃湁鎶ラ敊鏄笉澶熺殑銆?#
# 闇€瑕?root锛氳鍦?tracefs 閲屾敞鍐屾帰閽堛€佹敼缂撳啿銆傜敤娉曡 analysis/ 涓嬬殑
# 涓や釜鑴氭湰澶撮儴銆?set -u

T=/sys/kernel/debug/tracing
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$REPO/build/apps/master/emaster-master
TAG=${TAG:-trace}
TRACE=/tmp/${TAG}_trace.txt
RUNLOG=/tmp/${TAG}_run.txt
OFF=/tmp/${TAG}_off.txt
DURATION=${DURATION:-180}
DEGREES=${DEGREES:-0}
CPU=${CPU:-11}
BUFFER_MB=${BUFFER_MB:-2048}

rm -f /tmp/${TAG}_done /tmp/${TAG}_rc "$TRACE"
: > "$RUNLOG"

fail() {
    echo "澶辫触锛?1" | tee -a "$RUNLOG"
    echo 1 > /tmp/${TAG}_rc
    touch /tmp/${TAG}_done
    exit 1
}

echo "== 寮€璁板綍 $(date '+%F %T') ==" >> "$RUNLOG"
echo 0 > "$T/tracing_on"

# 涓婁竴杞暀涓嬬殑鎺㈤拡瑕佸厛鎽樻帀锛屽惁鍒欓噸鍚嶄細璁╄繖娆＄殑娉ㄥ唽澶辫触銆?echo > "$T/uprobe_events" 2>/dev/null

python3 "$REPO/analysis/uprobe_offsets.py" "$BIN" \
    emaster_soem_session_exchange \
    emaster_cyclic_timing_stats_record \
    emaster_cia_process_image_audit_output \
    emaster_command_server_receive > "$OFF" 2>&1 || fail "绗﹀彿鎹㈢畻鑴氭湰娌¤窇璧锋潵"
cat "$OFF" >> "$RUNLOG"
grep -q MISSING "$OFF" && fail "鏈夌鍙锋病鎵惧埌锛堣 $OFF锛?

offset_of() { awk -v n="$1" '$1 == n { print $2 }' "$OFF"; }
EXCH=$(offset_of emaster_soem_session_exchange)
TSTAT=$(offset_of emaster_cyclic_timing_stats_record)
AUDIT=$(offset_of emaster_cia_process_image_audit_output)
CMDRECV=$(offset_of emaster_command_server_receive)
for pair in "EXCH:$EXCH" "TSTAT:$TSTAT" "AUDIT:$AUDIT" "CMDRECV:$CMDRECV"; do
    [ -n "${pair#*:}" ] || fail "鍋忕Щ涓虹┖锛?pair"
done

echo "p:cyc_exch  $BIN:$EXCH"  >> "$T/uprobe_events"
echo "p:cyc_tstat $BIN:$TSTAT" >> "$T/uprobe_events"
echo "p:cyc_audit $BIN:$AUDIT" >> "$T/uprobe_events"
echo "p:cyc_cmd   $BIN:$CMDRECV" >> "$T/uprobe_events"
{
    echo "== 娉ㄥ唽鐨勬帰閽?=="
    cat "$T/uprobe_events"
} >> "$RUNLOG" 2>&1

for group in cyc_exch cyc_tstat cyc_audit cyc_cmd; do
    [ -e "$T/events/uprobes/$group/enable" ] || fail "鎺㈤拡 $group 娌″缓鍑烘潵锛堣 $RUNLOG锛?
    echo 1 > "$T/events/uprobes/$group/enable"
done

for event in \
    sched/sched_switch sched/sched_waking \
    irq/irq_handler_entry irq/irq_handler_exit \
    irq/softirq_entry irq/softirq_exit \
    raw_syscalls/sys_enter raw_syscalls/sys_exit; do
    [ -e "$T/events/$event/enable" ] || fail "浜嬩欢 $event 涓嶅瓨鍦?
    echo 1 > "$T/events/$event/enable"
done

: > "$T/trace"
echo mono > "$T/trace_clock"

# 鍙姄鍛ㄦ湡绾跨▼閭ｄ竴鏍搞€傚啓姝昏繖涓€鍙ユ槸鏈夋剰鐨勶細tracefs 鐨勮缃細涓€鐩寸暀鐫€锛屼笉鏄惧紡璁惧氨鏄?# 鍦ㄨ祵涓婁竴娆¤繍琛岀暀涓嬬殑鍊兼濂藉銆?printf '%x' $((1 << CPU)) > "$T/tracing_cpumask"

# buffer_size_kb 鏄?*姣?CPU 涓€浠?*锛屾暣鏈哄崰鐢?= 鏈€?脳 鏍告暟锛坄buffer_total_size_kb` 灏辨槸
# 涔樺嚭鏉ョ殑閭ｄ釜鏁帮級銆傛墍浠?鑳界粰澶氬ぇ"鐢辫繖鍙版満鍣ㄧ殑鍐呭瓨鍐冲畾锛?*娌℃湁涓€涓浐瀹氱殑鍐呮牳涓婇檺**锛?# 鏈彴鏋讹紙12 鏍?/ 32 GB锛夊啓 1 GB/鏍?鎴愬姛銆? GB/鏍?涔熸垚鍔燂紝鍙湁鍐?2 GB/鏍?**脳12** 鎵?ENOMEM銆?# 鏃㈢劧鍙姄涓€鏍革紝灏辨病蹇呰缁?12 浠介兘鍔犲ぇ鈥斺€旇€佸啓娉曞線鍏ㄥ眬鏂囦欢鍐欙紝鍦?12 鏍镐笂绛変簬鎶婂唴瀛樹箻 12 鍊?# 锛堝疄娴嬩竴娆?1 GB/鏍?鐨勫叏灞€鍐欏悆鎺?12 GB锛夈€傝繖閲屽彧鏀规姄鐨勯偅涓€鏍革紝鍐欎笉杩涘幓鍐嶉€€鍥炲叏灞€銆?# 鍐欎笉杩涘幓涓嶆槸鑷村懡閿欒锛堝唴鏍镐細鎷掔粷骞朵繚鐣欏師鍊硷紝涓嶄細鍐欏潖锛夛紝浣?*涓嶈兘鎮勬倓杩囧幓**锛?# 鐜皬浜嗗彧浼氫涪寮€澶达紝浜嬪悗璇昏褰曠殑浜烘棤浠庣煡閬?寮€澶?鍒板簳鍦ㄤ笉鍦紝寰堝鏄撴妸涓€涓鎴柇鐨?# 璁板綍褰撴垚瀹屾暣鐨勬潵鐪嬨€傛墍浠ヨ繖閲屽啓涓嶆垚瑕佸湪鏃ュ織閲屾樉寮忔爣鍑烘潵锛堝叏灞€鏂囦欢鍦ㄥ悇鏍镐笉涓€鑷存椂
# 浼氳鎴?`X`锛岀湅鍒?`X` 灞炴甯革級銆?BUFFER_REQUESTED=$((BUFFER_MB * 1024))
BUFFER_SHORT=0
if ! echo "$BUFFER_REQUESTED" > "$T/per_cpu/cpu$CPU/buffer_size_kb" 2>/dev/null; then
    if ! echo "$BUFFER_REQUESTED" > "$T/buffer_size_kb" 2>/dev/null; then
        echo "锛侊紒缂撳啿鍐欏叆澶辫触锛堣姹?${BUFFER_MB} MB锛夆€斺€斿唴鏍告嫆缁濅簡锛屾部鐢ㄥ師鍊硷紱" >> "$RUNLOG"
        echo "锛侊紒杩欏彴鏈哄櫒鐨勭┖闂插唴瀛樹笉澶熴€傝褰曚粛浼氬啓锛屼絾鍙兘鍙鐩栬繍琛岀殑鏈€鍚庝竴娈点€? >> "$RUNLOG"
        BUFFER_SHORT=1
    fi
fi
BUFFER_GOT=$(cat "$T/per_cpu/cpu$CPU/buffer_size_kb" 2>/dev/null)
if [ "$BUFFER_SHORT" = 0 ] && [ -n "$BUFFER_GOT" ] && \
   [ "$BUFFER_GOT" -lt $((BUFFER_REQUESTED * 3 / 4)) ] 2>/dev/null; then
    echo "锛侊紒缂撳啿瀹為檯鍙湁 ${BUFFER_GOT} kB锛屼笉鍒拌姹傚€?${BUFFER_REQUESTED} kB 鐨?3/4銆? >> "$RUNLOG"
    BUFFER_SHORT=1
fi
{
    echo "== 閰嶇疆 =="
    echo -n "tracing_cpumask="; cat "$T/tracing_cpumask"
    echo -n "cpu$CPU/buffer_size_kb="; cat "$T/per_cpu/cpu$CPU/buffer_size_kb"
    echo -n "buffer_size_kb="; cat "$T/buffer_size_kb"
    echo -n "buffer_total_size_kb="; cat "$T/buffer_total_size_kb"
    echo -n "trace_clock="; cat "$T/trace_clock"
    echo "TAG=$TAG DURATION=$DURATION DEGREES=$DEGREES CPU=$CPU BUFFER_MB=$BUFFER_MB"
} >> "$RUNLOG"
echo 1 > "$T/tracing_on"

cd "$REPO" || fail "杩涗笉鍘讳粨搴撶洰褰?
DURATION="$DURATION" DEGREES="$DEGREES" TRAVERSE=3 RATE=100 WAVE=triangle TAG="$TAG" \
    bash scripts/bench_reciprocate_30deg.sh
echo "$?" > /tmp/${TAG}_rc

echo 0 > "$T/tracing_on"
cat "$T/trace" > "$TRACE"
for group in cyc_exch cyc_tstat cyc_audit cyc_cmd; do
    echo 0 > "$T/events/uprobes/$group/enable"
done
echo > "$T/uprobe_events" 2>/dev/null

# 璁板綍瀹為檯瑕嗙洊浜嗗闀跨殑鏃堕棿銆傜幆鏄鐩栧啓锛岃繖涓暟鏄庢樉灏忎簬 DURATION 灏辫鏄庡紑澶磋涓簡鈥斺€?# 杩欐槸鍒ゆ柇"缂撳啿澶熶笉澶?鍞竴鍙潬鐨勪緷鎹紙鍐欑紦鍐叉椂涓嶆姤閿欎笉绛変簬澶熺敤锛夈€?SPAN=$(awk '{
    line = $4; sub(/:$/, "", line);
    if (line !~ /^[0-9]+\.[0-9]+$/) next;
    split(line, p, "."); v = p[1] + p[2] / 1000000;
    if (n == 0) first = v;
    last = v; n++;
} END { if (n > 0) printf "%.1f", last - first; else printf "0" }' "$TRACE")

{
    echo "== 鏀惰褰?$(date '+%F %T') =="
    echo -n "閫€鍑虹爜="; cat /tmp/${TAG}_rc
    echo -n "璁板綍璺ㄥ害绉?"; echo "$SPAN"
    echo -n "璁板綍琛屾暟="; wc -l < "$TRACE"
    echo -n "璁板綍瀛楄妭="; stat -c %s "$TRACE"
    echo -n "涓㈠純浜嬩欢琛?"; grep -c "events dropped" "$TRACE" || true
    echo -n "uprobe 鍛戒腑 cyc_exch="; grep -c "cyc_exch:" "$TRACE" || true
    echo -n "uprobe 鍛戒腑 cyc_tstat="; grep -c "cyc_tstat:" "$TRACE" || true
    echo -n "uprobe 鍛戒腑 cyc_audit="; grep -c "cyc_audit:" "$TRACE" || true
    echo -n "uprobe 鍛戒腑 cyc_cmd="; grep -c "cyc_cmd:" "$TRACE" || true
    echo -n "raw_syscalls 琛屾暟="; grep -c "sys_enter\|sys_exit" "$TRACE" || true
    if awk -v a="$SPAN" -v b="$DURATION" 'BEGIN { exit !(a < b - 5) }'; then
        echo "锛侊紒璁板綍鍙鐩栦簡 ${SPAN} s锛岃€岃繖杞窇浜?${DURATION} s鈥斺€斿紑澶磋鐜鐩栨帀浜嗐€?
        echo "锛侊紒鍙棶銆屾渶鍚庝竴娈靛彂鐢熶簡浠€涔堛€嶇殑鍒ゆ嵁涓嶅彈褰卞搷锛堝仠鏈洪偅涓€鎷嶆€诲湪鏈€鍚庯級锛?
        echo "锛侊紒闂€屼竴寮€濮嬫€庢牱銆嶇殑蹇呴』鎶?BUFFER_MB 璋冨ぇ閲嶈窇锛屼笉鑳芥嬁杩欎唤璁板綍涓嬬粨璁恒€?
    fi
} >> "$RUNLOG" 2>&1
touch /tmp/${TAG}_done
